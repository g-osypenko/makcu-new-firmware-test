// Транспорт команд движения в MAKCU по COM.
//
//  RawComTransport  - собственный overlapped-handle порта (горячий путь).
//      async: WriteFile(OVERLAPPED) "выстрелил и забыл", кольцо слотов,
//             поток тайминга никогда не блокируется на USB.
//      sync : та же запись, но с ожиданием завершения (для сравнения).
//      Форматы: ЭТАЛОН - Format::RawMove (0x69 RAW_MOVE + квитанции 0x6A,
//      один HID-отчёт на запрос, ~1000 Гц на fw 4094). Mak (0x18), KmMove,
//      KmMoveNow, KmMoveNowFramed идут через интерполятор прошивки (~640-690
//      Гц) и оставлены только для сравнения/диагностики.
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
#include <cstring>
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
    // Конец прогона: дослать то, что транспорт держит у себя (квитанции, перенос).
    virtual void finish() {}
    // Статистика сверх общей (пусто - нечего добавить).
    [[nodiscard]] virtual std::string extraStats() const { return {}; }
    virtual void resetStats() { stats.reset(); }
    TransportStats stats;
};

// ===========================================================================
// RawComTransport
// ===========================================================================
class RawComTransport final : public Transport {
public:
    enum class Mode { Async, Sync };
    // Формат команды движения на проводе.
    enum class Format {
        Mak,              // MAK_API MOVE 0x18: DE AD 04 00 18 x y
        KmMove,           // текст "km.move(x,y)\r\n"
        KmMoveNow,        // текст "km.move_now(x,y)\r\n" ("one-report horizon", makcu.com/en/api)
        KmMoveNowFramed,  // тот же move_now в рамке MAK_API: DE AD LEN 'k' "m.move_now(x,y)"
        RawMove,          // 0x69 RAW_MOVE + квитанции 0x6A (V4.074+): FIFO, ровно один HID-отчёт
                          // на запрос, мимо AUTO-интерполяции (makcu.com/api/raw-v4074.md)
    };

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
        const bool async = mode_ == Mode::Async;
        switch (format_) {
        case Format::KmMove: return async ? "km-async" : "km-sync";
        case Format::KmMoveNow: return async ? "kmnow-async" : "kmnow-sync";
        case Format::KmMoveNowFramed: return async ? "kmnowf-async" : "kmnowf-sync";
        case Format::RawMove: return async ? "rawmove-async" : "rawmove-sync";
        case Format::Mak: break;
        }
        return async ? "raw-async" : "raw-sync";
    }
    [[nodiscard]] bool healthy() const override { return handle_ != INVALID_HANDLE_VALUE && !failed_; }
    [[nodiscard]] bool isOpen() const { return handle_ != INVALID_HANDLE_VALUE; }
    void setMode(Mode mode) { mode_ = mode; }
    // Для RawMove после setFormat нужен prepareRaw().
    void setFormat(Format format) {
        format_ = format;
        if (format != Format::RawMove) rawMode_ = false;
    }

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
        if (format_ == Format::RawMove) {
            return moveRaw(dx, dy, false);
        }
        std::array<uint8_t, kFrameMax> frame{};
        const size_t len = encodeMove(dx, dy, frame);
        return sendFrame(frame.data(), len);
    }

    // ---- Raw movement 0x69/0x6A ---------------------------------------------
    // 0x69: u32 id, i16 x, i16 y, u16 timeout_uf -> ответ state:u8 reason:u8 id:u32
    //       (1 = в FIFO, ещё НЕ отправлено; 255 = немедленная ошибка, reason 3 = нет места).
    // 0x6A: u32 id, u8 consume -> тот же ответ (2 = отдано в USB-endpoint, 3 = отказ,
    //       255/6 = ещё в очереди). У платы не больше 16 живых/непрочитанных записей.
    // ID строго растут в пределах загрузки платы - узнаём водяной знак (id=0).
    bool prepareRaw(std::string& info) {
        rawMode_ = false;
        auto meta = queryRawMeta();
        if (!meta) {
            info = "нет ответа на 0x6A(id=0) - прошивка без Raw API?";
            return false;
        }
        char buf[200];
        std::snprintf(buf, sizeof(buf), "schema %u, last_accepted_id %lu, свободно записей %u из %u", meta->schema,
            static_cast<unsigned long>(meta->lastId), meta->freeRecords, meta->capacity);
        info = buf;
        // Неподтверждённые записи прошлых (прерванных) сессий живут до перезагрузки
        // платы и съедают её 16 мест: подтверждаем хвост последних id.
        if (meta->freeRecords < meta->capacity && meta->lastId > 0) {
            const uint32_t consumed = drainLeftoverRecords(meta->lastId, meta->capacity - meta->freeRecords);
            if (auto again = queryRawMeta()) meta = again;
            std::snprintf(buf, sizeof(buf), "; дочищено старых записей %lu -> свободно %u из %u",
                static_cast<unsigned long>(consumed), meta->freeRecords, meta->capacity);
            info += buf;
        }
        {
            std::lock_guard<std::mutex> lock(rawMutex_);
            rawNextId_ = meta->lastId + 1;
            rawLive_ = 0;
            rawCarryX_ = 0;
            rawCarryY_ = 0;
            for (auto& r : rawRec_) r = RawRecord{};
            // Окно не больше реально свободных записей платы (с запасом 2).
            rawWindow_ = std::clamp<uint32_t>(meta->freeRecords > 2 ? meta->freeRecords - 2u : 1u, 1u, kRawMaxLive);
        }
        {
            std::lock_guard<std::mutex> lock(rawRxMutex_);
            rawRx_.clear();
        }
        rawMode_ = true;
        if (rawWindow_ < kRawMaxLive) {
            info += "; окно уменьшено до " + std::to_string(rawWindow_);
        }
        return true;
    }

    // Конец прогона в RawMove: квитировать живые записи и дослать перенос (до 300 мс).
    void finish() override {
        if (format_ != Format::RawMove || !rawMode_.load()) return;
        const int64_t until = Clock::now() + Clock::fromUs(300000.0);
        while (Clock::now() < until && healthy()) {
            bool idle = false;
            {
                std::lock_guard<std::mutex> lock(rawMutex_);
                idle = rawLive_ == 0 && rawCarryX_ == 0 && rawCarryY_ == 0;
            }
            if (idle) break;
            (void)moveRaw(0, 0, true);
            Sleep(1);
        }
    }

    void resetStats() override {
        stats.reset();
        rawStats_.reset();
    }

    [[nodiscard]] std::string extraStats() const override {
        if (format_ != Format::RawMove) return {};
        std::lock_guard<std::mutex> lock(rawMutex_);
        char buf[400];
        std::snprintf(buf, sizeof(buf),
            "raw 0x69: отправлено %llu | принято в FIFO %llu | отдано в USB-endpoint %llu | отказ %llu"
            " | немедл. ошибок %llu (нет места %llu) | ack %llu, повторов %llu | окно max %u | ждали окно %llu"
            " | перенос сейчас (%lld,%lld) | посл. причина %u",
            ull(rawStats_.submitted), ull(rawStats_.accepted), ull(rawStats_.endpointOk), ull(rawStats_.rejected),
            ull(rawStats_.immediate), ull(rawStats_.noCapacity), ull(rawStats_.ackSent), ull(rawStats_.ackRetry),
            rawStats_.maxLive.load(), ull(rawStats_.windowFull), static_cast<long long>(rawCarryX_),
            static_cast<long long>(rawCarryY_), rawStats_.lastReason.load());
        return buf;
    }

private:
    static constexpr size_t kFrameMax = 64;

    // Общий путь записи: кадр(ы) в свободный слот кольца overlapped-записей.
    bool sendFrame(const uint8_t* data, size_t len) {
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

        std::copy(data, data + len, slot.frame.begin());
        slot.len = len;
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

    struct RawRecord {
        uint32_t id = 0;
        int32_t dx = 0;
        int32_t dy = 0;
        int64_t tSubmit = 0;
        int64_t tAck = 0;
        uint8_t phase = 0;  // 0 свободно, 1 ждём ответ на 0x69, 2 в FIFO платы, 3 отправлен 0x6A
    };
    struct RawStats {
        std::atomic<uint64_t> submitted{0};
        std::atomic<uint64_t> accepted{0};
        std::atomic<uint64_t> endpointOk{0};
        std::atomic<uint64_t> rejected{0};
        std::atomic<uint64_t> immediate{0};
        std::atomic<uint64_t> noCapacity{0};
        std::atomic<uint64_t> ackSent{0};
        std::atomic<uint64_t> ackRetry{0};
        std::atomic<uint64_t> windowFull{0};
        std::atomic<uint32_t> lastReason{0};
        std::atomic<uint32_t> maxLive{0};
        void reset() {
            submitted = 0;
            accepted = 0;
            endpointOk = 0;
            rejected = 0;
            immediate = 0;
            noCapacity = 0;
            ackSent = 0;
            ackRetry = 0;
            windowFull = 0;
            lastReason = 0;
            maxLive = 0;
        }
    };
    static constexpr size_t kRawSlots = 64;          // индекс записи = id % 64 (> ёмкости платы 16)
    static constexpr uint32_t kRawMaxLive = 12;      // своё окно, запас до 16 записей платы
    static constexpr double kRawAckAfterUs = 2000.0; // квитировать не раньше 2 мс после отправки
    static constexpr double kRawAckRetryUs = 1000.0; // повтор квитирования не чаще раза в 1 мс
    static constexpr double kRawLostReplyUs = 50000.0;  // нет ответа на 0x69 за 50 мс - спросить 0x6A

    struct RawMeta {
        uint8_t schema = 0;
        uint32_t lastId = 0;
        uint8_t freeRecords = 0;
        uint8_t capacity = 0;
    };

    // 0x6A(id=0, consume=0) -> 0:u8 schema:u8 last_accepted_id:u32 free_records:u8 capacity:u8.
    // Вызывать при rawMode_ == false (ответ читается через transact).
    std::optional<RawMeta> queryRawMeta() {
        const uint8_t query[10] = {0xDE, 0xAD, 0x05, 0x00, 0x6A, 0, 0, 0, 0, 0};
        const auto rx = transact(query, sizeof(query), std::chrono::milliseconds(80));
        for (size_t i = 0; i + 13 <= rx.size(); ++i) {
            if (rx[i] != 0xDE || rx[i + 1] != 0xAD || rx[i + 4] != 0x6A) continue;
            if (rx[i + 2] == 0x01 && rx[i + 5] == 0xFF) return std::nullopt;  // отказ: прошивка без Raw API
            if (rx[i + 2] != 0x08 || rx[i + 3] != 0x00) continue;
            const uint8_t* p = &rx[i + 5];
            return RawMeta{p[1], le32(p + 2), p[6], p[7]};
        }
        return std::nullopt;
    }

    // Подтвердить (consume=1) окончательные записи среди последних id, начиная с
    // lastId вниз, пачками по 32 запроса; до 4096 id или пока не найдено want штук.
    // Несуществующие/уже подтверждённые id отвечают ошибкой 5 - это безвредно.
    // ВНИМАНИЕ: подтверждает и записи другой программы, если она тоже шлёт Raw.
    uint32_t drainLeftoverRecords(uint32_t lastId, uint32_t want) {
        uint32_t consumed = 0;
        uint32_t id = lastId;
        for (uint32_t scanned = 0; id >= 1 && scanned < 4096 && consumed < want;) {
            std::vector<uint8_t> batch;
            for (int k = 0; k < 32 && id >= 1; ++k, --id, ++scanned) {
                const uint8_t payload[5] = {static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8),
                    static_cast<uint8_t>(id >> 16), static_cast<uint8_t>(id >> 24), 1};
                uint8_t frame[10];
                const size_t n = putRawFrame(frame, 0x6A, payload, sizeof(payload));
                batch.insert(batch.end(), frame, frame + n);
            }
            const auto rx = transact(batch.data(), batch.size(), std::chrono::milliseconds(15));
            for (size_t i = 0; i + 11 <= rx.size(); ++i) {
                if (rx[i] == 0xDE && rx[i + 1] == 0xAD && rx[i + 2] == 0x06 && rx[i + 3] == 0x00 &&
                    rx[i + 4] == 0x6A && (rx[i + 5] == 2 || rx[i + 5] == 3)) {
                    ++consumed;
                    i += 10;
                }
            }
        }
        return consumed;
    }

    static uint32_t le32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
               static_cast<uint32_t>(p[3]) << 24;
    }
    static unsigned long long ull(const std::atomic<uint64_t>& v) { return v.load(); }

    static size_t putRawFrame(uint8_t* out, uint8_t opcode, const uint8_t* payload, uint16_t len) {
        out[0] = 0xDE;
        out[1] = 0xAD;
        out[2] = static_cast<uint8_t>(len & 0xFF);
        out[3] = static_cast<uint8_t>(len >> 8);
        out[4] = opcode;
        std::memcpy(out + 5, payload, len);
        return 5u + len;
    }

    // Под rawMutex_: дописать в out кадры 0x6A(consume=1) для записей, которым пора.
    size_t appendAcksLocked(uint8_t* out, size_t len, int64_t now) {
        const int64_t ackAfter = Clock::fromUs(kRawAckAfterUs);
        const int64_t retry = Clock::fromUs(kRawAckRetryUs);
        const int64_t lost = Clock::fromUs(kRawLostReplyUs);
        int added = 0;
        for (auto& r : rawRec_) {
            if (added >= 3 || len + 10 > kFrameMax) break;
            const bool due = (r.phase == 2 && now - r.tSubmit >= ackAfter && (r.tAck == 0 || now - r.tAck >= retry)) ||
                             (r.phase == 3 && now - r.tAck >= Clock::fromUs(20000.0)) ||  // ответ на ack потерян
                             (r.phase == 1 && now - r.tSubmit >= lost);                   // ответ на 0x69 потерян
            if (!due) continue;
            const uint8_t payload[5] = {static_cast<uint8_t>(r.id), static_cast<uint8_t>(r.id >> 8),
                static_cast<uint8_t>(r.id >> 16), static_cast<uint8_t>(r.id >> 24), 1};
            len += putRawFrame(out + len, 0x6A, payload, sizeof(payload));
            r.phase = 3;
            r.tAck = now;
            rawStats_.ackSent.fetch_add(1, std::memory_order_relaxed);
            ++added;
        }
        return len;
    }

    // flushOnly: не новая дельта, только квитанции и перенос (finish()).
    bool moveRaw(int16_t dx, int16_t dy, bool flushOnly) {
        if (!rawMode_.load()) {
            fail();
            return false;
        }
        std::array<uint8_t, kFrameMax> buf{};
        size_t len = 0;
        bool submitted = false;
        uint32_t id = 0;
        const int64_t now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(rawMutex_);
            const bool carry = rawCarryX_ != 0 || rawCarryY_ != 0;
            // Слот следующего id ещё занят зависшей записью - не затирать её
            // (иначе она останется неподтверждённой на плате), ждать как при полном окне.
            const bool slotFree = rawRec_[rawNextId_ % kRawSlots].phase == 0;
            if ((!flushOnly || carry) && rawLive_ < rawWindow_ && slotFree) {
                const int64_t x = static_cast<int64_t>(dx) + rawCarryX_;
                const int64_t y = static_cast<int64_t>(dy) + rawCarryY_;
                const int64_t cx = std::clamp<int64_t>(x, -32767, 32767);
                const int64_t cy = std::clamp<int64_t>(y, -32767, 32767);
                rawCarryX_ = x - cx;
                rawCarryY_ = y - cy;
                id = rawNextId_++;
                RawRecord& r = rawRec_[id % kRawSlots];
                r = RawRecord{id, static_cast<int32_t>(cx), static_cast<int32_t>(cy), now, 0, 1};
                ++rawLive_;
                if (rawLive_ > rawStats_.maxLive.load(std::memory_order_relaxed)) {
                    rawStats_.maxLive.store(rawLive_, std::memory_order_relaxed);
                }
                const auto ux = static_cast<uint16_t>(cx);
                const auto uy = static_cast<uint16_t>(cy);
                // timeout_uf = 0: значение платы по умолчанию (8000 uf = 1 с).
                const uint8_t payload[10] = {static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8),
                    static_cast<uint8_t>(id >> 16), static_cast<uint8_t>(id >> 24), static_cast<uint8_t>(ux & 0xFF),
                    static_cast<uint8_t>(ux >> 8), static_cast<uint8_t>(uy & 0xFF), static_cast<uint8_t>(uy >> 8), 0, 0};
                len = putRawFrame(buf.data(), 0x69, payload, sizeof(payload));
                submitted = true;
            } else if (!flushOnly) {
                rawStats_.windowFull.fetch_add(1, std::memory_order_relaxed);
            }
            len = appendAcksLocked(buf.data(), len, now);
        }
        if (len == 0) {
            return false;
        }
        if (!sendFrame(buf.data(), len)) {
            if (submitted) {
                // Запись не ушла: вернуть дельту в перенос и освободить окно.
                std::lock_guard<std::mutex> lock(rawMutex_);
                RawRecord& r = rawRec_[id % kRawSlots];
                if (r.id == id && r.phase == 1) {
                    rawCarryX_ += r.dx;
                    rawCarryY_ += r.dy;
                    r.phase = 0;
                    --rawLive_;
                }
            }
            return false;
        }
        if (submitted) {
            rawStats_.submitted.fetch_add(1, std::memory_order_relaxed);
        }
        // Новая дельта принята транспортом, только если ушёл новый запрос.
        return submitted || flushOnly;
    }

    // Поток чтения: ответы 0x69/0x6A (длина 6). Остальные кадры пропускаем по длине.
    void parseRaw() {
        size_t i = 0;
        while (i + 5 <= rawRx_.size()) {
            if (rawRx_[i] != 0xDE || rawRx_[i + 1] != 0xAD) {
                ++i;
                continue;
            }
            const size_t plen = static_cast<size_t>(rawRx_[i + 2]) | static_cast<size_t>(rawRx_[i + 3]) << 8;
            if (plen > 512) {
                ++i;
                continue;
            }
            if (i + 5 + plen > rawRx_.size()) break;  // кадр ещё не дочитан
            const uint8_t op = rawRx_[i + 4];
            if ((op == 0x69 || op == 0x6A) && plen == 6) {
                const uint8_t* p = &rawRx_[i + 5];
                onRawReply(op, p[0], p[1], le32(p + 2));
            }
            i += 5 + plen;
        }
        rawRx_.erase(rawRx_.begin(), rawRx_.begin() + static_cast<std::ptrdiff_t>(i));
    }

    void onRawReply(uint8_t op, uint8_t state, uint8_t reason, uint32_t id) {
        std::lock_guard<std::mutex> lock(rawMutex_);
        RawRecord& r = rawRec_[id % kRawSlots];
        if (r.id != id || r.phase == 0) return;  // уже закрыта (повторная квитанция)
        auto release = [&](bool returnDelta) {
            if (returnDelta) {
                rawCarryX_ += r.dx;
                rawCarryY_ += r.dy;
            }
            r.phase = 0;
            --rawLive_;
        };
        if (state == 1) {  // в FIFO, ещё не отправлено
            if (op == 0x69) rawStats_.accepted.fetch_add(1, std::memory_order_relaxed);
            r.phase = 2;
        } else if (state == 2) {  // отдано в USB-endpoint
            if (op == 0x69) rawStats_.accepted.fetch_add(1, std::memory_order_relaxed);
            rawStats_.endpointOk.fetch_add(1, std::memory_order_relaxed);
            release(false);
        } else if (state == 3) {  // окончательный отказ: движение не ушло, вернуть в перенос
            rawStats_.rejected.fetch_add(1, std::memory_order_relaxed);
            rawStats_.lastReason.store(reason, std::memory_order_relaxed);
            release(true);
        } else if (state == 255 && reason == 6) {  // ack на запрос, который ещё в очереди
            rawStats_.ackRetry.fetch_add(1, std::memory_order_relaxed);
            r.phase = 2;
        } else {  // немедленная ошибка: запрос не принят (3 = нет места, 5 = нет такого ID и т.д.)
            rawStats_.immediate.fetch_add(1, std::memory_order_relaxed);
            rawStats_.lastReason.store(reason, std::memory_order_relaxed);
            if (reason == 3) rawStats_.noCapacity.fetch_add(1, std::memory_order_relaxed);
            // 2 = ID не больше уже принятого (ID занимал кто-то ещё): уйти вперёд.
            if (reason == 2) rawNextId_ += 1000;
            release(true);
        }
    }

public:
    // Диагностика: отправить байты и вернуть всё, что пришло за wait.
    std::vector<uint8_t> transact(const uint8_t* data, size_t size, std::chrono::milliseconds wait) {
        {
            std::lock_guard<std::mutex> lock(rxMutex_);
            rx_.clear();
        }
        if (!writeBlocking(data, size)) {
            return {};
        }
        Sleep(static_cast<DWORD>(wait.count()));
        std::lock_guard<std::mutex> lock(rxMutex_);
        return rx_;
    }

private:
    struct Slot {
        OVERLAPPED ov{};
        std::array<uint8_t, kFrameMax> frame{};
        size_t len = 0;
        bool used = false;
    };

    size_t encodeMove(int16_t dx, int16_t dy, std::array<uint8_t, kFrameMax>& out) const {
        char* text = reinterpret_cast<char*>(out.data());
        switch (format_) {
        case Format::KmMove:
            return static_cast<size_t>(std::snprintf(text, out.size(), "km.move(%d,%d)\r\n", dx, dy));
        case Format::KmMoveNow:
            return static_cast<size_t>(std::snprintf(text, out.size(), "km.move_now(%d,%d)\r\n", dx, dy));
        case Format::KmMoveNowFramed: {
            // KM-запись в рамке MAK_API: 'k' - байт команды, остаток текста - payload, LEN без 'k'.
            const int n = std::snprintf(text + 5, out.size() - 5, "m.move_now(%d,%d)", dx, dy);
            out[0] = 0xDE;
            out[1] = 0xAD;
            out[2] = static_cast<uint8_t>(n & 0xFF);
            out[3] = static_cast<uint8_t>(n >> 8);
            out[4] = 'k';
            return static_cast<size_t>(5 + n);
        }
        case Format::RawMove:  // кадры собирает moveRaw()
        case Format::Mak:
            break;
        }
        const auto bin = buildMoveFrame(dx, dy);
        std::copy(bin.begin(), bin.end(), out.begin());
        return bin.size();
    }

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
            if (n > 0 && rawMode_.load(std::memory_order_relaxed)) {
                std::lock_guard<std::mutex> lock(rawRxMutex_);
                rawRx_.insert(rawRx_.end(), buffer.begin(), buffer.begin() + n);
                parseRaw();
            }
        }
        CloseHandle(ov.hEvent);
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    Mode mode_ = Mode::Async;
    Format format_ = Format::Mak;
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

    // Raw movement: записи по id % kRawSlots, окно, перенос отказанных дельт.
    mutable std::mutex rawMutex_;
    std::array<RawRecord, kRawSlots> rawRec_{};
    uint32_t rawNextId_ = 1;
    uint32_t rawLive_ = 0;
    uint32_t rawWindow_ = kRawMaxLive;  // по свободным записям платы (prepareRaw)
    int64_t rawCarryX_ = 0;
    int64_t rawCarryY_ = 0;
    std::mutex rawRxMutex_;
    std::vector<uint8_t> rawRx_;  // только поток чтения (и prepareRaw под rawRxMutex_)
    std::atomic<bool> rawMode_{false};
    RawStats rawStats_;
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
