// MAKCU Raw transport (COM) - замороженная эталонная версия.
//
// Один заголовок, без зависимостей кроме WinAPI (SetupAPI для поиска порта).
// Доставляет целые отчёты (dx, dy) в MAKCU командой Raw movement 0x69 с
// квитанциями 0x6A: на fw с Raw API (V4.074+, проверено на 4094 и 4097) каждый
// принятый move() = ровно один HID-отчёт, мимо интерполятора прошивки.
//
// Что транспорт делает:
//   - свой overlapped-handle COM, кольцо из 32 записей WriteFile (под мьютексом), поток
//     чтения, сторожевой поток;
//   - открытие: поиск CH343 (VID 1A86 PID 55D3), проба скорости 4М/1М/115200
//     по ответу DEVICE (0x02), DTR/RTS выключены;
//   - Raw: метаданные 0x6A(id=0), дочистка записей прошлых сессий, окно
//     живых записей (<= 12 и <= свободно на плате - 2), квитанции, перенос
//     отказанных дельт, finish() в конце;
//   - чтение кнопок мыши: поток INPUT_STREAM mouse (0x52) включается в open(), события
//     INPUT_CHANGE 0x53 разбирает поток чтения, стартовая маска и пересинхронизация
//     после переполнения - GET PHYS_BUTTONS 0x55; buttons()/buttonDown()/pressCount();
//   - инжект кнопок: press()/release() - SET 0x11..0x15 сразу из вызывающего потока,
//     без очереди за движением, без тайминга и "хуманизации" (время нажатия задаёт
//     вызывающий);
//   - блокировки физического ввода: setLock() - SET LOCK 0x60, 14 целей (кнопки, оси,
//     направления, колесо). Блокировка действует только на физическую мышь: инжект её
//     обходит, а чтение кнопок (0x53/0x55) видит физику до блокировки;
//   - команды без ответа (кнопки, блокировки) уходят одной записью вместе со снимком
//     GET INJECT_SNAPSHOT 0x56: что не применилось (команда изредка теряется и по COM),
//     отправляется заново. Снимок же - пульс связи (раз в 250 мс);
//   - связь пропала (ошибка ввода-вывода: выдернут USB2; или плата молчит 750 мс):
//     move() == false, сторожевой поток закрывает порт, ждёт его появления, открывает
//     заново и восстанавливает поток событий, блокировки и зажатые кнопки; записи
//     движения, не дошедшие до платы, сбрасываются (устарели);
//   - open() снимает инжектированные кнопки и блокировки, оставшиеся от упавшей
//     программы; close() отпускает все кнопки, снимает все блокировки (с подтверждением
//     снимком) и возвращает поток событий в прежнее состояние.
// Чего транспорт НЕ делает (это забота вызывающего):
//   - темп (часы 1 мс), нарезка траектории, квантование/рендерер, время удержания
//     кнопок. Частота HID = частота ненулевых move(), не выше опроса USB клонированной мыши.
//
// Почему так (замеры 2026-10-12, подробно - README.md): по COM, в отличие от UDP, пауза
// после команды без ответа не нужна (SET -> SET/GET без паузы: 300 из 300), но SET
// изредка теряется (0.3-0.7 %, бывает кучками) - отсюда сверка снимком после каждой
// команды. Снимок по COM отвечает за ~0.3 мс.
//
// Контракт вызова:
//   - move()/service()/finish() - из ОДНОГО потока (обычно поток тайминга).
//     Никогда не блокируются на USB (finish() - до timeoutMs). Короткое ожидание
//     мьютекса кольца возможно, если другой поток в этот момент отправляет кнопку.
//   - move() == false: сейчас отправить нельзя (окно/кольцо заняты или связи нет) -
//     вызывающий оставляет дельту у себя и добавляет к следующему вызову (после
//     долгого обрыва, возможно, лучше её сбросить). Внутренний перенос (клампинг
//     >32767, отказ платы) транспорт ведёт сам.
//   - move(0, 0) ничего не отправляет на мышь, только квитанции/перенос (= service()).
//   - press()/release()/setLock()/..., buttons()/..., stats()/statsLine() - из любого потока.
//
// Ревизия 2026-10-12 (2): кнопки мыши. Ревизия 2026-10-12 (3): инжект кнопок, блокировки,
// переподключение, сброс состояния при open()/close(). Логика Raw-движения не менялась.
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
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace makcu {

namespace detail {

#ifndef MAKCU_DETAIL_QPC_DEFINED
#define MAKCU_DETAIL_QPC_DEFINED
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
#endif  // MAKCU_DETAIL_QPC_DEFINED (общие для COM- и UDP-транспорта)

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

#ifndef MAKCU_MOUSE_BUTTONS_DEFINED
#define MAKCU_MOUSE_BUTTONS_DEFINED
// Физические кнопки мыши в host-порту MAKCU: номер = бит маски PHYS_BUTTONS (0x55)
// и id события INPUT_CHANGE (0x53). Общий блок для makcu_raw_transport.h (COM) и
// makcu_udp_batch_transport.h (UDP): можно подключать оба заголовка.
enum class MouseButton : uint8_t { Left = 0, Right = 1, Middle = 2, Side1 = 3, Side2 = 4 };
inline constexpr uint8_t kMouseButtonCount = 5;
inline constexpr uint8_t buttonBit(MouseButton b) { return static_cast<uint8_t>(1u << static_cast<uint8_t>(b)); }
inline const char* buttonName(MouseButton b) {
    switch (b) {
    case MouseButton::Left: return "left";
    case MouseButton::Right: return "right";
    case MouseButton::Middle: return "middle";
    case MouseButton::Side1: return "side1";
    case MouseButton::Side2: return "side2";
    }
    return "?";
}
#endif  // MAKCU_MOUSE_BUTTONS_DEFINED

#ifndef MAKCU_LOCK_TARGET_DEFINED
#define MAKCU_LOCK_TARGET_DEFINED
// Цели блокировки физического ввода (SET/GET LOCK 0x60; бит в маске = номер цели).
// Ось целиком (X, Y, Wheel) = оба направления: плата хранит по оси два бита (+ и -),
// бит всей оси выставлен, когда заблокированы оба. Снять "+X" - значит снять и "X".
enum class LockTarget : uint8_t {
    Left = 0, Right = 1, Middle = 2, Side1 = 3, Side2 = 4,
    X = 5, XPos = 6, XNeg = 7,
    Y = 8, YPos = 9, YNeg = 10,
    Wheel = 11, WheelUp = 12, WheelDown = 13,
};
inline constexpr uint8_t kLockTargetCount = 14;
inline constexpr uint16_t lockBit(LockTarget t) { return static_cast<uint16_t>(1u << static_cast<uint8_t>(t)); }
inline const char* lockName(LockTarget t) {
    static const char* const kNames[kLockTargetCount] = {"left", "right", "middle", "side1", "side2", "X", "+X", "-X",
        "Y", "+Y", "-Y", "wheel", "wheel+", "wheel-"};
    const auto i = static_cast<uint8_t>(t);
    return i < kLockTargetCount ? kNames[i] : "?";
}
#endif  // MAKCU_LOCK_TARGET_DEFINED

class RawTransport {
public:
    static constexpr double kHeartbeatUs = 250000.0;       // снимок 0x56: сверка кнопок/блокировок + пульс
    static constexpr double kSnapLostUs = 50000.0;         // нет ответа на снимок 50 мс - потерян
    static constexpr double kStreamCheckUs = 5000000.0;    // проверка, что поток событий включён
    static constexpr double kLinkLostUs = 750000.0;        // плата молчит столько - связь потеряна
    static constexpr double kReconnectRetryUs = 250000.0;  // попытки переподключения

    // Снимок счётчиков (stats()).
    struct Stats {
        uint64_t writes = 0;        // WriteFile в драйвер (кадр = 0x69 и/или квитанции)
        uint64_t driverBusy = 0;    // move() == false: все 32 записи драйвера ещё в полёте
        uint64_t errors = 0;        // ошибок ввода-вывода (каждая - обрыв связи)
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
        // Чтение кнопок
        uint8_t buttons = 0;          // маска физических кнопок сейчас
        uint64_t buttonEvents = 0;    // событий 0x53 (мышь)
        uint64_t buttonOverflows = 0; // переполнений потока (после них - перечитывание 0x55)
        uint64_t buttonPolls = 0;     // запросов 0x55 (старт, переполнение, pollButtons())
        uint64_t buttonReplies = 0;   // ответов 0x55
        uint64_t buttonChanges = 0;   // изменений маски
        // Инжект кнопок и блокировки
        uint8_t injectWanted = 0;     // инжектированные кнопки: запрошено
        uint8_t injectOnBoard = 0;    //   по последнему снимку платы
        uint16_t locksWanted = 0;     // блокировки: запрошено
        uint16_t locksOnBoard = 0;    //   по последнему снимку платы
        uint64_t buttonCommands = 0;  // отправлено SET 0x11..0x15
        uint64_t lockCommands = 0;    // отправлено SET 0x60
        uint64_t snapshots = 0;       // снимков 0x56
        uint64_t snapshotLost = 0;    // снимков без ответа
        uint64_t repairs = 0;         // снимок разошёлся с отправленным - команды повторены
        uint64_t streamRestores = 0;  // поток событий оказался выключен - включён снова
        // Связь
        bool connected = false;
        uint64_t linkLost = 0;        // сколько раз связь терялась
        uint64_t reconnects = 0;      // сколько раз восстановлена
        double lastOutageMs = 0.0;    // длительность последнего обрыва
        uint64_t droppedRecords = 0;  // записей движения сброшено при обрывах
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
        autoPort_ = port.empty();
        std::string name = port;
        if (name.empty()) {
            const auto found = findMakcuPorts();
            if (found.empty()) {
                error = "MAKCU (CH343 1A86:55D3) не найден";
                return false;
            }
            name = found.front();
        }
        resetButtons();
        resetControls();
        resetLinkStats();
        if (!connect(name, error, true)) {
            closeHandle();
            return false;
        }
        lastRxT_.store(detail::Qpc::now(), std::memory_order_release);
        connected_ = true;
        ready_ = true;
        stopWatch_ = false;
        watchdog_ = std::thread([this] { watchdogLoop(); });
        kick();  // снять оставшееся от прошлой программы
        return true;
    }

    void close() {
        closing_ = true;
        stopWatch_ = true;
        if (watchdog_.joinable()) watchdog_.join();
        if (ready_.load() && connected_.load()) {
            flushControls(300);  // отпустить кнопки, снять блокировки
            if (streamRestore_) {
                // Вернуть INPUT_STREAM mouse, каким он был до open().
                const uint8_t set[7] = {0xDE, 0xAD, 0x02, 0x00, kOpInputStream, 0x01, *streamRestore_};
                std::lock_guard<std::mutex> lock(sendMutex_);
                (void)writeBlocking(set, sizeof(set));
            }
        }
        ready_ = false;
        connected_ = false;
        closeHandle();
        streamRestore_.reset();
        buttonsOk_ = false;
        controlsOk_ = false;
        closing_ = false;
    }

    // Можно звать move(): открыт, связь есть.
    [[nodiscard]] bool healthy() const {
        return ready_.load() && connected_.load() && rawMode_.load() && !closing_.load();
    }
    // Связь с платой есть (false - идёт переподключение).
    [[nodiscard]] bool connected() const { return connected_.load(); }
    [[nodiscard]] const std::string& port() const { return port_; }
    [[nodiscard]] uint32_t baud() const { return baud_; }
    // Версия прошивки (FIRMWARE 0x04), 0 - не ответила.
    [[nodiscard]] uint32_t firmware() const { return firmware_; }
    // Строка о сессии Raw: schema, last_accepted_id, свободные записи, дочистка.
    [[nodiscard]] const std::string& info() const { return info_; }

    // ---- чтение кнопок мыши (любой поток, не блокируют) --------------------------------
    // Маска физических кнопок: bit0 левая, bit1 правая, bit2 средняя, bit3 side1, bit4 side2.
    // Обновляется событиями 0x53 сразу по изменению (поток чтения). Физика читается и у
    // заблокированной кнопки (блокировка прячет её только от ПК).
    [[nodiscard]] uint8_t buttons() const { return mask_.load(std::memory_order_acquire); }
    [[nodiscard]] bool buttonDown(MouseButton b) const { return (buttons() & buttonBit(b)) != 0; }
    // Нажатий (переходов 0 -> 1) с open(). Разность двух чтений = нажатия между ними:
    // клик не теряется, даже если вызывающий читает buttons() редко. Кнопка, зажатая
    // уже в момент open(), нажатием не считается.
    [[nodiscard]] uint32_t pressCount(MouseButton b) const {
        return presses_[static_cast<uint8_t>(b)].load(std::memory_order_acquire);
    }
    // false - плата не ответила на 0x55 при open() (прошивка старше V4.036).
    [[nodiscard]] bool buttonsAvailable() const { return buttonsOk_.load(); }
    // Перечитать маску (0x55). Обычно не нужно: события приходят сами, после переполнения
    // транспорт перечитывает сам. false - кольцо записи занято или связи нет.
    bool pollButtons() {
        std::lock_guard<std::mutex> lock(sendMutex_);
        return pollButtonsLocked();
    }

    // ---- инжект кнопок (любой поток, не блокируют) -------------------------------------
    // Нажать/отпустить кнопку программно (SET 0x11..0x15). Уходит сразу из вызывающего
    // потока (одна запись WriteFile вместе со снимком-проверкой), движение не ждёт.
    // Время удержания и паузы задаёт вызывающий. Нажатие идёт в ПК вместе с физической
    // кнопкой (ИЛИ): отпускание физической не отменяет инжектированное нажатие.
    void press(MouseButton b) { setButtonState(b, true); }
    void release(MouseButton b) { setButtonState(b, false); }
    void setButtonState(MouseButton b, bool down) {
        if (closing_.load()) return;
        if (down) injectWanted_.fetch_or(buttonBit(b), std::memory_order_acq_rel);
        else injectWanted_.fetch_and(static_cast<uint8_t>(~buttonBit(b)), std::memory_order_acq_rel);
        kick();
    }
    void releaseAllButtons() {
        injectWanted_.store(0, std::memory_order_release);
        kick();
    }
    // Запрошенные инжектированные кнопки / что на плате по последнему снимку.
    [[nodiscard]] uint8_t injectedButtons() const { return injectWanted_.load(); }
    [[nodiscard]] uint8_t injectedButtonsOnBoard() const { return boardButtons_.load(); }
    // false - плата не ответила на снимок 0x56 при open(): команды уходят без проверки.
    [[nodiscard]] bool controlsVerified() const { return controlsOk_.load(); }

    // ---- блокировки физического ввода (любой поток, не блокируют) ---------------------
    // Заблокировать/разблокировать физическую кнопку, ось или направление (SET LOCK 0x60).
    // Заблокированное не доходит до ПК от физической мыши; инжект проходит. Проверяется
    // снимком, после переподключения восстанавливается.
    void setLock(LockTarget t, bool locked) { setLocks({t}, locked); }
    // Несколько целей разом - одной записью в порт (как в UDP-эталоне, где это одна 0x16).
    void setLocks(std::initializer_list<LockTarget> targets, bool locked) {
        if (closing_.load()) return;
        uint16_t cur = locksWanted_.load(std::memory_order_acquire);
        for (;;) {
            uint16_t next = cur;
            for (const LockTarget t : targets) next = applyLock(next, t, locked);
            if (locksWanted_.compare_exchange_weak(cur, next, std::memory_order_acq_rel)) break;
        }
        kick();
    }
    void unlockAll() {
        locksWanted_.store(0, std::memory_order_release);
        kick();
    }
    [[nodiscard]] bool locked(LockTarget t) const { return (locksWanted_.load() & lockBit(t)) != 0; }
    // Маска запрошенных блокировок (бит = номер цели) / что на плате по последнему снимку.
    [[nodiscard]] uint16_t locks() const { return locksWanted_.load(); }
    [[nodiscard]] uint16_t locksOnBoard() const { return boardLocks_.load(); }

    // ---- горячий путь ---------------------------------------------------------
    // Один целый отчёт. true - принят (уйдёт ровно одним HID-отчётом);
    // false - сейчас нельзя, оставьте дельту у себя до следующего вызова.
    [[nodiscard]] bool move(int16_t dx, int16_t dy) {
        if (!healthy()) return false;
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (!linkUpLocked()) return false;
        serviceLocked(detail::Qpc::now());
        if (dx == 0 && dy == 0) {
            (void)moveRaw(0, 0, true);
            return true;
        }
        return moveRaw(dx, dy, false);
    }

    // Только квитанции и перенос, без новой дельты (можно звать на пустых тиках).
    void service() {
        if (!healthy()) return;
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (!linkUpLocked()) return;
        serviceLocked(detail::Qpc::now());
        (void)moveRaw(0, 0, true);
    }

    // Конец движения: квитировать живые записи и дослать перенос (до timeoutMs).
    // true - всё закрыто, перенос (0,0). Кнопки и блокировки не трогает (это close()).
    bool finish(int timeoutMs = 300) {
        if (!healthy()) return false;
        const int64_t until = detail::Qpc::now() + detail::Qpc::fromUs(timeoutMs * 1000.0);
        while (detail::Qpc::now() < until && healthy()) {
            {
                std::lock_guard<std::mutex> lock(rawMutex_);
                if (rawLive_ == 0 && rawCarryX_ == 0 && rawCarryY_ == 0) return true;
            }
            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                if (linkUpLocked()) (void)moveRaw(0, 0, true);
            }
            Sleep(1);
        }
        std::lock_guard<std::mutex> lock(rawMutex_);
        return healthy() && rawLive_ == 0 && rawCarryX_ == 0 && rawCarryY_ == 0;
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
        s.buttons = mask_.load();
        s.buttonEvents = buttonEvents_.load();
        s.buttonOverflows = buttonOverflows_.load();
        s.buttonPolls = buttonPolls_.load();
        s.buttonReplies = buttonReplies_.load();
        s.buttonChanges = buttonChanges_.load();
        s.injectWanted = injectWanted_.load();
        s.injectOnBoard = boardButtons_.load();
        s.locksWanted = locksWanted_.load();
        s.locksOnBoard = boardLocks_.load();
        s.buttonCommands = buttonCommands_.load();
        s.lockCommands = lockCommands_.load();
        s.snapshots = snapshots_.load();
        s.snapshotLost = snapshotLost_.load();
        s.repairs = repairs_.load();
        s.streamRestores = streamRestores_.load();
        s.connected = connected_.load();
        s.linkLost = linkLost_.load();
        s.reconnects = reconnects_.load();
        s.lastOutageMs = lastOutageMs_.load();
        s.droppedRecords = droppedRecords_.load();
        std::lock_guard<std::mutex> lock(rawMutex_);
        s.window = rawWindow_;
        s.carryX = rawCarryX_;
        s.carryY = rawCarryY_;
        return s;
    }

    [[nodiscard]] std::string statsLine() const {
        const Stats s = stats();
        char buf[1400];
        std::snprintf(buf, sizeof(buf),
            "raw 0x69: отправлено %llu | принято в FIFO %llu | отдано в USB-endpoint %llu | отказ %llu"
            " | немедл. ошибок %llu (нет места %llu) | ack %llu, повторов %llu | окно %u, max живых %u"
            " | ждали окно %llu | драйвер занят %llu | перенос (%lld,%lld) | посл. причина %u"
            " | вызов ср %.1f / max %.1f мкс | ошибок в/в %llu"
            " | кнопки %02X: событий %llu, переполнений %llu, опросов %llu, ответов %llu, изменений %llu"
            " | инжект %02X (плата %02X), команд %llu | блокировки %04X (плата %04X), команд %llu"
            " | снимков %llu (потеряно %llu), исправлено %llu, поток событий включён заново %llu"
            " | связь %s: обрывов %llu, переподключений %llu, посл. обрыв %.0f мс, сброшено записей %llu",
            u(s.submitted), u(s.accepted), u(s.endpointOk), u(s.rejected), u(s.immediate), u(s.noCapacity),
            u(s.ackSent), u(s.ackRetry), s.window, s.maxLive, u(s.windowFull), u(s.driverBusy),
            static_cast<long long>(s.carryX), static_cast<long long>(s.carryY), s.lastReason, s.avgCallUs,
            s.maxCallUs, u(s.errors), s.buttons, u(s.buttonEvents), u(s.buttonOverflows), u(s.buttonPolls),
            u(s.buttonReplies), u(s.buttonChanges), s.injectWanted, s.injectOnBoard, u(s.buttonCommands),
            s.locksWanted, s.locksOnBoard, u(s.lockCommands), u(s.snapshots), u(s.snapshotLost), u(s.repairs),
            u(s.streamRestores), s.connected ? "есть" : "НЕТ", u(s.linkLost), u(s.reconnects), s.lastOutageMs,
            u(s.droppedRecords));
        return buf;
    }

    void resetStats() {
        io_.reset();
        rawStats_.reset();
    }

private:
    static constexpr size_t kFrameMax = 128;
    static constexpr uint8_t kOpDevice = 0x02;
    static constexpr uint8_t kOpFirmware = 0x04;
    static constexpr uint8_t kOpButtonBase = 0x11;     // SET LEFT..SIDE2 0x11..0x15 state:u8 (без ответа)
    static constexpr uint8_t kOpInputStream = 0x52;    // GET kind -> enabled; SET kind enabled (без ответа)
    static constexpr uint8_t kOpInputChange = 0x53;    // событие: kind:u8 id:u8 value:u8 (FF FF - переполнение)
    static constexpr uint8_t kOpPhysButtons = 0x55;    // V4.036+: GET -> mask:u8 физических кнопок
    static constexpr uint8_t kOpInjectSnapshot = 0x56; // GET -> 40 байт: [0] инжект-кнопки, [1..2] блокировки
    static constexpr uint8_t kOpLock = 0x60;           // SET target:u8 state:u8 (без ответа)
    static constexpr uint8_t kOpRawMove = 0x69;
    static constexpr uint8_t kOpRawAck = 0x6A;

    // Блокировки, которые отправляются платой по отдельности: кнопки и направления осей.
    // Биты осей целиком (X 5, Y 8, Wheel 11) плата выставляет сама, когда заблокированы оба направления.
    static constexpr uint16_t kLockAll = 0x3FFF;
    static constexpr uint16_t kLockSendable = 0x1F | 1u << 6 | 1u << 7 | 1u << 9 | 1u << 10 | 1u << 12 | 1u << 13;
    // Снимков в ожидании ответа не больше стольких (кольцо эпох).
    static constexpr uint32_t kSnapRing = 16;
    static constexpr uint32_t kSnapMaxOutstanding = 12;
    static constexpr DWORD kTimerHighResolution = 0x00000002;  // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Win10 1803+)

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

    // ---- блокировки: нормальная форма маски ------------------------------------------
    // У платы по оси два бита (+ и -); бит оси целиком она выставляет сама, когда стоят оба
    // (замер: "+X, затем -X" -> {X,+X,-X}; "X, затем снять +X" -> {-X}). Поэтому бит оси
    // никогда не берётся на вход, а только вычисляется из направлений.
    static uint16_t normalizeLocks(uint16_t m) {
        m &= kLockAll;
        for (const uint8_t axis : {5, 8, 11}) {
            const uint16_t whole = static_cast<uint16_t>(1u << axis);
            const uint16_t pos = static_cast<uint16_t>(1u << (axis + 1));
            const uint16_t neg = static_cast<uint16_t>(1u << (axis + 2));
            m &= static_cast<uint16_t>(~whole);
            if ((m & pos) && (m & neg)) m |= whole;
        }
        return m;
    }
    // Ось целиком = оба её направления.
    static uint16_t applyLock(uint16_t m, LockTarget t, bool on) {
        const uint8_t i = static_cast<uint8_t>(t);
        if (i >= kLockTargetCount) return m;
        uint16_t bits = static_cast<uint16_t>(1u << i);
        if (i == 5 || i == 8 || i == 11) bits = static_cast<uint16_t>(1u << (i + 1) | 1u << (i + 2));
        if (on) m |= bits;
        else m &= static_cast<uint16_t>(~bits);
        return normalizeLocks(m);
    }

    // ---- открытие, закрытие, переподключение -------------------------------------------
    // Открыть порт, подобрать скорость, Raw, кнопки, снимок. initial: open() (запомнить
    // исходный поток событий, сбросить состояние); иначе переподключение (продолжить id,
    // восстановить запрошенное). При false хэндл может остаться открытым - closeHandle().
    bool connect(const std::string& name, std::string& error, bool initial) {
        const std::string path = "\\\\.\\" + name;
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            error = "CreateFile(" + name + ") failed, error " + std::to_string(GetLastError()) +
                    " (порт занят другой программой?)";
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            handle_ = h;
            next_ = 0;
        }
        port_ = name;
        SetupComm(handle_, 4096, 4096);
        COMMTIMEOUTS timeouts{};
        // Чтение: вернуться сразу, если есть байты, иначе ждать первый байт до 50 мс.
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
        timeouts.ReadTotalTimeoutConstant = 50;
        SetCommTimeouts(handle_, &timeouts);

        ioError_ = false;
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
            return false;
        }
        std::string info;
        if (!prepareRaw(info, initial)) {
            error = info;
            return false;
        }
        prepareButtons(info, initial);
        prepareControls(info, initial);
        if (initial) info_ = info;
        return true;
    }

    // Остановить поток чтения, дождаться записей кольца, закрыть хэндл.
    void closeHandle() {
        rawMode_ = false;
        stopReader_ = true;
        if (handle_ != INVALID_HANDLE_VALUE) CancelIoEx(handle_, nullptr);
        if (reader_.joinable()) reader_.join();
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (handle_ == INVALID_HANDLE_VALUE) return;
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

    // Сторожевой поток (тик 2 мс): пульс-снимок, повтор не ушедших команд, обрыв и
    // переподключение.
    void watchdogLoop() {
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, kTimerHighResolution, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);  // старая Windows: грубее
        LARGE_INTEGER due{};
        due.QuadPart = -20000;  // 2 мс, затем период 2 мс
        if (timer) SetWaitableTimer(timer, &due, 2, nullptr, nullptr, FALSE);
        int64_t nextTry = 0;
        while (!stopWatch_.load()) {
            if (timer) WaitForSingleObject(timer, 50);
            else Sleep(2);
            if (stopWatch_.load() || closing_.load()) break;
            const int64_t now = detail::Qpc::now();
            if (connected_.load()) {
                if (ioError_.load() || now - lastRxT_.load(std::memory_order_acquire) > detail::Qpc::fromUs(kLinkLostUs)) {
                    declareLost(now);
                    nextTry = now;
                    continue;
                }
                std::lock_guard<std::mutex> lock(sendMutex_);
                if (linkUpLocked()) serviceLocked(now);
                continue;
            }
            if (now < nextTry) continue;
            nextTry = now + detail::Qpc::fromUs(kReconnectRetryUs);
            tryReconnect();
        }
        if (timer) {
            CancelWaitableTimer(timer);
            CloseHandle(timer);
        }
    }

    // Связь потеряна: закрыть порт. Записи движения, не дошедшие до платы, сбросить
    // (к моменту восстановления они устареют).
    void declareLost(int64_t now) {
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            connected_ = false;
            lostT_ = now;
            linkLost_.fetch_add(1, std::memory_order_relaxed);
        }
        closeHandle();  // поток чтения остановлен: счётчики снимков больше никто не трогает
        snapReq_ = 0;
        snapRep_ = 0;
        std::lock_guard<std::mutex> lock(rawMutex_);
        uint64_t dropped = rawLive_;
        if (rawCarryX_ != 0 || rawCarryY_ != 0) ++dropped;
        droppedRecords_.fetch_add(dropped, std::memory_order_relaxed);
        rawLive_ = 0;
        rawCarryX_ = 0;
        rawCarryY_ = 0;
        for (auto& r : rawRec_) r = RawRecord{};
    }

    // Порт появился (тот же, или первый найденный MAKCU, если open("")): открыть заново.
    void tryReconnect() {
        std::string name = port_;
        if (autoPort_) {
            const auto found = findMakcuPorts();
            if (found.empty()) return;
            if (std::find(found.begin(), found.end(), port_) == found.end()) name = found.front();
        }
        std::string err;
        if (!connect(name, err, false)) {
            closeHandle();
            return;
        }
        const int64_t back = detail::Qpc::now();
        lastRxT_.store(back, std::memory_order_release);
        lastOutageMs_.store(detail::Qpc::toUs(back - lostT_) / 1000.0);
        reconnects_.fetch_add(1, std::memory_order_relaxed);
        connected_ = true;
        kick();  // дослать запрошенные кнопки и блокировки
    }

    // close(): отпустить кнопки, снять блокировки и дождаться подтверждения снимком.
    void flushControls(int timeoutMs) {
        injectWanted_.store(0, std::memory_order_release);
        locksWanted_.store(0, std::memory_order_release);
        const int64_t until = detail::Qpc::now() + detail::Qpc::fromUs(timeoutMs * 1000.0);
        while (detail::Qpc::now() < until && connected_.load() && !ioError_.load()) {
            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                if (!linkUpLocked()) return;
                serviceLocked(detail::Qpc::now());
                const bool sentClean = sentButtons_ == 0 && sentLocks_ == 0;
                if (sentClean && !controlsOk_.load()) return;  // проверить нечем
                if (sentClean && boardEpoch_.load() == ctlEpoch_ && boardButtons_.load() == 0 &&
                    boardLocks_.load() == 0 && snapRep_.load() == snapReq_) {
                    return;
                }
            }
            Sleep(1);
        }
    }

    // ---- служебный цикл (под sendMutex_) -------------------------------------------------
    [[nodiscard]] bool linkUpLocked() const {
        return handle_ != INVALID_HANDLE_VALUE && connected_.load() && rawMode_.load() && !ioError_.load();
    }

    // Разбудить отправку из любого потока (кнопки, блокировки): отправить сразу.
    void kick() {
        if (!ready_.load()) return;
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (linkUpLocked()) serviceLocked(detail::Qpc::now());
    }

    // Под sendMutex_. По порядку: пересинхронизация кнопок после переполнения потока,
    // снимок разошёлся с отправленным, команды кнопок/блокировок (+ снимок одной записью),
    // включить поток событий заново, пульс-снимок, проверка потока событий.
    void serviceLocked(int64_t now) {
        if (buttonResync_.exchange(false, std::memory_order_acq_rel) && !pollButtonsLocked()) {
            buttonResync_.store(true, std::memory_order_release);  // кольцо занято - на следующем тике
        }
        // Снимок без ответа: забыть (эпохи кольца больше не совпадают с ответами).
        const uint32_t req = snapReq_.load(std::memory_order_acquire);
        const uint32_t rep = snapRep_.load(std::memory_order_acquire);
        if (const uint32_t outstanding = req - rep; outstanding != 0) {
            if (outstanding > kSnapRing ||
                now - snapReqT_[rep % kSnapRing].load(std::memory_order_relaxed) > detail::Qpc::fromUs(kSnapLostUs)) {
                snapshotLost_.fetch_add(std::min(outstanding, kSnapRing), std::memory_order_relaxed);
                snapRep_.store(req, std::memory_order_release);
                verifyDue_ = true;
            }
        }
        // Снимок пришёл после последней команды, а на плате не то, что отправлено (команда
        // потерялась, плату перезагрузили): разница уйдёт ниже.
        if (snapFresh_.exchange(false, std::memory_order_acq_rel)) {
            const uint8_t b = boardButtons_.load();
            const uint16_t l = boardLocks_.load();
            if (boardEpoch_.load() == ctlEpoch_ && (b != sentButtons_ || l != sentLocks_)) {
                repairs_.fetch_add(1, std::memory_order_relaxed);
                sentButtons_ = b;
                sentLocks_ = l;
            }
        }
        if (sendControlsLocked(now)) return;
        if (streamReenable_.exchange(false, std::memory_order_acq_rel)) {
            const uint8_t on[7] = {0xDE, 0xAD, 0x02, 0x00, kOpInputStream, 0x01, 0x01};
            if (sendFrame(on, sizeof(on))) streamRestores_.fetch_add(1, std::memory_order_relaxed);
            else streamReenable_ = true;
            return;
        }
        if (verifyDue_ || now >= nextSnapT_) {
            // Пульс: снимок; если плата его не знает - DEVICE 0x02 (ответ нужен только как признак связи).
            const uint8_t device[5] = {0xDE, 0xAD, 0x00, 0x00, kOpDevice};
            if (controlsOk_.load() ? sendSnapshotLocked(now, nullptr, 0) : sendFrame(device, sizeof(device))) {
                verifyDue_ = false;
                nextSnapT_ = now + detail::Qpc::fromUs(kHeartbeatUs);
            }
            return;
        }
        if (buttonsOk_.load() && now >= nextStreamCheckT_) {
            const uint8_t get[6] = {0xDE, 0xAD, 0x01, 0x00, kOpInputStream, 0x01};
            if (sendFrame(get, sizeof(get))) {
                nextStreamCheckT_ = now + detail::Qpc::fromUs(kStreamCheckUs);
                streamCheck_.store(true, std::memory_order_release);
            }
        }
    }

    // Под sendMutex_: все различия запрошенного и отправленного - одной записью, кнопки
    // первыми, в конце снимок 0x56 для проверки. Пауза после команд по COM не нужна.
    bool sendControlsLocked(int64_t now) {
        const uint8_t wantB = static_cast<uint8_t>(injectWanted_.load(std::memory_order_acquire) & 0x1F);
        const uint16_t wantL = locksWanted_.load(std::memory_order_acquire);
        const uint8_t diffB = static_cast<uint8_t>(wantB ^ sentButtons_);
        const uint16_t diffL = static_cast<uint16_t>((wantL ^ sentLocks_) & kLockSendable);
        if (!diffB && !diffL) return false;
        std::array<uint8_t, kFrameMax> buf{};
        size_t len = 0;
        uint32_t nb = 0, nl = 0;
        for (uint8_t i = 0; i < kMouseButtonCount; ++i) {
            if (!(diffB & (1u << i))) continue;
            const uint8_t f[6] = {0xDE, 0xAD, 0x01, 0x00, static_cast<uint8_t>(kOpButtonBase + i),
                static_cast<uint8_t>((wantB >> i) & 1u)};
            std::memcpy(buf.data() + len, f, sizeof(f));
            len += sizeof(f);
            ++nb;
        }
        for (uint8_t i = 0; i < kLockTargetCount; ++i) {
            if (!(diffL & (1u << i))) continue;
            const uint8_t f[7] = {0xDE, 0xAD, 0x02, 0x00, kOpLock, i, static_cast<uint8_t>((wantL >> i) & 1u)};
            std::memcpy(buf.data() + len, f, sizeof(f));
            len += sizeof(f);
            ++nl;
        }
        // 5 кнопок x 6 + 11 блокировок x 7 + снимок 5 = 112 <= kFrameMax.
        ++ctlEpoch_;
        if (!sendSnapshotLocked(now, buf.data(), len)) {
            --ctlEpoch_;
            return false;  // кольцо занято: повтор на следующем тике (move()/сторож)
        }
        sentButtons_ = wantB;
        sentLocks_ = normalizeLocks(static_cast<uint16_t>((sentLocks_ & ~kLockSendable) | (wantL & kLockSendable)));
        buttonCommands_.fetch_add(nb, std::memory_order_relaxed);
        lockCommands_.fetch_add(nl, std::memory_order_relaxed);
        return true;
    }

    // Под sendMutex_: prefix (команды) + GET 0x56 одной записью. Эпоха снимка запоминается в
    // кольце: ответы по COM приходят по порядку. Слишком много снимков в ожидании - без снимка
    // (проверка позже, verifyDue_).
    bool sendSnapshotLocked(int64_t now, const uint8_t* prefix, size_t len) {
        std::array<uint8_t, kFrameMax> buf{};
        if (len) std::memcpy(buf.data(), prefix, len);
        const bool withSnap = controlsOk_.load() && snapReq_ - snapRep_.load(std::memory_order_acquire) < kSnapMaxOutstanding;
        size_t n = len;
        const uint32_t req = snapReq_.load(std::memory_order_relaxed);
        if (withSnap) {
            const uint8_t get[5] = {0xDE, 0xAD, 0x00, 0x00, kOpInjectSnapshot};
            std::memcpy(buf.data() + n, get, sizeof(get));
            n += sizeof(get);
            snapReqEpoch_[req % kSnapRing].store(ctlEpoch_, std::memory_order_relaxed);
            snapReqT_[req % kSnapRing].store(now, std::memory_order_relaxed);
            // До записи: ответ (~0.3 мс) не должен опередить счётчик и показаться лишним.
            snapReq_.store(req + 1, std::memory_order_release);
        }
        if (n == 0) return true;  // снимки недоступны и команд нет
        if (!sendFrame(buf.data(), n)) {
            if (withSnap) snapReq_.store(req, std::memory_order_release);  // ничего не ушло - ответа не будет
            return false;
        }
        if (withSnap) snapshots_.fetch_add(1, std::memory_order_relaxed);
        else if (controlsOk_.load()) verifyDue_ = true;
        return true;
    }

    bool pollButtonsLocked() {
        if (!linkUpLocked()) return false;
        const uint8_t q[5] = {0xDE, 0xAD, 0x00, 0x00, kOpPhysButtons};
        if (!sendFrame(q, sizeof(q))) return false;
        buttonPolls_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Под sendMutex_: запись кадра(ов) в свободный слот кольца overlapped-записей.
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

    // initial == false (переподключение): id продолжаются, строго растут и после перезагрузки платы.
    bool prepareRaw(std::string& info, bool initial) {
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
            rawNextId_ = initial ? meta->lastId + 1 : std::max(rawNextId_, meta->lastId + 1);
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

    // Под sendMutex_. flushOnly: не новая дельта, только квитанции и перенос (service()/finish()).
    bool moveRaw(int16_t dx, int16_t dy, bool flushOnly) {
        if (!rawMode_.load()) return false;
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

    // Поток чтения: ответы 0x69/0x6A (длина 6), события и ответы кнопок, снимки.
    // Остальные кадры пропускаем по длине. Возвращает true, если пришёл снимок.
    bool parseRaw() {
        bool snapshot = false;
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
            const uint8_t* p = &rawRx_[i + 5];
            if ((op == kOpRawMove || op == kOpRawAck) && plen == 6) {
                onRawReply(op, p[0], p[1], le32(p + 2));
            } else if (op == kOpInputChange && plen == 3 && p[0] == 1) {
                onButtonEvent(p[1], p[2]);  // kind 1 = мышь
            } else if (op == kOpPhysButtons && plen == 1) {
                buttonReplies_.fetch_add(1, std::memory_order_relaxed);
                applyMask(p[0]);
            } else if (op == kOpInjectSnapshot && plen >= 3) {
                snapshot = onSnapshot(p) || snapshot;
            } else if (op == kOpInputStream && plen == 1 && streamCheck_.exchange(false, std::memory_order_acq_rel)) {
                if (p[0] != 1) streamReenable_.store(true, std::memory_order_release);
            }
            i += 5 + plen;
        }
        rawRx_.erase(rawRx_.begin(), rawRx_.begin() + static_cast<std::ptrdiff_t>(i));
        return snapshot;
    }

    // Поток чтения: снимок 0x56. Ответы по COM идут по порядку запросов: эпоха - из кольца.
    // Снимок без запроса (рукопожатие читает свой через transact) - пропустить.
    bool onSnapshot(const uint8_t* p) {
        const uint32_t rep = snapRep_.load(std::memory_order_acquire);
        if (rep == snapReq_.load(std::memory_order_acquire)) return false;
        const uint64_t epoch = snapReqEpoch_[rep % kSnapRing].load(std::memory_order_relaxed);
        snapRep_.store(rep + 1, std::memory_order_release);
        boardButtons_.store(static_cast<uint8_t>(p[0] & 0x1F), std::memory_order_release);
        boardLocks_.store(normalizeLocks(static_cast<uint16_t>(p[1] | p[2] << 8)), std::memory_order_release);
        boardEpoch_.store(epoch, std::memory_order_release);
        snapFresh_.store(true, std::memory_order_release);
        return true;
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
    // Отправить байты и вернуть всё, что пришло за wait. Только пока кольцо не пишет
    // (рукопожатие: connected_ == false).
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

    // Первый кадр с опкодом op и длиной plen в rx - его payload.
    static std::optional<std::vector<uint8_t>> findReply(const std::vector<uint8_t>& rx, uint8_t op, size_t plen) {
        for (size_t i = 0; i + 5 + plen <= rx.size(); ++i) {
            if (rx[i] == 0xDE && rx[i + 1] == 0xAD && rx[i + 2] == static_cast<uint8_t>(plen & 0xFF) &&
                rx[i + 3] == static_cast<uint8_t>(plen >> 8) && rx[i + 4] == op) {
                return std::vector<uint8_t>(rx.begin() + static_cast<std::ptrdiff_t>(i + 5),
                    rx.begin() + static_cast<std::ptrdiff_t>(i + 5 + plen));
            }
        }
        return std::nullopt;
    }

    // ---- кнопки -----------------------------------------------------------------------------
    // Включить поток событий мыши, запомнить прежнее состояние (initial), прочитать маску.
    // Порядок важен: поток начинается с "всё отпущено", поэтому маска читается после включения.
    void prepareButtons(std::string& info, bool initial) {
        const uint8_t get[6] = {0xDE, 0xAD, 0x01, 0x00, kOpInputStream, 0x01};
        std::optional<uint8_t> was;
        if (auto r = findReply(transact(get, sizeof(get), std::chrono::milliseconds(30)), kOpInputStream, 1)) {
            was = (*r)[0];
        }
        if (!was || *was != 1) {
            const uint8_t set[7] = {0xDE, 0xAD, 0x02, 0x00, kOpInputStream, 0x01, 0x01};
            if (writeBlocking(set, sizeof(set)) && initial) streamRestore_ = was.value_or(0);
        }
        // Ответ 0x55 разбирает и поток чтения (rawMode_): при переподключении он и применит
        // маску (изменения за время обрыва считаются как обычные).
        const uint8_t q[5] = {0xDE, 0xAD, 0x00, 0x00, kOpPhysButtons};
        buttonPolls_.fetch_add(1, std::memory_order_relaxed);
        if (auto r = findReply(transact(q, sizeof(q), std::chrono::milliseconds(30)), kOpPhysButtons, 1)) {
            if (initial) {
                // Стартовая маска - не нажатия (поток чтения мог применить тот же ответ раньше).
                mask_.store((*r)[0], std::memory_order_release);
                primed_.store(true, std::memory_order_release);
            }
            buttonsOk_ = true;
        }
        if (!initial) return;
        char buf[96];
        if (buttonsOk_) {
            std::snprintf(buf, sizeof(buf), "; кнопки: маска %02X, поток событий %s", mask_.load(),
                streamRestore_ ? "включён" : "уже был включён");
            info += buf;
        } else {
            info += "; кнопки: 0x55 не ответил (нужна V4.036+)";
        }
    }

    // Прошивка и снимок инжекта/блокировок: что сейчас на плате (после упавшей программы,
    // перезагрузки). Цикл сравнит с запрошенным и дошлёт разницу: при open() запрошено "ничего".
    void prepareControls(std::string& info, bool initial) {
        const uint8_t fw[5] = {0xDE, 0xAD, 0x00, 0x00, kOpFirmware};
        if (auto r = findReply(transact(fw, sizeof(fw), std::chrono::milliseconds(20)), kOpFirmware, 4)) {
            firmware_ = le32(r->data());
        }
        const uint8_t q[5] = {0xDE, 0xAD, 0x00, 0x00, kOpInjectSnapshot};
        std::optional<std::vector<uint8_t>> snap;
        for (int attempt = 0; attempt < 3 && !snap; ++attempt) {
            snap = findReply(transact(q, sizeof(q), std::chrono::milliseconds(20)), kOpInjectSnapshot, 40);
        }
        const uint8_t onBoardB = snap ? static_cast<uint8_t>((*snap)[0] & 0x1F) : 0;
        const uint16_t onBoardL = snap ? normalizeLocks(static_cast<uint16_t>((*snap)[1] | (*snap)[2] << 8)) : 0;
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            controlsOk_ = snap.has_value();
            sentButtons_ = onBoardB;
            sentLocks_ = onBoardL;
            boardButtons_ = onBoardB;
            boardLocks_ = onBoardL;
            ctlEpoch_ = 0;
            boardEpoch_ = 0;
            snapReq_ = 0;
            snapRep_ = 0;
            snapFresh_ = false;
            verifyDue_ = false;
            streamCheck_ = false;
            streamReenable_ = false;
            const int64_t now = detail::Qpc::now();
            nextSnapT_ = now + detail::Qpc::fromUs(kHeartbeatUs);
            nextStreamCheckT_ = now + detail::Qpc::fromUs(kStreamCheckUs);
        }
        if (!initial) return;
        char buf[200];
        std::snprintf(buf, sizeof(buf), "; прошивка %u", firmware_);
        info += buf;
        if (!snap) {
            info += "; снимок 0x56 не ответил: кнопки и блокировки без проверки";
        } else if (onBoardB || onBoardL) {
            std::snprintf(buf, sizeof(buf), "; осталось от прошлой программы: инжект %02X, блокировки %04X - снимаю",
                onBoardB, onBoardL);
            info += buf;
        }
    }

    // Поток чтения: событие кнопки мыши. FF FF - события потеряны, нужна маска заново.
    void onButtonEvent(uint8_t id, uint8_t value) {
        if (id == 0xFF && value == 0xFF) {
            buttonOverflows_.fetch_add(1, std::memory_order_relaxed);
            buttonResync_.store(true, std::memory_order_release);
            return;
        }
        if (id >= 8 || value > 1) return;  // не кнопка (например, колесо 0x20 в тест-сборках)
        buttonEvents_.fetch_add(1, std::memory_order_relaxed);
        const uint8_t bit = static_cast<uint8_t>(1u << id);
        const uint8_t old = mask_.load(std::memory_order_acquire);
        applyMask(value ? static_cast<uint8_t>(old | bit) : static_cast<uint8_t>(old & ~bit));
    }

    // Поток чтения: новая маска; переходы 0 -> 1 считаются нажатиями (после стартовой маски).
    void applyMask(uint8_t mask) {
        const uint8_t old = mask_.exchange(mask, std::memory_order_acq_rel);
        if (!primed_.exchange(true, std::memory_order_acq_rel)) return;  // первая маска - не нажатия
        if (old == mask) return;
        buttonChanges_.fetch_add(1, std::memory_order_relaxed);
        const uint8_t pressed = static_cast<uint8_t>(mask & ~old);
        for (uint8_t i = 0; i < kMouseButtonCount; ++i) {
            if (pressed & (1u << i)) presses_[i].fetch_add(1, std::memory_order_acq_rel);
        }
    }

    void resetButtons() {
        mask_.store(0, std::memory_order_release);
        primed_.store(false, std::memory_order_release);
        buttonResync_.store(false, std::memory_order_release);
        for (auto& c : presses_) c.store(0, std::memory_order_release);
        buttonEvents_ = buttonOverflows_ = buttonPolls_ = buttonReplies_ = buttonChanges_ = 0;
    }

    void resetControls() {
        injectWanted_ = 0;
        locksWanted_ = 0;
        boardButtons_ = 0;
        boardLocks_ = 0;
        snapReq_ = 0;
        snapRep_ = 0;
        buttonCommands_ = lockCommands_ = snapshots_ = snapshotLost_ = repairs_ = streamRestores_ = 0;
    }

    void resetLinkStats() {
        linkLost_ = reconnects_ = droppedRecords_ = 0;
        lastOutageMs_ = 0.0;
        connected_ = false;
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

    // Ошибка ввода-вывода (выдернут USB2, порт исчез): сторож объявит обрыв и переподключится.
    void fail() {
        if (!ioError_.exchange(true)) io_.errors.fetch_add(1, std::memory_order_relaxed);
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
                fail();
                Sleep(5);
                continue;
            }
            ok = GetOverlappedResult(handle_, &ov, &n, TRUE);
            if (!ok) {
                if (stopReader_) break;
                fail();
                Sleep(5);
                continue;
            }
            if (n == 0) continue;
            lastRxT_.store(detail::Qpc::now(), std::memory_order_release);  // пульс: плата на связи
            {
                std::lock_guard<std::mutex> lock(rxMutex_);
                rx_.insert(rx_.end(), buffer.begin(), buffer.begin() + n);
                if (rx_.size() > 8192) rx_.erase(rx_.begin(), rx_.end() - 4096);
                rxCv_.notify_all();
            }
            if (rawMode_.load(std::memory_order_relaxed)) {
                bool snapshot = false;
                {
                    std::lock_guard<std::mutex> lock(rawRxMutex_);
                    rawRx_.insert(rawRx_.end(), buffer.begin(), buffer.begin() + n);
                    snapshot = parseRaw();
                }
                // Снимок мог показать потерянную команду - дослать сразу, если кольцо свободно.
                if (snapshot && ready_.load() && connected_.load()) {
                    std::unique_lock<std::mutex> lock(sendMutex_, std::try_to_lock);
                    if (lock.owns_lock() && linkUpLocked()) serviceLocked(detail::Qpc::now());
                }
            }
        }
        CloseHandle(ov.hEvent);
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;  // меняется под sendMutex_ (и при остановленном потоке чтения)
    std::string port_;
    bool autoPort_ = true;  // open("") - при переподключении искать любой MAKCU
    std::string info_;
    uint32_t baud_ = 0;
    uint32_t firmware_ = 0;
    std::atomic<bool> ready_{false};
    std::atomic<bool> closing_{false};
    std::atomic<bool> ioError_{false};
    IoStats io_;

    std::mutex sendMutex_;  // кольцо записи, handle_ и всё, что помечено "под sendMutex_"
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

    // Чтение кнопок: маску пишет поток чтения (и open()), читают все.
    std::atomic<bool> buttonsOk_{false};
    std::atomic<bool> primed_{false};        // стартовая маска получена
    std::atomic<bool> buttonResync_{false};  // переполнение: перечитать 0x55
    std::optional<uint8_t> streamRestore_;   // INPUT_STREAM mouse до open(), если меняли
    std::atomic<bool> streamCheck_{false};     // отправлен GET 0x52 - ждём ответ
    std::atomic<bool> streamReenable_{false};  // проверка нашла поток выключенным
    std::atomic<uint8_t> mask_{0};
    std::array<std::atomic<uint32_t>, kMouseButtonCount> presses_{};
    std::atomic<uint64_t> buttonEvents_{0}, buttonOverflows_{0}, buttonPolls_{0}, buttonReplies_{0}, buttonChanges_{0};

    // Инжект и блокировки: запрошенное пишут вызывающие (любой поток), отправленное -
    // под sendMutex_, состояние платы - поток чтения по снимку.
    std::atomic<bool> controlsOk_{false};  // плата отвечает на снимок 0x56
    std::atomic<uint8_t> injectWanted_{0};
    std::atomic<uint16_t> locksWanted_{0};
    std::atomic<uint8_t> boardButtons_{0};
    std::atomic<uint16_t> boardLocks_{0};
    std::atomic<bool> snapFresh_{false};
    uint8_t sentButtons_ = 0;   // под sendMutex_
    uint16_t sentLocks_ = 0;    // под sendMutex_ (нормальная форма)
    uint64_t ctlEpoch_ = 0;     // под sendMutex_: номер последней записи с командами
    std::atomic<uint64_t> boardEpoch_{0};  // ctlEpoch_, к которому относится последний снимок
    std::atomic<uint32_t> snapReq_{0};     // снимков отправлено (пишет только под sendMutex_)
    std::atomic<uint32_t> snapRep_{0};     // снимков получено (поток чтения; сброс - под sendMutex_)
    std::array<std::atomic<uint64_t>, kSnapRing> snapReqEpoch_{};
    std::array<std::atomic<int64_t>, kSnapRing> snapReqT_{};
    bool verifyDue_ = false;       // под sendMutex_: нужен снимок
    int64_t nextSnapT_ = 0;        // под sendMutex_: пульс-снимок
    int64_t nextStreamCheckT_ = 0; // под sendMutex_: проверка потока событий
    std::atomic<uint64_t> buttonCommands_{0}, lockCommands_{0}, snapshots_{0}, snapshotLost_{0}, repairs_{0},
        streamRestores_{0};

    // Связь
    std::atomic<bool> connected_{false};
    std::atomic<int64_t> lastRxT_{0};
    int64_t lostT_ = 0;
    std::atomic<uint64_t> linkLost_{0}, reconnects_{0}, droppedRecords_{0};
    std::atomic<double> lastOutageMs_{0.0};
    std::thread watchdog_;
    std::atomic<bool> stopWatch_{false};
};

}  // namespace makcu
