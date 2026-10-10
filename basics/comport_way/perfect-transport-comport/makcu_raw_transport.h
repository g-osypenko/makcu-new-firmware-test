// MAKCU Raw transport (COM) - замороженная эталонная версия.
//
// Один заголовок, без зависимостей кроме WinAPI (SetupAPI для поиска порта).
// Доставляет целые отчёты (dx, dy) в MAKCU командой Raw movement 0x69 с
// квитанциями 0x6A: на fw с Raw API (V4.074+, проверено на 4094) каждый
// принятый move() = ровно один HID-отчёт, мимо интерполятора прошивки.
//
// Что транспорт делает:
//   - свой overlapped-handle COM, кольцо из 32 записей WriteFile, поток чтения;
//   - открытие: поиск CH343 (VID 1A86 PID 55D3), проба скорости 4М/1М/115200
//     по ответу DEVICE (0x02), DTR/RTS выключены;
//   - Raw: метаданные 0x6A(id=0), дочистка записей прошлых сессий, окно
//     живых записей (<= 12 и <= свободно на плате - 2), квитанции, перенос
//     отказанных дельт, finish() в конце.
// Чего транспорт НЕ делает (это забота вызывающего):
//   - темп (часы 1 мс), нарезка траектории, квантование/рендерер. Частота HID =
//     частота ненулевых move(), не выше опроса USB клонированной мыши.
//
// Контракт вызова:
//   - move()/service()/finish() - из ОДНОГО потока (обычно поток тайминга).
//     Никогда не блокируются на USB (finish() - до timeoutMs).
//   - move() == false: сейчас отправить нельзя (окно/кольцо заняты) -
//     вызывающий оставляет дельту у себя и добавляет к следующему вызову.
//     Внутренний перенос (клампинг >32767, отказ платы) транспорт ведёт сам.
//   - move(0, 0) ничего не отправляет на мышь, только квитанции/перенос (= service()).
//   - stats()/statsLine() - из любого потока.
//
// Происхождение: basics/comport_way/makcu_transport.h (RawComTransport,
// Format::RawMove, Mode::Async) от 2026-10-10, логика Raw перенесена без
// изменений. Не править: новая версия - новая папка.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace makcu {

namespace detail {

// QPC-часы транспорта (свои, чтобы не зависеть от окружения).
struct Qpc {
    static int64_t freq() {
        static const int64_t f = [] {
            LARGE_INTEGER v;
            QueryPerformanceFrequency(&v);
            return static_cast<int64_t>(v.QuadPart);
        }();
        return f;
    }
    static int64_t now() {
        LARGE_INTEGER v;
        QueryPerformanceCounter(&v);
        return v.QuadPart;
    }
    static int64_t fromUs(double us) {
        return static_cast<int64_t>(std::llround(us * static_cast<double>(freq()) / 1e6));
    }
    static double toUs(int64_t ticks) { return static_cast<double>(ticks) * 1e6 / static_cast<double>(freq()); }
};

}  // namespace detail

// COM-порты MAKCU (мост CH343, VID 1A86 PID 55D3), например {"COM5"}.
inline std::vector<std::string> findMakcuPorts() {
    // GUID_DEVCLASS_PORTS {4D36E978-E325-11CE-BFC1-08002BE10318} - без devguid.h/initguid.
    static const GUID kPortsClass = {0x4D36E978, 0xE325, 0x11CE, {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}};
    std::vector<std::string> ports;
    HDEVINFO set = SetupDiGetClassDevsA(&kPortsClass, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return ports;
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); ++i) {
        char hwid[1024]{};
        if (!SetupDiGetDeviceRegistryPropertyA(set, &info, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(hwid),
                sizeof(hwid) - 1, nullptr)) {
            continue;
        }
        std::string id(hwid);
        std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (id.find("VID_1A86&PID_55D3") == std::string::npos) continue;
        HKEY key = SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        char name[256]{};
        DWORD size = sizeof(name) - 1;
        if (RegQueryValueExA(key, "PortName", nullptr, nullptr, reinterpret_cast<BYTE*>(name), &size) == ERROR_SUCCESS) {
            ports.emplace_back(name);
        }
        RegCloseKey(key);
    }
    SetupDiDestroyDeviceInfoList(set);
    std::sort(ports.begin(), ports.end());
    return ports;
}

class RawTransport {
public:
    // Снимок счётчиков (stats()).
    struct Stats {
        uint64_t writes = 0;        // WriteFile в драйвер (кадр = 0x69 и/или квитанции)
        uint64_t driverBusy = 0;    // move() == false: все 32 записи драйвера ещё в полёте
        uint64_t errors = 0;
        uint32_t maxInflight = 0;   // записей драйвера в полёте, максимум
        double maxCallUs = 0.0;     // самый долгий вызов отправки
        double avgCallUs = 0.0;
        // Raw 0x69/0x6A
        uint64_t submitted = 0;     // новых записей 0x69 (= принятых move())
        uint64_t accepted = 0;      // плата приняла в FIFO
        uint64_t endpointOk = 0;    // плата отдала в USB-endpoint (= HID-отчёт ушёл на ПК)
        uint64_t rejected = 0;      // окончательный отказ (дельта вернулась в перенос)
        uint64_t immediate = 0;     // немедленная ошибка (дельта вернулась в перенос)
        uint64_t noCapacity = 0;    //   из них "нет места"
        uint64_t ackSent = 0;
        uint64_t ackRetry = 0;
        uint64_t windowFull = 0;    // move() == false: окно живых записей заполнено
        uint32_t maxLive = 0;       // живых записей, максимум
        uint32_t window = 0;        // окно этой сессии
        uint32_t lastReason = 0;
        int64_t carryX = 0;         // перенос сейчас (должен быть 0 после finish())
        int64_t carryY = 0;
    };

    RawTransport() {
        for (auto& slot : ring_) slot.ov.hEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    }
    ~RawTransport() {
        close();
        for (auto& slot : ring_) {
            if (slot.ov.hEvent) CloseHandle(slot.ov.hEvent);
        }
    }
    RawTransport(const RawTransport&) = delete;
    RawTransport& operator=(const RawTransport&) = delete;

    // Открыть порт (пустая строка - первый найденный MAKCU), подобрать скорость
    // и включить Raw. false - error объясняет причину (порт занят, плата не
    // ответила, прошивка без Raw API).
    bool open(const std::string& port, std::string& error) {
        close();
        std::string name = port;
        if (name.empty()) {
            const auto found = findMakcuPorts();
            if (found.empty()) {
                error = "MAKCU (CH343 1A86:55D3) не найден";
                return false;
            }
            name = found.front();
        }
        const std::string path = "\\\\.\\" + name;
        handle_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            error = "CreateFile(" + name + ") failed, error " + std::to_string(GetLastError()) +
                    " (порт занят другой программой?)";
            return false;
        }
        port_ = name;
        SetupComm(handle_, 4096, 4096);
        COMMTIMEOUTS timeouts{};
        // Чтение: вернуться сразу, если есть байты, иначе ждать первый байт до 50 мс.
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
        timeouts.ReadTotalTimeoutConstant = 50;
        SetCommTimeouts(handle_, &timeouts);

        failed_ = false;
        stopReader_ = false;
        reader_ = std::thread([this] { readerLoop(); });

        // Скорость UART CH343<->ESP32 должна совпадать с прошивкой.
        bool found = false;
        for (uint32_t candidate : {4000000u, 1000000u, 115200u}) {
            if (!configure(candidate)) continue;
            Sleep(20);
            if (probeDevice(std::chrono::milliseconds(300))) {
                baud_ = candidate;
                found = true;
                break;
            }
        }
        if (!found) {
            error = "MAKCU на " + name + " не ответил на DEVICE (0x02) ни на одной скорости";
            close();
            return false;
        }
        if (!prepareRaw(info_)) {
            error = info_;
            close();
            return false;
        }
        return true;
    }

    void close() {
        if (handle_ == INVALID_HANDLE_VALUE) return;
        rawMode_ = false;
        stopReader_ = true;
        CancelIoEx(handle_, nullptr);
        if (reader_.joinable()) reader_.join();
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

    [[nodiscard]] bool healthy() const { return handle_ != INVALID_HANDLE_VALUE && !failed_ && rawMode_.load(); }
    [[nodiscard]] const std::string& port() const { return port_; }
    [[nodiscard]] uint32_t baud() const { return baud_; }
    // Строка о сессии Raw: schema, last_accepted_id, свободные записи, дочистка.
    [[nodiscard]] const std::string& info() const { return info_; }

    // ---- горячий путь ---------------------------------------------------------
    // Один целый отчёт. true - принят (уйдёт ровно одним HID-отчётом);
    // false - сейчас нельзя, оставьте дельту у себя до следующего вызова.
    [[nodiscard]] bool move(int16_t dx, int16_t dy) {
        if (!healthy()) return false;
        if (dx == 0 && dy == 0) {
            (void)moveRaw(0, 0, true);
            return true;
        }
        return moveRaw(dx, dy, false);
    }

    // Только квитанции и перенос, без новой дельты (можно звать на пустых тиках).
    void service() {
        if (healthy()) (void)moveRaw(0, 0, true);
    }

    // Конец движения: квитировать живые записи и дослать перенос (до timeoutMs).
    // true - всё закрыто, перенос (0,0).
    bool finish(int timeoutMs = 300) {
        if (!healthy()) return false;
        const int64_t until = detail::Qpc::now() + detail::Qpc::fromUs(timeoutMs * 1000.0);
        while (detail::Qpc::now() < until && healthy()) {
            {
                std::lock_guard<std::mutex> lock(rawMutex_);
                if (rawLive_ == 0 && rawCarryX_ == 0 && rawCarryY_ == 0) return true;
            }
            (void)moveRaw(0, 0, true);
            Sleep(1);
        }
        std::lock_guard<std::mutex> lock(rawMutex_);
        return rawLive_ == 0 && rawCarryX_ == 0 && rawCarryY_ == 0;
    }

    [[nodiscard]] Stats stats() const {
        Stats s;
        s.writes = io_.writes.load();
        s.driverBusy = io_.deferred.load();
        s.errors = io_.errors.load();
        s.maxInflight = io_.maxInflight.load();
        s.maxCallUs = io_.maxCallUs.load();
        s.avgCallUs = s.writes ? io_.sumCallUs.load() / static_cast<double>(s.writes) : 0.0;
        s.submitted = rawStats_.submitted.load();
        s.accepted = rawStats_.accepted.load();
        s.endpointOk = rawStats_.endpointOk.load();
        s.rejected = rawStats_.rejected.load();
        s.immediate = rawStats_.immediate.load();
        s.noCapacity = rawStats_.noCapacity.load();
        s.ackSent = rawStats_.ackSent.load();
        s.ackRetry = rawStats_.ackRetry.load();
        s.windowFull = rawStats_.windowFull.load();
        s.maxLive = rawStats_.maxLive.load();
        s.lastReason = rawStats_.lastReason.load();
        std::lock_guard<std::mutex> lock(rawMutex_);
        s.window = rawWindow_;
        s.carryX = rawCarryX_;
        s.carryY = rawCarryY_;
        return s;
    }

    [[nodiscard]] std::string statsLine() const {
        const Stats s = stats();
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "raw 0x69: отправлено %llu | принято в FIFO %llu | отдано в USB-endpoint %llu | отказ %llu"
            " | немедл. ошибок %llu (нет места %llu) | ack %llu, повторов %llu | окно %u, max живых %u"
            " | ждали окно %llu | драйвер занят %llu | перенос (%lld,%lld) | посл. причина %u"
            " | вызов ср %.1f / max %.1f мкс | ошибок %llu",
            u(s.submitted), u(s.accepted), u(s.endpointOk), u(s.rejected), u(s.immediate), u(s.noCapacity),
            u(s.ackSent), u(s.ackRetry), s.window, s.maxLive, u(s.windowFull), u(s.driverBusy),
            static_cast<long long>(s.carryX), static_cast<long long>(s.carryY), s.lastReason, s.avgCallUs,
            s.maxCallUs, u(s.errors));
        return buf;
    }

    void resetStats() {
        io_.reset();
        rawStats_.reset();
    }

private:
    static constexpr size_t kFrameMax = 64;
    static constexpr uint8_t kOpDevice = 0x02;
    static constexpr uint8_t kOpRawMove = 0x69;
    static constexpr uint8_t kOpRawAck = 0x6A;

    static unsigned long long u(uint64_t v) { return static_cast<unsigned long long>(v); }

    struct IoStats {
        std::atomic<uint64_t> writes{0};
        std::atomic<uint64_t> deferred{0};
        std::atomic<uint64_t> errors{0};
        std::atomic<uint32_t> maxInflight{0};
        std::atomic<double> maxCallUs{0.0};
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
            if (us > maxCallUs.load(std::memory_order_relaxed)) maxCallUs.store(us, std::memory_order_relaxed);
        }
    };

    struct Slot {
        OVERLAPPED ov{};
        std::array<uint8_t, kFrameMax> frame{};
        size_t len = 0;
        bool used = false;
    };

    // Запись кадра(ов) в свободный слот кольца overlapped-записей.
    bool sendFrame(const uint8_t* data, size_t len) {
        Slot& slot = ring_[next_];
        if (slot.used) {
            if (!HasOverlappedIoCompleted(&slot.ov)) {
                // Драйвер ещё не отправил 32 предыдущих кадра: не копим
                // очередь (это дало бы пачки и рывки), а переносим дельту.
                io_.deferred.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (!reapSlot(slot)) return false;
        }
        std::copy(data, data + len, slot.frame.begin());
        slot.len = len;
        const int64_t t0 = detail::Qpc::now();
        if (!submit(slot)) return false;
        io_.noteCall(detail::Qpc::toUs(detail::Qpc::now() - t0));
        io_.writes.fetch_add(1, std::memory_order_relaxed);
        next_ = (next_ + 1) % ring_.size();

        uint32_t inflight = 0;
        for (const auto& s : ring_) {
            if (s.used && !HasOverlappedIoCompleted(&s.ov)) ++inflight;
        }
        if (inflight > io_.maxInflight.load(std::memory_order_relaxed)) {
            io_.maxInflight.store(inflight, std::memory_order_relaxed);
        }
        return true;
    }

    // ---- Raw movement 0x69/0x6A -----------------------------------------------
    // 0x69: u32 id, i16 x, i16 y, u16 timeout_uf -> ответ state:u8 reason:u8 id:u32
    //       (1 = в FIFO, ещё НЕ отправлено; 255 = немедленная ошибка, reason 3 = нет места).
    // 0x6A: u32 id, u8 consume -> тот же ответ (2 = отдано в USB-endpoint, 3 = отказ,
    //       255/6 = ещё в очереди). У платы не больше 16 живых/непрочитанных записей.
    // ID строго растут в пределах загрузки платы - узнаём водяной знак (id=0).
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
    static constexpr size_t kRawSlots = 64;             // индекс записи = id % 64 (> ёмкости платы 16)
    static constexpr uint32_t kRawMaxLive = 12;         // своё окно, запас до 16 записей платы
    static constexpr double kRawAckAfterUs = 2000.0;    // квитировать не раньше 2 мс после отправки
    static constexpr double kRawAckRetryUs = 1000.0;    // повтор квитирования не чаще раза в 1 мс
    static constexpr double kRawLostReplyUs = 50000.0;  // нет ответа на 0x69 за 50 мс - спросить 0x6A

    struct RawMeta {
        uint8_t schema = 0;
        uint32_t lastId = 0;
        uint8_t freeRecords = 0;
        uint8_t capacity = 0;
    };

    bool prepareRaw(std::string& info) {
        rawMode_ = false;
        auto meta = queryRawMeta();
        if (!meta) {
            info = "нет ответа на 0x6A(id=0) - прошивка без Raw API (нужна V4.074+)";
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
        if (rawWindow_ < kRawMaxLive) info += "; окно уменьшено до " + std::to_string(rawWindow_);
        return true;
    }

    // 0x6A(id=0, consume=0) -> 0:u8 schema:u8 last_accepted_id:u32 free_records:u8 capacity:u8.
    // Вызывать при rawMode_ == false (ответ читается через transact).
    std::optional<RawMeta> queryRawMeta() {
        const uint8_t query[10] = {0xDE, 0xAD, 0x05, 0x00, kOpRawAck, 0, 0, 0, 0, 0};
        const auto rx = transact(query, sizeof(query), std::chrono::milliseconds(80));
        for (size_t i = 0; i + 13 <= rx.size(); ++i) {
            if (rx[i] != 0xDE || rx[i + 1] != 0xAD || rx[i + 4] != kOpRawAck) continue;
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
                const size_t n = putRawFrame(frame, kOpRawAck, payload, sizeof(payload));
                batch.insert(batch.end(), frame, frame + n);
            }
            const auto rx = transact(batch.data(), batch.size(), std::chrono::milliseconds(15));
            for (size_t i = 0; i + 11 <= rx.size(); ++i) {
                if (rx[i] == 0xDE && rx[i + 1] == 0xAD && rx[i + 2] == 0x06 && rx[i + 3] == 0x00 &&
                    rx[i + 4] == kOpRawAck && (rx[i + 5] == 2 || rx[i + 5] == 3)) {
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

    // MAK_API: DE AD | LEN:u16 | CMD:u8 | PAYLOAD
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
        const int64_t ackAfter = detail::Qpc::fromUs(kRawAckAfterUs);
        const int64_t retry = detail::Qpc::fromUs(kRawAckRetryUs);
        const int64_t lost = detail::Qpc::fromUs(kRawLostReplyUs);
        int added = 0;
        for (auto& r : rawRec_) {
            if (added >= 3 || len + 10 > kFrameMax) break;
            const bool due =
                (r.phase == 2 && now - r.tSubmit >= ackAfter && (r.tAck == 0 || now - r.tAck >= retry)) ||
                (r.phase == 3 && now - r.tAck >= detail::Qpc::fromUs(20000.0)) ||  // ответ на ack потерян
                (r.phase == 1 && now - r.tSubmit >= lost);                         // ответ на 0x69 потерян
            if (!due) continue;
            const uint8_t payload[5] = {static_cast<uint8_t>(r.id), static_cast<uint8_t>(r.id >> 8),
                static_cast<uint8_t>(r.id >> 16), static_cast<uint8_t>(r.id >> 24), 1};
            len += putRawFrame(out + len, kOpRawAck, payload, sizeof(payload));
            r.phase = 3;
            r.tAck = now;
            rawStats_.ackSent.fetch_add(1, std::memory_order_relaxed);
            ++added;
        }
        return len;
    }

    // flushOnly: не новая дельта, только квитанции и перенос (service()/finish()).
    bool moveRaw(int16_t dx, int16_t dy, bool flushOnly) {
        if (!rawMode_.load()) {
            fail();
            return false;
        }
        std::array<uint8_t, kFrameMax> buf{};
        size_t len = 0;
        bool submitted = false;
        uint32_t id = 0;
        const int64_t now = detail::Qpc::now();
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
                len = putRawFrame(buf.data(), kOpRawMove, payload, sizeof(payload));
                submitted = true;
            } else if (!flushOnly) {
                rawStats_.windowFull.fetch_add(1, std::memory_order_relaxed);
            }
            len = appendAcksLocked(buf.data(), len, now);
        }
        if (len == 0) return false;
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
        if (submitted) rawStats_.submitted.fetch_add(1, std::memory_order_relaxed);
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
            if ((op == kOpRawMove || op == kOpRawAck) && plen == 6) {
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
            if (op == kOpRawMove) rawStats_.accepted.fetch_add(1, std::memory_order_relaxed);
            r.phase = 2;
        } else if (state == 2) {  // отдано в USB-endpoint
            if (op == kOpRawMove) rawStats_.accepted.fetch_add(1, std::memory_order_relaxed);
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

    // ---- служебный обмен (открытие, метаданные) --------------------------------
    // Отправить байты и вернуть всё, что пришло за wait.
    std::vector<uint8_t> transact(const uint8_t* data, size_t size, std::chrono::milliseconds wait) {
        {
            std::lock_guard<std::mutex> lock(rxMutex_);
            rx_.clear();
        }
        if (!writeBlocking(data, size)) return {};
        Sleep(static_cast<DWORD>(wait.count()));
        std::lock_guard<std::mutex> lock(rxMutex_);
        return rx_;
    }

    bool configure(uint32_t baud) {
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(handle_, &dcb)) return false;
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
        if (!SetCommState(handle_, &dcb)) return false;
        PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        return true;
    }

    // DEVICE (0x02) -> DE AD 01 00 02 kinds.
    bool probeDevice(std::chrono::milliseconds timeout) {
        {
            std::lock_guard<std::mutex> lock(rxMutex_);
            rx_.clear();
        }
        const uint8_t request[5] = {0xDE, 0xAD, 0x00, 0x00, kOpDevice};
        if (!writeBlocking(request, sizeof(request))) return false;
        std::unique_lock<std::mutex> lock(rxMutex_);
        return rxCv_.wait_for(lock, timeout, [&] {
            for (size_t i = 0; i + 6 <= rx_.size(); ++i) {
                if (rx_[i] == 0xDE && rx_[i + 1] == 0xAD && rx_[i + 2] == 0x01 && rx_[i + 3] == 0x00 &&
                    rx_[i + 4] == kOpDevice) {
                    return true;
                }
            }
            return false;
        });
    }

    bool writeBlocking(const uint8_t* data, size_t size) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = 0;
        BOOL ok = WriteFile(handle_, data, static_cast<DWORD>(size), nullptr, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) ok = TRUE;
        if (ok) ok = GetOverlappedResult(handle_, &ov, &n, TRUE);
        CloseHandle(ov.hEvent);
        return ok && n == size;
    }

    bool submit(Slot& slot) {
        HANDLE ev = slot.ov.hEvent;
        slot.ov = OVERLAPPED{};
        slot.ov.hEvent = ev;
        slot.used = true;
        const BOOL ok = WriteFile(handle_, slot.frame.data(), static_cast<DWORD>(slot.len), nullptr, &slot.ov);
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
        io_.errors.fetch_add(1, std::memory_order_relaxed);
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
                if (rx_.size() > 8192) rx_.erase(rx_.begin(), rx_.end() - 4096);
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
    std::string port_;
    std::string info_;
    std::atomic<bool> failed_{false};
    uint32_t baud_ = 0;
    IoStats io_;

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

}  // namespace makcu
