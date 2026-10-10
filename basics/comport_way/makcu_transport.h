// Транспорт команд MOVE в MAKCU по COM.
//
//  RawComTransport  - собственный overlapped-handle порта (горячий путь).
//      async: WriteFile(OVERLAPPED) "выстрелил и забыл", кольцо слотов,
//             поток тайминга никогда не блокируется на USB.
//      sync : та же запись, но с ожиданием завершения (для сравнения).
//  SdkTransport     - makxd::Device::mouseMove() в отдельном потоке
//      (эталон SDK; дельты копятся в почтовом ящике, сумма не теряется).
//
// Почему не SDK в горячем пути: SDK открывает порт синхронно, а его поток-
// слушатель и монитор соединения обращаются к тому же handle; Windows
// сериализует I/O на синхронном handle, и mouseMove() измеренно блокируется
// p50 ~0.9 мс, p99 ~19 мс. Для 1000 Гц это неприемлемо.
#pragma once

#include "mover_common.h"

#include <makxd.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace mover {

// MAK_API: DE AD | LEN:u16 | CMD:u8 | PAYLOAD
inline std::array<uint8_t, 9> buildMoveFrame(int16_t dx, int16_t dy) {
    const auto ux = static_cast<uint16_t>(dx);
    const auto uy = static_cast<uint16_t>(dy);
    return {0xDE, 0xAD, 0x04, 0x00,
            static_cast<uint8_t>(makxd::ApiOpcode::MOVE),
            static_cast<uint8_t>(ux & 0xFF), static_cast<uint8_t>(ux >> 8),
            static_cast<uint8_t>(uy & 0xFF), static_cast<uint8_t>(uy >> 8)};
}

struct TransportStats {
    std::atomic<uint64_t> writes{0};        // команд реально отправлено в драйвер
    std::atomic<uint64_t> deferred{0};      // тик пропущен из-за занятого кольца (дельта перенесена)
    std::atomic<uint64_t> errors{0};
    std::atomic<uint32_t> maxInflight{0};
    std::atomic<double> maxCallUs{0.0};     // самый долгий вызов отправки
    std::atomic<double> sumCallUs{0.0};

    void reset() {
        writes = 0;
        deferred = 0;
        errors = 0;
        maxInflight = 0;
        maxCallUs = 0.0;
        sumCallUs = 0.0;
    }
    void noteCall(double us) {
        sumCallUs.store(sumCallUs.load(std::memory_order_relaxed) + us, std::memory_order_relaxed);
        if (us > maxCallUs.load(std::memory_order_relaxed)) {
            maxCallUs.store(us, std::memory_order_relaxed);
        }
    }
};

class Transport {
public:
    virtual ~Transport() = default;
    [[nodiscard]] virtual const char* name() const = 0;
    // true  - команда принята (отправлена/поставлена в очередь);
    // false - сейчас отправить нельзя, вызывающий сохраняет дельту у себя.
    [[nodiscard]] virtual bool move(int16_t dx, int16_t dy) = 0;
    [[nodiscard]] virtual bool healthy() const = 0;
    TransportStats stats;
};

// ===========================================================================
// RawComTransport
// ===========================================================================
class RawComTransport final : public Transport {
public:
    enum class Mode { Async, Sync };

    RawComTransport() {
        for (auto& slot : ring_) {
            slot.ov.hEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        }
    }
    ~RawComTransport() override {
        close();
        for (auto& slot : ring_) {
            if (slot.ov.hEvent) {
                CloseHandle(slot.ov.hEvent);
            }
        }
    }

    [[nodiscard]] const char* name() const override {
        if (kmText_) return mode_ == Mode::Async ? "km-async" : "km-sync";
        return mode_ == Mode::Async ? "raw-async" : "raw-sync";
    }
    [[nodiscard]] bool healthy() const override { return handle_ != INVALID_HANDLE_VALUE && !failed_; }
    [[nodiscard]] bool isOpen() const { return handle_ != INVALID_HANDLE_VALUE; }
    void setMode(Mode mode) { mode_ = mode; }
    // true - слать текстовый KM_API "km.move(x,y)" вместо бинарного MAK_API 0x18.
    void setKmText(bool enabled) { kmText_ = enabled; }

    [[nodiscard]] uint32_t baud() const { return baud_; }
    [[nodiscard]] std::optional<uint8_t> deviceKinds() const { return kinds_; }

    bool open(const std::string& port, std::string& error) {
        close();
        const std::string path = "\\\\.\\" + port;
        handle_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            error = "CreateFile(" + port + ") failed, error " + std::to_string(GetLastError());
            return false;
        }
        SetupComm(handle_, 4096, 4096);
        COMMTIMEOUTS timeouts{};
        // Чтение: вернуться сразу, если есть байты, иначе ждать первый байт до 50 мс.
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
        timeouts.ReadTotalTimeoutConstant = 50;
        timeouts.WriteTotalTimeoutConstant = 0;
        timeouts.WriteTotalTimeoutMultiplier = 0;
        SetCommTimeouts(handle_, &timeouts);

        failed_ = false;
        stopReader_ = false;
        reader_ = std::thread([this] { readerLoop(); });

        // Скорость UART CH343<->ESP32 должна совпадать с прошивкой.
        for (uint32_t candidate : {4000000u, 1000000u, 115200u}) {
            if (!configure(candidate)) {
                continue;
            }
            Sleep(20);
            if (auto k = probeDeviceKinds(std::chrono::milliseconds(300))) {
                baud_ = candidate;
                kinds_ = k;
                return true;
            }
        }
        error = "MAKCU не ответил на DEVICE (0x02) ни на одной скорости";
        close();
        return false;
    }

    void close() {
        if (handle_ == INVALID_HANDLE_VALUE) {
            return;
        }
        stopReader_ = true;
        CancelIoEx(handle_, nullptr);
        if (reader_.joinable()) {
            reader_.join();
        }
        for (auto& slot : ring_) {
            if (slot.used) {
                DWORD n = 0;
                GetOverlappedResult(handle_, &slot.ov, &n, TRUE);
                slot.used = false;
            }
        }
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        next_ = 0;
    }

    // Текстовый запрос KM_API (например "km.device()"), возвращает строку результата.
    std::optional<std::string> kmQuery(const std::string& command, std::chrono::milliseconds timeout) {
        {
            std::lock_guard<std::mutex> lock(rxMutex_);
            rx_.clear();
        }
        const std::string line = command + "\r\n";
        if (!writeBlocking(reinterpret_cast<const uint8_t*>(line.data()), line.size())) {
            return std::nullopt;
        }
        std::unique_lock<std::mutex> lock(rxMutex_);
        std::optional<std::string> result;
        rxCv_.wait_for(lock, timeout, [&] {
            // Ответ: "[эхо\r\n]<результат>\r\n>>> " - берём последнюю строку перед приглашением.
            const std::string text(rx_.begin(), rx_.end());
            const auto prompt = text.find("\r\n>>> ");
            if (prompt == std::string::npos) return false;
            const auto lineStart = text.rfind("\r\n", prompt == 0 ? 0 : prompt - 1);
            const size_t from = (lineStart == std::string::npos || lineStart >= prompt) ? 0 : lineStart + 2;
            result = text.substr(from, prompt - from);
            return true;
        });
        return result;
    }

    // ---- горячий путь -----------------------------------------------------
    [[nodiscard]] bool move(int16_t dx, int16_t dy) override {
        if (!healthy()) {
            return false;
        }
        Slot& slot = ring_[next_];
        if (slot.used) {
            if (!HasOverlappedIoCompleted(&slot.ov)) {
                // Драйвер ещё не отправил 32 предыдущих кадра: не копим
                // очередь (это дало бы пачки и рывки), а переносим дельту.
                stats.deferred.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (!reapSlot(slot)) {
                return false;
            }
        }

        if (kmText_) {
            const int n = std::snprintf(reinterpret_cast<char*>(slot.frame.data()), slot.frame.size(),
                "km.move(%d,%d)\r\n", dx, dy);
            slot.len = static_cast<size_t>(n);
        } else {
            const auto bin = buildMoveFrame(dx, dy);
            std::copy(bin.begin(), bin.end(), slot.frame.begin());
            slot.len = bin.size();
        }
        const int64_t t0 = Clock::now();
        if (!submit(slot)) {
            return false;
        }
        if (mode_ == Mode::Sync) {
            DWORD n = 0;
            const BOOL ok = GetOverlappedResult(handle_, &slot.ov, &n, TRUE);
            slot.used = false;
            if (!ok || n != slot.len) {
                fail();
                return false;
            }
        }
        stats.noteCall(Clock::toUs(Clock::now() - t0));
        stats.writes.fetch_add(1, std::memory_order_relaxed);
        next_ = (next_ + 1) % ring_.size();

        if (mode_ == Mode::Async) {
            uint32_t inflight = 0;
            for (const auto& s : ring_) {
                if (s.used && !HasOverlappedIoCompleted(&s.ov)) {
                    ++inflight;
                }
            }
            if (inflight > stats.maxInflight.load(std::memory_order_relaxed)) {
                stats.maxInflight.store(inflight, std::memory_order_relaxed);
            }
        }
        return true;
    }

private:
    struct Slot {
        OVERLAPPED ov{};
        std::array<uint8_t, 40> frame{};
        size_t len = 0;
        bool used = false;
    };

    bool configure(uint32_t baud) {
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(handle_, &dcb)) {
            return false;
        }
        dcb.BaudRate = baud;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        // DTR/RTS выключены: иначе можно перезапустить ESP32.
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fErrorChar = FALSE;
        dcb.fNull = FALSE;
        dcb.fAbortOnError = FALSE;
        if (!SetCommState(handle_, &dcb)) {
            return false;
        }
        PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        return true;
    }

    std::optional<uint8_t> probeDeviceKinds(std::chrono::milliseconds timeout) {
        {
            std::lock_guard<std::mutex> lock(rxMutex_);
            rx_.clear();
        }
        const uint8_t request[5] = {0xDE, 0xAD, 0x00, 0x00,
                                    static_cast<uint8_t>(makxd::ApiOpcode::DEVICE)};
        if (!writeBlocking(request, sizeof(request))) {
            return std::nullopt;
        }
        std::unique_lock<std::mutex> lock(rxMutex_);
        std::optional<uint8_t> kinds;
        rxCv_.wait_for(lock, timeout, [&] {
            for (size_t i = 0; i + 6 <= rx_.size(); ++i) {
                if (rx_[i] == 0xDE && rx_[i + 1] == 0xAD && rx_[i + 2] == 0x01 &&
                    rx_[i + 3] == 0x00 && rx_[i + 4] == 0x02) {
                    kinds = rx_[i + 5];
                    return true;
                }
            }
            return false;
        });
        return kinds;
    }

    bool writeBlocking(const uint8_t* data, size_t size) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = 0;
        BOOL ok = WriteFile(handle_, data, static_cast<DWORD>(size), nullptr, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            ok = TRUE;
        }
        if (ok) {
            ok = GetOverlappedResult(handle_, &ov, &n, TRUE);
        }
        CloseHandle(ov.hEvent);
        return ok && n == size;
    }

    bool submit(Slot& slot) {
        HANDLE ev = slot.ov.hEvent;
        slot.ov = OVERLAPPED{};
        slot.ov.hEvent = ev;
        slot.used = true;
        const BOOL ok = WriteFile(handle_, slot.frame.data(),
            static_cast<DWORD>(slot.len), nullptr, &slot.ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            slot.used = false;
            fail();
            return false;
        }
        return true;
    }

    bool reapSlot(Slot& slot) {
        DWORD n = 0;
        const BOOL ok = GetOverlappedResult(handle_, &slot.ov, &n, FALSE);
        slot.used = false;
        if (!ok || n != slot.len) {
            fail();
            return false;
        }
        return true;
    }

    void fail() {
        failed_ = true;
        stats.errors.fetch_add(1, std::memory_order_relaxed);
    }

    // Поток чтения: принимает ответы и вычищает входной буфер (события кнопок
    // и т.п.), чтобы RX драйвера никогда не переполнялся. Overlapped-handle
    // позволяет читать параллельно с записью без взаимной блокировки.
    void readerLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::array<uint8_t, 512> buffer{};
        while (!stopReader_) {
            DWORD n = 0;
            ResetEvent(ov.hEvent);
            BOOL ok = ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &ov);
            if (!ok && GetLastError() != ERROR_IO_PENDING) {
                if (stopReader_) break;
                Sleep(5);
                continue;
            }
            ok = GetOverlappedResult(handle_, &ov, &n, TRUE);
            if (!ok) {
                if (stopReader_) break;
                Sleep(5);
                continue;
            }
            if (n > 0) {
                std::lock_guard<std::mutex> lock(rxMutex_);
                rx_.insert(rx_.end(), buffer.begin(), buffer.begin() + n);
                if (rx_.size() > 8192) {
                    rx_.erase(rx_.begin(), rx_.end() - 4096);
                }
                rxCv_.notify_all();
            }
        }
        CloseHandle(ov.hEvent);
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    Mode mode_ = Mode::Async;
    bool kmText_ = false;
    std::atomic<bool> failed_{false};
    uint32_t baud_ = 0;
    std::optional<uint8_t> kinds_;

    std::array<Slot, 32> ring_{};
    size_t next_ = 0;

    std::thread reader_;
    std::atomic<bool> stopReader_{false};
    std::mutex rxMutex_;
    std::condition_variable rxCv_;
    std::vector<uint8_t> rx_;
};

// ===========================================================================
// SdkTransport: makxd::Device::mouseMove() в отдельном потоке.
// Поток тайминга только складывает дельты в почтовый ящик (атомики), поэтому
// блокировки SDK не ломают расписание, а сумма пути сохраняется точно.
// Если SDK не успевает - несколько тиков сливаются в одну команду (это и
// будет видно как падение поллинга).
// ===========================================================================
class SdkTransport final : public Transport {
public:
    explicit SdkTransport(makxd::Device& device) : device_(device) {
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        worker_ = std::thread([this] { loop(); });
    }
    ~SdkTransport() override {
        stop_ = true;
        SetEvent(wake_);
        if (worker_.joinable()) {
            worker_.join();
        }
        CloseHandle(wake_);
    }

    [[nodiscard]] const char* name() const override { return "sdk-mouseMove"; }
    [[nodiscard]] bool healthy() const override { return device_.isConnected() && !failed_; }

    [[nodiscard]] bool move(int16_t dx, int16_t dy) override {
        pendingX_.fetch_add(dx, std::memory_order_relaxed);
        pendingY_.fetch_add(dy, std::memory_order_relaxed);
        pendingCalls_.fetch_add(1, std::memory_order_relaxed);
        SetEvent(wake_);
        return true;
    }

private:
    void loop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        while (!stop_) {
            WaitForSingleObject(wake_, 20);
            int64_t x = pendingX_.exchange(0, std::memory_order_relaxed);
            int64_t y = pendingY_.exchange(0, std::memory_order_relaxed);
            // Явный MOVE(0,0) (самотест) тоже отправляем один раз.
            bool forceOnce = pendingCalls_.exchange(0, std::memory_order_relaxed) > 0 && x == 0 && y == 0;
            while ((x != 0 || y != 0 || forceOnce) && !stop_) {
                forceOnce = false;
                const auto cx = static_cast<int32_t>(std::clamp<int64_t>(x, -32767, 32767));
                const auto cy = static_cast<int32_t>(std::clamp<int64_t>(y, -32767, 32767));
                const int64_t t0 = Clock::now();
                const bool ok = device_.mouseMove(cx, cy);
                stats.noteCall(Clock::toUs(Clock::now() - t0));
                if (!ok) {
                    stats.errors.fetch_add(1, std::memory_order_relaxed);
                    failed_ = true;
                    break;
                }
                stats.writes.fetch_add(1, std::memory_order_relaxed);
                x -= cx;
                y -= cy;
            }
        }
    }

    makxd::Device& device_;
    HANDLE wake_ = nullptr;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::atomic<int64_t> pendingX_{0};
    std::atomic<int64_t> pendingY_{0};
    std::atomic<uint64_t> pendingCalls_{0};
};

}  // namespace mover
