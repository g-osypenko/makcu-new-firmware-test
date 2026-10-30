// MAKCU UDP batch transport (Ethernet, 0x6C) - замороженная эталонная версия.
//
// Один заголовок, без зависимостей кроме WinAPI (Winsock 2). Доставляет целые
// отчёты (dx, dy) в MAKCU по UDP командой batch 0x6C (V4.097+): до 7 Raw-записей в
// одной датаграмме, одна квитанция на пакет. Каждый принятый move() = одна запись =
// один HID-отчёт, мимо интерполятора; только если очередь отстала на maxLag записей,
// новая дельта прибавляется к последней неотправленной (путь точный).
//
// Что транспорт делает:
//   - свой UDP-сокет (обёртка RAW: 0x55 + nonce u64 + кадр MAK_API), поток приёма и
//     сторожевой поток;
//   - открытие: DEVICE 0x02 (маршрут мыши), FIRMWARE 0x04 (>= 4097), Raw-метаданные
//     0x6A(id=0), дочистка записей прошлых сессий, поток событий кнопок, сброс
//     инжектированных кнопок и блокировок (после упавшей программы);
//   - closed loop: в полёте одна команда; поток приёма по квитанции сразу шлёт
//     следующий пакет с накопившимися записями (<= 7), X = старший терминальный id из
//     квитанций (flags bit0) освобождает записи платы;
//   - непринятые платой записи (нет места) возвращаются в начало очереди;
//     нет квитанции 30 мс - опрос 0x6A(id=0): last_accepted_id показывает, что принято;
//   - finish(): дослать очередь, дождаться квитанции, освободить хвост записей;
//   - глубина очереди движения (setMaxLag, по умолчанию 5 записей) и stopMovement():
//     курсор встаёт через 3-4 мс после последнего move() / stopMovement(), а не через 8;
//   - физическая мышь (setTracking): опрос позиции платы GET 0x63, пока нет инжекта
//     движения, накопленные отсчёты - physical();
//   - чтение кнопок мыши: поток INPUT_STREAM mouse (0x52), события INPUT_CHANGE 0x53
//     сразу по нажатию; стартовая маска и страховочный опрос (раз в 50 мс) - GET
//     PHYS_BUTTONS 0x55. Если опрос находит изменения, которых не принесли события
//     (их режет брандмауэр?), опрос сам учащается до 5 мс. buttons()/buttonDown()/
//     pressCount(). ВАЖНО: события плата шлёт на наш порт С ПОРТА, РАВНОГО НАШЕМУ,
//     а не с 8080, поэтому сокет без connect();
//   - инжект кнопок: press()/release() - SET 0x11..0x15, первым в цикле (раньше
//     движения), без тайминга и "хуманизации" (время нажатия задаёт вызывающий);
//   - блокировки физического ввода: setLock(), 14 целей (кнопки, оси, направления,
//     колесо). Все направления движения уходят одной SET MOVE_MASK 0x16, колесо - одной
//     SET WHEEL_MASK 0x17, кнопки - SET LOCK 0x60 (те же биты, что у 0x60). Блокировка
//     действует только на физическую мышь: инжект её обходит, а чтение кнопок (0x53/0x55)
//     видит физику до блокировки;
//   - команды без ответа (кнопки, блокировки) проверяются снимком GET INJECT_SNAPSHOT
//     0x56: что не применилось (потерялась датаграмма, перезагрузилась плата),
//     отправляется заново. Снимок же - пульс связи (раз в 250 мс);
//   - связь пропала (плата молчит 750 мс): move() == false, сторожевой поток
//     переподключается и восстанавливает поток событий, блокировки и зажатые кнопки;
//     записи движения, не дошедшие до платы, сбрасываются (устарели);
//   - close(): отпустить все инжектированные кнопки, снять все блокировки, вернуть поток
//     событий в прежнее состояние.
// Чего транспорт НЕ делает (это забота вызывающего):
//   - темп (часы 1 мс), нарезка траектории, квантование/рендерер, время удержания
//     кнопок. Частота HID = частота ненулевых принятых move(), не выше опроса USB
//     клонированной мыши.
//
// Почему так (замеры 2026-10-11/12, подробно - README.md):
//   - каждая UDP-команда идёт на второй чип MAKCU по внутреннему каналу, который
//     везёт одну команду за раз; датаграмма, пришедшая во время передачи, молча
//     выбрасывается. Поэтому одна команда в полёте и много записей в одной команде;
//     после команды без ответа - пауза 3 мс (замер 2026-10-30: 2 мс хватает, пока мышь
//     стоит, но сразу после ответа на опрос и с движущейся рукой каждая 4-я команда
//     из серии терялась; с 3 мс - ни одной);
//   - блокировка X+Y одной 0x16 (setLocks({X, Y})): физика перестаёт доходить до Windows
//     через 1.3-2 мс (max 2.5). Четырьмя 0x60 через 2 мс было 7.7 мс (max 12.3) -
//     направление, заблокированное последним, текло всё это время;
//   - квитанция сообщает не больше 7 терминальных строк, поэтому пакет <= 7 записей.
//
// Контракт вызова:
//   - move()/service()/finish() - из ОДНОГО потока (обычно поток тайминга).
//     Никогда не блокируются на сети (finish() - до timeoutMs).
//   - move() == false: очередь полна (7 записей ждут пакета) или связи нет - вызывающий
//     оставляет дельту у себя (после долгого обрыва, возможно, лучше её сбросить).
//   - move(0, 0) ничего не отправляет на мышь (= service()).
//   - press()/release()/setLock()/... , buttons()/..., stats()/statsLine() - из любого потока.
//
// Ревизия 2026-10-12 (2): чтение кнопок мыши. Ревизия 2026-10-12 (3): инжект кнопок,
// блокировки, переподключение, сброс состояния при open()/close(). Логика движения
// не менялась. Ревизия 2026-10-30 (4): блокировки направлений движения одной 0x16
// (колеса - одной 0x17) вместо серии 0x60, пауза после команды без ответа 3 мс.
// Ревизия 2026-10-30 (5): глубина очереди движения (setMaxLag), stopMovement(),
// отсчёты физической мыши по позиции платы (setTracking/physical).
//
// Происхождение: basics/udp_way/makcu_udp_transport.h (RawUdpTransport,
// Format::Batch) от 2026-10-12. Не править: новая версия - новая папка.
#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

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

inline uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}
inline void put16(uint8_t* p, int32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

}  // namespace detail

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

class UdpBatchTransport {
public:
    static constexpr uint16_t kDefaultPort = 8080;
    static constexpr uint32_t kMinFirmware = 4097;  // первая сборка с 0x6C
    static constexpr size_t kMaxRecords = 7;        // записей в пакете: квитанция освобождает <= 7 строк
    static constexpr size_t kMaxPending = 7;        // очередь move(); полна - move() == false
    static constexpr uint32_t kDefaultMaxLag = 5;   // глубина очереди движения, записей (setMaxLag): замер 2026-10-30
    static constexpr int32_t kMergeLimit = 2047;    // склейка не больше этого по оси (поле HID >= 12 бит)
    static constexpr double kLostUs = 30000.0;      // нет квитанции 30 мс - спросить last_accepted_id
    static constexpr double kDefaultButtonPollMs = 50.0;  // страховочный опрос 0x55 (0 - выключить)
    static constexpr double kFallbackPollMs = 5.0;        // опрос, если события 0x53 не доходят
    static constexpr uint32_t kMissedForFallback = 2;     // изменений, найденных только опросом
    static constexpr double kPollLostUs = 10000.0;        // нет ответа на опрос/снимок 10 мс - потерян
    static constexpr double kNoReplyGapUs = 3000.0;       // пауза после команды без ответа (2 мс мало, замер 2026-10-30)
    static constexpr double kHeartbeatUs = 250000.0;      // снимок 0x56: сверка кнопок/блокировок + пульс
    static constexpr double kStreamCheckUs = 5000000.0;   // проверка, что поток событий включён
    static constexpr int kTrackScreen = 32767;            // km.screen на время слежения: позиция в центре
    static constexpr int kTrackCentre = 16383;            //   (km.screen ставит её в середину), запас +-16k
    static constexpr int kRecentreIdle = 8000;            // дальше от центра - перецентровать, когда мышь стоит
    static constexpr int kRecentreForce = 14000;          //   дальше - сразу (иначе край обрежет движение)
    static constexpr double kPosLostUs = 5000.0;          // нет ответа на 0x63 5 мс - потерян
    static constexpr double kLockSettleUs = 5000.0;       // оси заблокированы (снимок): ещё столько опрашивать
    static constexpr double kInjectSettleUs = 2000.0;     // запас после очереди платы до первого опроса
    static constexpr double kLinkLostUs = 750000.0;       // плата молчит столько - связь потеряна
    static constexpr double kReconnectRetryUs = 250000.0; // попытки переподключения

    // Снимок счётчиков (stats()).
    struct Stats {
        uint64_t records = 0;        // записей (= HID-отчётов); move(), прибавленные к записи, - в merged
        uint64_t packets = 0;        // отправлено пакетов 0x6C
        uint64_t admitted = 0;       // записей, принятых платой
        uint64_t receipts = 0;       // квитанций
        uint64_t partial = 0;        // result 1: часть не принята (обычно reason 3 = нет места)
        uint64_t envelope = 0;       // result 255: пакет отклонён целиком
        uint64_t xRejected = 0;      // 255/1: X больше сообщённого (транспорт сбрасывает X)
        uint64_t badReplies = 0;     // ответ не похож на квитанцию 0x6C
        uint64_t lostReceipts = 0;   // нет квитанции kLostUs - разобрано опросом
        uint64_t metaQueries = 0;    // опросов 0x6A(id=0) после потери
        uint64_t lateReplies = 0;    // ответ на уже разобранный пакет/опрос
        uint64_t freed = 0;          // освобождено платой по X
        uint64_t termSubmitted = 0;  // терминальных строк: отдано в USB-endpoint
        uint64_t termRejected = 0;   // терминальных строк: отказ
        uint64_t moreSet = 0;        // квитанций с флагом more
        uint64_t busy = 0;           // move() == false из-за полной очереди
        uint64_t merged = 0;         // move() прибавлен к последней неотправленной записи (очередь >= maxLag)
        uint64_t stopped = 0;        // записей выброшено stopMovement()
        uint32_t boardQueued = 0;    // в очереди платы по последней квитанции
        uint32_t maxBoardQueued = 0; // максимум в очереди платы
        uint32_t maxLag = 0;         // максимум оценки очереди движения при move()
        uint64_t sendErrors = 0;     // send() не прошёл (занят буфер или нет сети)
        uint64_t datagrams = 0;      // всех датаграмм, включая служебные
        uint32_t minFree = 256;      // минимум свободных записей платы по квитанциям (256 - квитанций не было)
        uint32_t maxPending = 0;     // максимум записей в очереди
        uint32_t pending = 0;        // записей в очереди сейчас
        bool inFlight = false;       // пакет ждёт квитанции
        uint32_t lastReason = 0;     // последняя причина отказа из квитанции
        double rttAvgMs = 0.0;       // квитанция: средняя и максимальная задержка
        double rttMaxMs = 0.0;
        // Чтение кнопок
        uint8_t buttons = 0;           // маска физических кнопок сейчас
        uint64_t buttonEvents = 0;     // событий 0x53 (мышь)
        uint64_t buttonOverflows = 0;  // переполнений потока событий (после них - опрос сразу)
        uint64_t buttonPolls = 0;      // опросов 0x55
        uint64_t buttonReplies = 0;    // ответов на опрос
        uint64_t buttonLost = 0;       // опросов без ответа kPollLostUs
        uint64_t buttonMissed = 0;     // изменений, которые нашёл опрос, а не событие
        uint64_t buttonChanges = 0;    // изменений маски
        bool eventsMissing = false;    // событий нет, а опрос видит нажатия: опрос учащён до 5 мс
        // Инжект кнопок и блокировки
        uint8_t injectWanted = 0;      // инжектированные кнопки: запрошено
        uint8_t injectOnBoard = 0;     //   по последнему снимку платы
        uint16_t locksWanted = 0;      // блокировки: запрошено
        uint16_t locksOnBoard = 0;     //   по последнему снимку платы
        uint64_t buttonCommands = 0;   // отправлено SET 0x11..0x15
        uint64_t lockCommands = 0;     // отправлено команд блокировки (0x16, 0x17, 0x60)
        uint64_t snapshots = 0;        // снимков 0x56
        uint64_t snapshotLost = 0;     // снимков без ответа
        uint64_t repairs = 0;          // снимок разошёлся с отправленным - команды повторены
        uint64_t streamRestores = 0;   // поток событий оказался выключен - включён снова
        // Связь
        bool connected = false;
        uint64_t linkLost = 0;         // сколько раз связь терялась
        uint64_t reconnects = 0;       // сколько раз восстановлена
        double lastOutageMs = 0.0;     // длительность последнего обрыва
        uint64_t droppedRecords = 0;   // записей движения сброшено при обрывах
        // Физическая мышь (setTracking)
        bool tracking = false;
        uint64_t posPolls = 0;         // опросов 0x63
        uint64_t posReplies = 0;       // ответов
        uint64_t posLost = 0;          // без ответа kPosLostUs
        uint64_t recentres = 0;        // km.screen заново (позиция ушла далеко от центра)
        uint64_t clamped = 0;          // ответов с позицией на краю (движение за краем потеряно)
    };

    UdpBatchTransport() = default;
    ~UdpBatchTransport() { close(); }
    UdpBatchTransport(const UdpBatchTransport&) = delete;
    UdpBatchTransport& operator=(const UdpBatchTransport&) = delete;

    // host - IP или имя платы, port - UDP-порт команд (8080). false + причина в err.
    bool open(const std::string& host, uint16_t port, std::string& err) {
        close();
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            err = "WSAStartup failed";
            return false;
        }
        wsaStarted_ = true;
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &remote.sin_addr) != 1) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* res = nullptr;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
                err = "не удалось разрешить адрес " + host;
                close();
                return false;
            }
            remote.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
        sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCKET) {
            err = "socket() failed, WSA " + std::to_string(WSAGetLastError());
            close();
            return false;
        }
        // ICMP "порт недоступен" иначе ломает recv() ошибкой WSAECONNRESET.
        BOOL off = FALSE;
        DWORD bytes = 0;
        WSAIoctl(sock_, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
        int rcvbuf = 1 << 20;
        setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
        // Без connect(): события 0x53 приходят с другого порта платы (см. шапку).
        remote_ = remote;
        sockaddr_in local{};
        local.sin_family = AF_INET;
        if (::bind(sock_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            err = "bind() failed, WSA " + std::to_string(WSAGetLastError());
            close();
            return false;
        }
        u_long nonblocking = 1;
        ioctlsocket(sock_, FIONBIO, &nonblocking);
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &remote.sin_addr, ip, sizeof(ip));
        endpoint_ = std::string(ip) + ":" + std::to_string(port);
        // Случайная база nonce: поздние ответы прошлых сессий не совпадут.
        seq_ = (static_cast<uint64_t>(detail::Qpc::now()) << 20) ^ static_cast<uint64_t>(GetCurrentProcessId());
        failed_ = false;
        stopRx_ = false;
        resetButtons();
        resetControls();
        resetLinkStats();
        rx_ = std::thread([this] { rxLoop(); });

        if (!handshake(err, true)) {
            close();
            return false;
        }
        lastRxT_.store(detail::Qpc::now(), std::memory_order_release);
        connected_ = true;
        ready_ = true;
        watchdog_ = std::thread([this] { watchdogLoop(); });
        return true;
    }

    void close() {
        if (ready_.load()) {
            closing_ = true;
            if (connected_.load()) {
                flushControls(300);  // отпустить кнопки, снять блокировки
                if (streamRestore_) {
                    // Вернуть INPUT_STREAM mouse, каким он был до open().
                    const uint8_t restore[2] = {0x01, *streamRestore_};
                    (void)sendNoReply(kOpInputStream, restore, 2);
                }
            }
            std::lock_guard<std::mutex> tl(trackMutex_);
            trackOn_ = false;
            restoreScreenLocked();
        }
        ready_ = false;
        stopRx_ = true;
        if (watchdog_.joinable()) watchdog_.join();
        if (rx_.joinable()) rx_.join();
        closing_ = false;
        connected_ = false;
        streamRestore_.reset();
        screenRestore_.reset();
        trackOn_ = false;
        streamOk_ = false;
        buttonsOk_ = false;
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        if (wsaStarted_) {
            WSACleanup();
            wsaStarted_ = false;
        }
    }

    // Можно звать move(): открыт, связь есть, протокол не нарушен.
    [[nodiscard]] bool healthy() const {
        return sock_ != INVALID_SOCKET && ready_.load() && connected_.load() && !failed_.load() && !closing_.load();
    }
    // Связь с платой есть (false - идёт переподключение).
    [[nodiscard]] bool connected() const { return connected_.load(); }
    [[nodiscard]] const std::string& endpoint() const { return endpoint_; }
    [[nodiscard]] uint32_t firmware() const { return firmware_; }
    [[nodiscard]] const std::string& info() const { return info_; }

    // ---- чтение кнопок мыши (любой поток, не блокируют) --------------------------------
    // Маска физических кнопок: bit0 левая, bit1 правая, bit2 средняя, bit3 side1, bit4 side2.
    // Обновляется событием 0x53 сразу по изменению (~1 мс); страховочный опрос 0x55
    // исправляет маску, если событие потерялось (UDP) или события не доходят совсем.
    // Физика читается и у заблокированной кнопки (блокировка прячет её только от ПК).
    [[nodiscard]] uint8_t buttons() const { return mask_.load(std::memory_order_acquire); }
    [[nodiscard]] bool buttonDown(MouseButton b) const { return (buttons() & buttonBit(b)) != 0; }
    // Нажатий (переходов 0 -> 1) с open(). Разность двух чтений = нажатия между ними:
    // так не теряются клики, если вызывающий читает buttons() редко. Кнопка, зажатая
    // уже при open(), нажатием не считается.
    [[nodiscard]] uint32_t pressCount(MouseButton b) const {
        return presses_[static_cast<uint8_t>(b)].load(std::memory_order_acquire);
    }
    // false - прошивка не ответила на 0x55 (кнопки не обновляются).
    [[nodiscard]] bool buttonsAvailable() const { return buttonsOk_.load(); }
    // Интервал страховочного опроса 0x55, мс (по умолчанию 50; 0 - не опрашивать совсем).
    // Каждый опрос занимает один оборот команды (~1 мс). Если события не доходят, опрос
    // сам идёт не реже kFallbackPollMs (5 мс = 200 опросов/с, движению 1000 Гц хватает).
    void setButtonPollMs(double ms) {
        pollTicks_.store(ms > 0.0 ? detail::Qpc::fromUs(ms * 1000.0) : 0, std::memory_order_release);
    }

    // ---- физическая мышь: позиция платы GET 0x63 --------------------------------------
    // Плата ведёт позицию по каждому отчёту мыши, который принял ПК: физика плюс наш инжект,
    // без того, что спрятала блокировка. Пока включено слежение, транспорт опрашивает её в
    // свободные слоты цикла, ПОКА НЕТ ИНЖЕКТА ДВИЖЕНИЯ, и копит разности - это отсчёты
    // физической мыши. Замер 2026-10-30: ~500 опросов/с, сумма совпадает с Raw Input до
    // отсчёта, между опросами 1-3 отчёта мыши (каждый отчёт отдельно плата не отдаёт).
    // Инжект: первая же запись move() закрывает отрезок; следующий начнётся, когда плата
    // отдаст свою очередь. Под полной блокировкой осей позиция не меняется - опрос встаёт
    // через kLockSettleUs после подтверждения (утёкшие до блокировки отсчёты учтены).
    // Опрос занимает единственный слот: move()/press()/setLocks() ждут ответа на него (до ~2 мс).
    // На время слежения km.screen = 32767x32767, при выключении и в close() - прежний
    // (km.moveto других программ это заденет).
    struct Physical {
        int64_t x = 0;          // отсчёты физической мыши с setTracking(true)
        int64_t y = 0;
        uint64_t samples = 0;   // ответов 0x63
        int64_t sampleQpc = 0;  // когда пришёл последний (QueryPerformanceCounter)
        bool live = false;      // отрезок идёт: последний ответ засчитан и ему меньше 10 мс
    };
    // Включить/выключить слежение. Блокирует до ~0.3 с (запрос km.screen). false - нет связи.
    bool setTracking(bool on) {
        std::lock_guard<std::mutex> tl(trackMutex_);
        if (on == trackOn_.load()) return true;
        if (!on) {
            trackOn_ = false;
            restoreScreenLocked();
            return true;
        }
        if (!healthy()) return false;
        auto r = query(kOpScreen, nullptr, 0, 100, 3);
        if (!r || r->size() < 4) return false;
        screenRestore_ = std::make_pair(static_cast<uint16_t>((*r)[0] | (*r)[1] << 8),
            static_cast<uint16_t>((*r)[2] | (*r)[3] << 8));
        setTrackScreen();
        {
            std::lock_guard<std::mutex> pl(posMutex_);
            physX_ = physY_ = 0;
            posSamples_ = 0;
            posBaseValid_ = false;
        }
        trackOn_ = true;
        kick();
        return true;
    }
    [[nodiscard]] bool tracking() const { return trackOn_.load(); }
    [[nodiscard]] Physical physical() const {
        std::lock_guard<std::mutex> pl(posMutex_);
        Physical r;
        r.x = physX_;
        r.y = physY_;
        r.samples = posSamples_;
        r.sampleQpc = posSampleT_;
        r.live = trackOn_.load() && posBaseValid_ && detail::Qpc::now() - posSampleT_ < detail::Qpc::fromUs(10000.0);
        return r;
    }

    // ---- инжект кнопок (любой поток, не блокируют) -------------------------------------
    // Нажать/отпустить кнопку программно (SET 0x11..0x15). Уходит следующей командой цикла,
    // раньше движения из очереди (ждёт только команду, которая уже в полёте: ~1-2 мс).
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
    // Запрошенные инжектированные кнопки / что на плате по последнему снимку (до 250 мс).
    [[nodiscard]] uint8_t injectedButtons() const { return injectWanted_.load(); }
    [[nodiscard]] uint8_t injectedButtonsOnBoard() const { return boardButtons_.load(); }

    // ---- блокировки физического ввода (любой поток, не блокируют) ---------------------
    // Заблокировать/разблокировать физическую кнопку, ось или направление.
    // Заблокированное не доходит до ПК от физической мыши; инжект проходит. Применяется
    // командой цикла, проверяется снимком, после переподключения восстанавливается.
    void setLock(LockTarget t, bool locked) { setLocks({t}, locked); }
    // Несколько целей разом: все направления движения из них уйдут ОДНОЙ командой 0x16
    // (физика встаёт через ~2 мс). setLock(X), затем setLock(Y) - две команды через 3 мс.
    void setLocks(std::initializer_list<LockTarget> targets, bool locked) {
        if (closing_.load()) return;
        uint16_t cur = locksWanted_.load(std::memory_order_acquire);
        for (;;) {
            uint16_t next = cur;
            for (const LockTarget t : targets) next = applyLock(next, t, locked);
            if (locksWanted_.compare_exchange_weak(cur, next, std::memory_order_acq_rel)) {
                if ((next ^ cur) & kLockMoveDirs) fullLockT_.store(0, std::memory_order_release);
                break;
            }
        }
        kick();
    }
    void unlockAll() {
        locksWanted_.store(0, std::memory_order_release);
        fullLockT_.store(0, std::memory_order_release);
        kick();
    }
    [[nodiscard]] bool locked(LockTarget t) const { return (locksWanted_.load() & lockBit(t)) != 0; }
    // Маска запрошенных блокировок (бит = номер цели) / что на плате по последнему снимку.
    [[nodiscard]] uint16_t locks() const { return locksWanted_.load(); }
    [[nodiscard]] uint16_t locksOnBoard() const { return boardLocks_.load(); }

    // ---- глубина очереди и остановка инжекта (любой поток) ------------------------------
    // Всё, что принято move() и ещё не ушло в USB (очередь транспорта, пакет в полёте,
    // очередь платы), выходит в ПК по одной записи на отчёт мыши (1 мс). Это отставание
    // курсора от вызова и хвост после остановки. Плата отдаёт ровно столько, сколько мы
    // шлём, поэтому каждая заминка сети оставляла запись в очереди навсегда: за 10 с хвост
    // дорастал до 8 мс. Если оценка очереди дошла до maxLag записей, новая дельта
    // прибавляется к последней неотправленной записи: путь точный, отчётов на один меньше.
    // 0 - не ограничивать (каждый move() - отдельный отчёт).
    void setMaxLag(uint32_t records) { maxLag_.store(records, std::memory_order_release); }
    [[nodiscard]] uint32_t maxLag() const { return maxLag_.load(); }
    // Остановить инжект сейчас: записи, ещё не отправленные плате, выбрасываются - их путь
    // в ПК не попадёт. Пакет в полёте и очередь платы доиграют (отозвать их у платы нельзя).
    // Возвращает число выброшенных записей, в dx/dy - их сумму.
    uint32_t stopMovement(int32_t* droppedDx = nullptr, int32_t* droppedDy = nullptr) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        int32_t sx = 0, sy = 0;
        for (const Rec& r : pend_) {
            sx += r.dx;
            sy += r.dy;
        }
        const auto n = static_cast<uint32_t>(pend_.size());
        pend_.clear();
        st_.stopped += n;
        if (droppedDx) *droppedDx = sx;
        if (droppedDy) *droppedDy = sy;
        return n;
    }

    // ---- горячий путь -------------------------------------------------------------
    // true - дельта принята: отдельной записью (= одним HID-отчётом) или прибавлена к
    // последней неотправленной, если очередь доросла до maxLag. false - очередь полна или
    // связи нет: дельту хранит вызывающий.
    [[nodiscard]] bool move(int16_t dx, int16_t dy) {
        if (!healthy()) return false;
        if (dx == 0 && dy == 0) {
            service();
            return true;
        }
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            // Оценка очереди: у платы по квитанции минус отданное с тех пор (1 запись/мс),
            // плюс пакет в полёте, плюс неотправленные.
            const int64_t sinceReceipt = detail::Qpc::now() - receiptT_;
            const int64_t emitted = sinceReceipt / detail::Qpc::fromUs(1000.0);
            const int64_t onBoard = std::max<int64_t>(0, static_cast<int64_t>(boardQueued_) - emitted);
            const auto lag = static_cast<uint32_t>(onBoard + (flight_.active ? flight_.n : 0) + pend_.size());
            st_.maxLag = std::max(st_.maxLag, lag);
            const uint32_t cap = maxLag_.load(std::memory_order_relaxed);
            if (cap && lag >= cap && !pend_.empty()) {
                Rec& last = pend_.back();
                const int32_t nx = last.dx + dx, ny = last.dy + dy;
                if (std::abs(nx) <= kMergeLimit && std::abs(ny) <= kMergeLimit) {
                    last.dx = nx;
                    last.dy = ny;
                    ++st_.merged;
                    accepted = true;
                }
            }
            if (!accepted && pend_.size() < kMaxPending) {
                pend_.push_back(Rec{dx, dy});
                ++st_.records;
                st_.maxPending = std::max<uint32_t>(st_.maxPending, static_cast<uint32_t>(pend_.size()));
                accepted = true;
            } else if (!accepted) {
                ++st_.busy;
            }
        }
        if (accepted) posEpoch_.fetch_add(1, std::memory_order_acq_rel);  // инжект: отрезок физики закрыт
        service();
        return accepted;
    }

    // Отправить команду, если предыдущая уже получила ответ (обычно это делает поток приёма).
    void service() {
        if (!ready_.load()) return;
        std::unique_lock<std::mutex> lock(sendMutex_, std::try_to_lock);
        if (lock.owns_lock()) (void)pumpLocked();
    }

    // Конец движения: дослать очередь, дождаться квитанции, освободить записи платы.
    // true - всё отдано и плата чистая. Кнопки и блокировки не трогает (это close()).
    bool finish(int timeoutMs = 300) {
        const int64_t until = detail::Qpc::now() + detail::Qpc::fromUs(timeoutMs * 1000.0);
        bool done = false;
        while (detail::Qpc::now() < until && healthy()) {
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                done = pend_.empty() && !flight_.active && batchSeq_.load() == 0 && metaSeq_.load() == 0;
            }
            if (done) break;
            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                (void)pumpLocked();
            }
            Sleep(1);
        }
        if (!done || !healthy()) return false;
        Sleep(30);  // последние записи уходят в USB по одной на опрос мыши
        auto meta = queryMeta();
        if (!meta) return false;
        if (meta->freeRecords < meta->capacity && meta->lastId > 0) {
            (void)consumeLeftovers(meta->lastId, meta->capacity - meta->freeRecords);
            meta = queryMeta();
        }
        return meta && meta->freeRecords == meta->capacity;
    }

    // ---- статистика ------------------------------------------------------------------
    [[nodiscard]] Stats stats() const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        Stats s = st_;
        s.pending = static_cast<uint32_t>(pend_.size());
        s.inFlight = flight_.active;
        s.datagrams = datagrams_.load();
        s.sendErrors = sendErrors_.load();
        s.rttAvgMs = rttCount_ ? rttSumUs_ / static_cast<double>(rttCount_) / 1000.0 : 0.0;
        s.rttMaxMs = rttMaxUs_ / 1000.0;
        s.buttons = mask_.load();
        s.buttonEvents = buttonEvents_.load();
        s.buttonOverflows = buttonOverflows_.load();
        s.buttonPolls = buttonPolls_.load();
        s.buttonReplies = buttonReplies_.load();
        s.buttonLost = buttonLost_.load();
        s.buttonMissed = buttonMissed_.load();
        s.buttonChanges = buttonChanges_.load();
        s.eventsMissing = eventsMissing();
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
        s.tracking = trackOn_.load();
        s.posPolls = posPolls_.load();
        s.posLost = posLost_.load();
        s.recentres = recentres_.load();
        {
            std::lock_guard<std::mutex> pl(posMutex_);
            s.posReplies = posReplies_;
            s.clamped = clamped_;
        }
        return s;
    }

    [[nodiscard]] std::string statsLine() const {
        const Stats s = stats();
        char buf[2200];
        std::snprintf(buf, sizeof(buf),
            "batch 0x6C %s: записей %llu, пакетов %llu (%.2f/пакет), принято платой %llu | квитанций %llu, частичных %llu,"
            " отклонено %llu (X %llu), непонятных %llu | потеряно квитанций %llu, опросов %llu, поздних %llu"
            " | освобождено %llu, мин. свободно %d, more %llu | терминальных: отдано %llu, отказ %llu"
            " | очередь max %u, сейчас %u, полна %llu раз | глубина max %u (предел %u), склеено %llu, остановкой"
            " выброшено %llu, у платы max %u | RTT ср %.2f max %.2f мс | датаграмм %llu, ошибок send %llu"
            " | посл. причина %u | кнопки %02X: событий %llu, переполнений %llu, опросов %llu, ответов %llu,"
            " потеряно %llu, найдено опросом %llu, изменений %llu%s"
            " | инжект %02X (плата %02X), команд %llu | блокировки %04X (плата %04X), команд %llu"
            " | снимков %llu (потеряно %llu), исправлено %llu, поток событий включён заново %llu"
            " | связь %s: обрывов %llu, переподключений %llu, посл. обрыв %.0f мс, сброшено записей %llu"
            " | физика %s: опросов 0x63 %llu, ответов %llu, потеряно %llu, перецентровок %llu, у края %llu",
            endpoint_.c_str(), u(s.records), u(s.packets),
            s.packets ? static_cast<double>(s.records) / static_cast<double>(s.packets) : 0.0, u(s.admitted),
            u(s.receipts), u(s.partial), u(s.envelope), u(s.xRejected), u(s.badReplies), u(s.lostReceipts),
            u(s.metaQueries), u(s.lateReplies), u(s.freed), s.minFree > 255 ? -1 : static_cast<int>(s.minFree),
            u(s.moreSet), u(s.termSubmitted), u(s.termRejected), s.maxPending, s.pending, u(s.busy), s.maxLag,
            maxLag_.load(), u(s.merged), u(s.stopped), s.maxBoardQueued, s.rttAvgMs,
            s.rttMaxMs, u(s.datagrams), u(s.sendErrors), s.lastReason, s.buttons, u(s.buttonEvents),
            u(s.buttonOverflows), u(s.buttonPolls), u(s.buttonReplies), u(s.buttonLost), u(s.buttonMissed),
            u(s.buttonChanges), s.eventsMissing ? " | СОБЫТИЯ НЕ ДОХОДЯТ (брандмауэр?) - опрос 5 мс" : "",
            s.injectWanted, s.injectOnBoard, u(s.buttonCommands), s.locksWanted, s.locksOnBoard, u(s.lockCommands),
            u(s.snapshots), u(s.snapshotLost), u(s.repairs), u(s.streamRestores), s.connected ? "есть" : "НЕТ",
            u(s.linkLost), u(s.reconnects), s.lastOutageMs, u(s.droppedRecords), s.tracking ? "вкл" : "выкл",
            u(s.posPolls), u(s.posReplies), u(s.posLost), u(s.recentres), u(s.clamped));
        return buf;
    }

private:
    static constexpr uint8_t kOpDevice = 0x02;
    static constexpr uint8_t kOpFirmware = 0x04;
    static constexpr uint8_t kOpButtonBase = 0x11;   // SET LEFT..SIDE2 0x11..0x15 state:u8 (без ответа)
    static constexpr uint8_t kOpInputStream = 0x52;  // GET kind -> enabled; SET kind enabled (без ответа)
    static constexpr uint8_t kOpInputChange = 0x53;  // событие: kind:u8 id:u8 value:u8 (FF FF - переполнение)
    static constexpr uint8_t kOpPhysButtons = 0x55;  // V4.036+: GET -> mask:u8 физических кнопок
    static constexpr uint8_t kOpInjectSnapshot = 0x56;  // GET -> 40 байт: [0] инжект-кнопки, [1..2] блокировки
    static constexpr uint8_t kOpMoveMask = 0x16;     // SET left:u8 right:u8 down:u8 up:u8 (без ответа) = биты 7 6 9 10
    static constexpr uint8_t kOpWheelMask = 0x17;    // SET down:u8 up:u8 (без ответа) = биты 13 12
    static constexpr uint8_t kOpLock = 0x60;         // SET target:u8 state:u8 (без ответа); GET target -> state
    static constexpr uint8_t kOpGetPos = 0x63;       // GET -> x:u16 y:u16 позиция платы (зажата в km.screen)
    static constexpr uint8_t kOpScreen = 0x64;       // GET -> w:u16 h:u16; SET w h (без ответа): позиция в центр
    static constexpr uint8_t kOpRawAck = 0x6A;       // 0x6A(id=0): метаданные; (id, consume=1): освободить запись
    static constexpr uint8_t kOpBatch = 0x6C;        // V4.097+: пакет Raw-записей + квитанция

    // Блокировки, которые отправляются платой по отдельности: кнопки и направления осей.
    // Биты осей целиком (X 5, Y 8, Wheel 11) плата выставляет сама, когда заблокированы оба направления.
    static constexpr uint16_t kLockAll = 0x3FFF;
    static constexpr uint16_t kLockSendable = 0x1F | 1u << 6 | 1u << 7 | 1u << 9 | 1u << 10 | 1u << 12 | 1u << 13;
    static constexpr uint16_t kLockMoveDirs = 1u << 6 | 1u << 7 | 1u << 9 | 1u << 10;  // +X -X +Y -Y: одна 0x16
    static constexpr uint16_t kLockWheelDirs = 1u << 12 | 1u << 13;                    // wheel+ wheel-: одна 0x17

    static unsigned long long u(uint64_t v) { return static_cast<unsigned long long>(v); }
    static constexpr DWORD kTimerHighResolution = 0x00000002;  // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Win10 1803+)

    struct Rec {
        int32_t dx = 0;
        int32_t dy = 0;
    };
    struct Flight {
        bool active = false;
        uint64_t seq = 0;  // nonce пакета
        uint32_t firstId = 0;
        uint8_t n = 0;
        std::array<Rec, kMaxRecords> recs{};
    };
    struct Meta {
        uint8_t schema = 0;
        uint32_t lastId = 0;
        uint8_t freeRecords = 0;
        uint8_t capacity = 0;
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

    // ---- открытие и переподключение ---------------------------------------------------
    // Рукопожатие с платой. initial: open() (запомнить исходный поток событий, сбросить
    // состояние); иначе переподключение (продолжить id, восстановить запрошенное).
    bool handshake(std::string& err, bool initial) {
        auto kinds = query(kOpDevice, nullptr, 0, 150, initial ? 3 : 1);
        if (!kinds || kinds->size() != 1) {
            err = "MAKCU " + endpoint_ + " не ответил на DEVICE (0x02) - адрес/порт, плата в сети?";
            return false;
        }
        if (!((*kinds)[0] & 1)) {
            err = "маршрут мыши не активен (DEVICE без mouse) - мышь в host-порту MAKCU?";
            return false;
        }
        if (auto fw = query(kOpFirmware, nullptr, 0, 150, 3); fw && fw->size() == 4) firmware_ = detail::le32(fw->data());
        if (firmware_ < kMinFirmware) {
            err = "прошивка " + std::to_string(firmware_) + " без batch 0x6C (нужна V4.097+)";
            return false;
        }
        auto meta = queryMeta();
        if (!meta) {
            err = "нет ответа на 0x6A(id=0) - Raw API недоступен";
            return false;
        }
        uint32_t cleaned = 0;
        if (meta->freeRecords < meta->capacity && meta->lastId > 0) {
            cleaned = consumeLeftovers(meta->lastId, meta->capacity - meta->freeRecords);
            if (auto again = queryMeta()) meta = again;
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (initial) {
                pend_.clear();
                nextId_ = meta->lastId + 1;
            } else {
                nextId_ = std::max(nextId_, meta->lastId + 1);  // id строго растут (после перезагрузки платы - тоже)
            }
            flight_ = Flight{};
            haveX_ = false;
            maxTerm_ = 0;
            boardQueued_ = 0;
            receiptT_ = 0;
        }
        // Поток событий мыши: включить (запомнить, каким был при open()), потом маска.
        // Порядок важен: поток начинается с "всё отпущено", маска читается после включения.
        const uint8_t kindMouse[1] = {0x01};
        if (auto was = query(kOpInputStream, kindMouse, 1, 100, 3); was && was->size() == 1) {
            if ((*was)[0] != 1) {
                const uint8_t on[2] = {0x01, 0x01};
                if (sendNoReply(kOpInputStream, on, 2) && initial) streamRestore_ = (*was)[0];
            }
            streamOk_ = true;
        }
        if (auto mask = query(kOpPhysButtons, nullptr, 0, 100, 3); mask && mask->size() == 1) {
            if (initial) {
                mask_.store((*mask)[0], std::memory_order_release);
                primed_.store(true, std::memory_order_release);
            } else {
                applyMask((*mask)[0], false);  // изменения за время обрыва - как обычные
            }
            buttonsOk_ = true;
        }
        // Инжект и блокировки: что сейчас на плате (после упавшей программы, перезагрузки).
        // Цикл сравнит с запрошенным и дошлёт разницу: при open() запрошено "ничего".
        std::optional<std::vector<uint8_t>> snap = query(kOpInjectSnapshot, nullptr, 0, 100, 3);
        uint8_t onBoardB = 0;
        uint16_t onBoardL = 0;
        if (snap && snap->size() >= 3) {
            onBoardB = static_cast<uint8_t>((*snap)[0] & 0x1F);
            onBoardL = normalizeLocks(static_cast<uint16_t>((*snap)[1] | (*snap)[2] << 8));
        }
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            sentButtons_ = onBoardB;
            sentLocks_ = onBoardL;
            boardButtons_ = onBoardB;
            boardLocks_ = onBoardL;
            ctlEpoch_ = boardEpoch_ = 0;
            verifyDue_ = false;
            const int64_t now = detail::Qpc::now();
            nextPollT_ = now;
            nextSnapT_ = now + detail::Qpc::fromUs(kHeartbeatUs);
            nextStreamCheckT_ = now + detail::Qpc::fromUs(kStreamCheckUs);
        }
        if (initial) {
            char buf[200];
            std::snprintf(buf, sizeof(buf), "прошивка %u, schema %u, last_accepted_id %lu, свободно записей %u из %u",
                firmware_, meta->schema, static_cast<unsigned long>(meta->lastId), meta->freeRecords, meta->capacity);
            info_ = buf;
            if (cleaned) info_ += ", дочищено записей прошлых сессий " + std::to_string(cleaned);
            char bb[200];
            if (buttonsOk_) {
                std::snprintf(bb, sizeof(bb), ", кнопки: маска %02X, события 0x53 %s, страховочный опрос %.0f мс", mask_.load(),
                    !streamOk_ ? "недоступны" : streamRestore_ ? "включены" : "уже были включены",
                    detail::Qpc::toUs(pollTicks_.load()) / 1000.0);
                info_ += bb;
            } else {
                info_ += ", кнопки: 0x55 не ответил";
            }
            if (onBoardB || onBoardL) {
                std::snprintf(bb, sizeof(bb), ", осталось от прошлой программы: инжект %02X, блокировки %04X - снимаю",
                    onBoardB, onBoardL);
                info_ += bb;
            }
        }
        if (!initial && trackOn_.load()) setTrackScreen();  // плата могла перезагрузиться (размер по умолчанию)
        return true;
    }

    // km.screen = 32767x32767: позиция в центре, отрезок физики начнётся заново.
    void setTrackScreen() {
        const uint8_t pl[4] = {kTrackScreen & 0xFF, kTrackScreen >> 8, kTrackScreen & 0xFF, kTrackScreen >> 8};
        (void)sendNoReply(kOpScreen, pl, 4);
        posEpoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    // Под trackMutex_: вернуть km.screen, каким он был до setTracking(true).
    void restoreScreenLocked() {
        if (screenRestore_ && connected_.load()) {
            const uint8_t pl[4] = {static_cast<uint8_t>(screenRestore_->first), static_cast<uint8_t>(screenRestore_->first >> 8),
                static_cast<uint8_t>(screenRestore_->second), static_cast<uint8_t>(screenRestore_->second >> 8)};
            (void)sendNoReply(kOpScreen, pl, 4);
        }
        screenRestore_.reset();
    }

    // Сторожевой поток: связь пропала - переподключиться и восстановить запрошенное.
    void watchdogLoop() {
        int64_t nextTry = 0;
        while (!stopRx_.load()) {
            Sleep(20);
            if (!ready_.load() || closing_.load()) continue;
            const int64_t now = detail::Qpc::now();
            if (connected_.load()) {
                if (now - lastRxT_.load(std::memory_order_acquire) > detail::Qpc::fromUs(kLinkLostUs)) {
                    std::lock_guard<std::mutex> lock(sendMutex_);
                    declareLostLocked(now);
                    nextTry = now;
                }
                continue;
            }
            if (now < nextTry) continue;
            nextTry = now + detail::Qpc::fromUs(kReconnectRetryUs);
            std::string err;
            if (!handshake(err, false)) continue;
            const int64_t back = detail::Qpc::now();
            lastRxT_.store(back, std::memory_order_release);
            lastOutageMs_.store(detail::Qpc::toUs(back - lostT_) / 1000.0);
            reconnects_.fetch_add(1, std::memory_order_relaxed);
            connected_ = true;
            kick();
        }
    }

    // Под sendMutex_: связь потеряна. Всё, что было в полёте, забыть; записи движения,
    // не дошедшие до платы, сбросить (к моменту восстановления они устареют).
    void declareLostLocked(int64_t now) {
        connected_ = false;
        lostT_ = now;
        linkLost_.fetch_add(1, std::memory_order_relaxed);
        batchSeq_ = metaSeq_ = physSeq_ = snapSeq_ = streamSeq_ = posSeq_ = 0;
        awaitSeq_.store(0, std::memory_order_release);
        std::lock_guard<std::mutex> lock(stateMutex_);
        uint64_t dropped = pend_.size();
        if (flight_.active) dropped += flight_.n;
        droppedRecords_.fetch_add(dropped, std::memory_order_relaxed);
        pend_.clear();
        flight_.active = false;
    }

    // close(): отпустить кнопки, снять блокировки и дождаться подтверждения снимком.
    void flushControls(int timeoutMs) {
        injectWanted_.store(0, std::memory_order_release);
        locksWanted_.store(0, std::memory_order_release);
        const int64_t until = detail::Qpc::now() + detail::Qpc::fromUs(timeoutMs * 1000.0);
        while (detail::Qpc::now() < until && connected_.load()) {
            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                (void)pumpLocked();
                if (sentButtons_ == 0 && sentLocks_ == 0 && boardEpoch_ == ctlEpoch_ && boardButtons_.load() == 0 &&
                    boardLocks_.load() == 0 && awaitSeq_.load() == 0) {
                    return;
                }
            }
            Sleep(1);
        }
    }

    // ---- служебный обмен (open/finish/переподключение, не горячий путь) -----------------
    std::optional<std::vector<uint8_t>> query(uint8_t op, const uint8_t* payload, uint16_t len, int timeoutMs,
        int tries) {
        for (int attempt = 0; attempt < tries; ++attempt) {
            uint64_t seq = 0;
            {
                // Одна команда в полёте: дождаться ответа на команду цикла и паузы после
                // команды без ответа. Пока держим sendMutex_, поток приёма новых не отправит.
                std::lock_guard<std::mutex> lock(sendMutex_);
                const int64_t idleBy = detail::Qpc::now() + detail::Qpc::fromUs(kLostUs);
                while ((awaitSeq_.load(std::memory_order_acquire) != 0 || detail::Qpc::now() < gapUntil_) &&
                       detail::Qpc::now() < idleBy) {
                    YieldProcessor();
                }
                seq = ++seq_;
                {
                    std::lock_guard<std::mutex> sl(syncMutex_);
                    syncSeq_ = seq;
                    syncDone_ = false;
                    syncWaiting_ = true;
                }
                awaitT_.store(detail::Qpc::now(), std::memory_order_relaxed);
                awaitSeq_.store(seq, std::memory_order_release);
                if (!sendFrameLocked(seq, op, payload, len)) {
                    clearAwait(seq);
                    std::lock_guard<std::mutex> sl(syncMutex_);
                    syncWaiting_ = false;
                    continue;
                }
            }
            std::unique_lock<std::mutex> sl(syncMutex_);
            const bool ok = syncCv_.wait_for(sl, std::chrono::milliseconds(timeoutMs), [&] { return syncDone_; });
            syncWaiting_ = false;
            clearAwait(seq);
            if (ok) return syncReply_;
        }
        return std::nullopt;
    }

    std::optional<Meta> queryMeta() {
        const uint8_t zero[5] = {0, 0, 0, 0, 0};
        auto r = query(kOpRawAck, zero, 5, 100, 3);
        if (!r || r->size() != 8) return std::nullopt;
        const uint8_t* p = r->data();
        return Meta{p[1], detail::le32(p + 2), p[6], p[7]};
    }

    // Освободить (consume=1) терминальные записи среди последних id: прерванные сессии
    // оставляют их на плате до перезагрузки. ВНИМАНИЕ: освобождает и записи другой
    // программы, если она тоже шлёт Raw.
    uint32_t consumeLeftovers(uint32_t lastId, uint32_t want) {
        uint32_t consumed = 0;
        for (uint32_t id = lastId, scanned = 0; id >= 1 && scanned < 1024 && consumed < want; --id, ++scanned) {
            uint8_t p[5];
            detail::put32(p, id);
            p[4] = 1;
            auto r = query(kOpRawAck, p, 5, 30, 2);
            if (r && r->size() == 6 && ((*r)[0] == 2 || (*r)[0] == 3)) ++consumed;
        }
        return consumed;
    }

    // ---- отправка -----------------------------------------------------------------------
    // Под sendMutex_. Датаграмма = 0x55 | nonce u64 | DE AD | LEN u16 | op | payload.
    // Ошибка сети (кабель, адаптер) не делает транспорт неисправным: обрыв заметит сторож.
    bool sendFrameLocked(uint64_t seq, uint8_t op, const uint8_t* payload, uint16_t len) {
        std::array<uint8_t, 14 + 8 + 10 * kMaxRecords> d{};
        size_t n = 0;
        d[n++] = 0x55;
        for (int i = 0; i < 8; ++i) d[n++] = static_cast<uint8_t>(seq >> (8 * i));
        d[n++] = 0xDE;
        d[n++] = 0xAD;
        d[n++] = static_cast<uint8_t>(len & 0xFF);
        d[n++] = static_cast<uint8_t>(len >> 8);
        d[n++] = op;
        if (len) std::memcpy(d.data() + n, payload, len);
        n += len;
        const int rc = ::sendto(sock_, reinterpret_cast<const char*>(d.data()), static_cast<int>(n), 0,
            reinterpret_cast<const sockaddr*>(&remote_), sizeof(remote_));
        if (rc != static_cast<int>(n)) {
            sendErrors_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        datagrams_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Команда без ответа вне цикла (SET 0x52 в open/close): дождаться, пока цикл свободен,
    // отправить и переждать передачу на второй чип.
    bool sendNoReply(uint8_t op, const uint8_t* payload, uint16_t len) {
        std::lock_guard<std::mutex> lock(sendMutex_);
        const int64_t idleBy = detail::Qpc::now() + detail::Qpc::fromUs(kLostUs);
        while ((awaitSeq_.load(std::memory_order_acquire) != 0 || detail::Qpc::now() < gapUntil_) &&
               detail::Qpc::now() < idleBy) {
            YieldProcessor();
        }
        const bool ok = sendFrameLocked(++seq_, op, payload, len);
        gapUntil_ = detail::Qpc::now() + detail::Qpc::fromUs(kNoReplyGapUs);
        while (detail::Qpc::now() < gapUntil_) YieldProcessor();
        return ok;
    }

    // Разбудить цикл из любого потока (кнопки, блокировки): отправить сразу, если свободен.
    void kick() {
        if (!ready_.load()) return;
        std::unique_lock<std::mutex> lock(sendMutex_, std::try_to_lock);
        if (lock.owns_lock()) (void)pumpLocked();
    }

    // Под sendMutex_. Одна команда в полёте, по приоритету:
    //   1) ответ не пришёл вовремя - разобрать (потерянная квитанция -> опрос 0x6A);
    //   2) пауза после команды без ответа;
    //   3) кнопки и блокировки (SET без ответа), затем сверка снимком 0x56;
    //   4) пульс-снимок, проверка потока событий, страховочный опрос кнопок 0x55;
    //   5) пакет движения 0x6C.
    bool pumpLocked() {
        if (!ready_.load() || failed_.load() || !connected_.load()) return false;
        const int64_t now = detail::Qpc::now();
        if (const uint64_t a = awaitSeq_.load(std::memory_order_acquire); a != 0) {
            const int64_t waited = now - awaitT_.load(std::memory_order_relaxed);
            if (a == posSeq_.load(std::memory_order_acquire)) {
                // Опрос позиции без ответа: позиция на плате копится сама, отсчёты не теряются.
                if (waited < detail::Qpc::fromUs(kPosLostUs)) return false;
                posLost_.fetch_add(1, std::memory_order_relaxed);
                posSeq_ = 0;
                clearAwait(a);
                return false;
            }
            if (a == physSeq_.load(std::memory_order_acquire) || a == snapSeq_.load(std::memory_order_acquire) ||
                a == streamSeq_.load(std::memory_order_acquire)) {
                // Опрос/снимок без ответа: забыть его, движение не ждёт (снимок повторится).
                if (waited < detail::Qpc::fromUs(kPollLostUs)) return false;
                if (a == physSeq_.load()) {
                    buttonLost_.fetch_add(1, std::memory_order_relaxed);
                    physSeq_ = 0;
                } else if (a == snapSeq_.load()) {
                    snapshotLost_.fetch_add(1, std::memory_order_relaxed);
                    snapSeq_ = 0;
                    verifyDue_ = true;
                } else {
                    streamSeq_ = 0;
                }
                clearAwait(a);
                return false;
            }
            if (a != batchSeq_.load() && a != metaSeq_.load()) return false;  // служебный запрос: ждёт сам
            if (waited < detail::Qpc::fromUs(kLostUs)) return false;
            const bool wasPacket = metaSeq_.load() == 0;
            const uint64_t seq = ++seq_;
            metaSeq_.store(seq, std::memory_order_release);
            awaitT_.store(now, std::memory_order_relaxed);
            awaitSeq_.store(seq, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                if (wasPacket) ++st_.lostReceipts;
                ++st_.metaQueries;
            }
            const uint8_t zero[5] = {0, 0, 0, 0, 0};
            (void)sendFrameLocked(seq, kOpRawAck, zero, 5);  // не ушёл - повтор через kLostUs
            return true;
        }
        if (now < gapUntil_) return false;  // после команды без ответа

        // Снимок пришёл: на плате может быть не то, что мы отправляли - разница уйдёт ниже.
        if (snapFresh_.exchange(false, std::memory_order_acq_rel)) {
            const uint8_t b = boardButtons_.load();
            const uint16_t l = boardLocks_.load();
            if (boardEpoch_ == ctlEpoch_ && (b != sentButtons_ || l != sentLocks_)) {
                repairs_.fetch_add(1, std::memory_order_relaxed);
                sentButtons_ = b;
                sentLocks_ = l;
            }
        }
        if (sendControlLocked(now)) return true;
        if (streamReenable_.exchange(false, std::memory_order_acq_rel)) {
            const uint8_t on[2] = {0x01, 0x01};
            if (sendFrameLocked(++seq_, kOpInputStream, on, 2)) {
                streamRestores_.fetch_add(1, std::memory_order_relaxed);
                gapUntil_ = now + detail::Qpc::fromUs(kNoReplyGapUs);
                return true;
            }
            streamReenable_ = true;
            return false;
        }
        // Сверка после команд и пульс связи.
        if (verifyDue_ || now >= nextSnapT_) {
            verifyDue_ = false;
            nextSnapT_ = now + detail::Qpc::fromUs(kHeartbeatUs);
            const uint64_t ss = ++seq_;
            snapEpoch_ = ctlEpoch_;
            snapSeq_.store(ss, std::memory_order_release);
            awaitT_.store(now, std::memory_order_relaxed);
            awaitSeq_.store(ss, std::memory_order_release);
            if (!sendFrameLocked(ss, kOpInjectSnapshot, nullptr, 0)) {
                snapSeq_ = 0;
                clearAwait(ss);
                verifyDue_ = true;
                return false;
            }
            snapshots_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // Поток событий всё ещё включён? (после перезагрузки платы - нет)
        if (streamOk_ && now >= nextStreamCheckT_) {
            nextStreamCheckT_ = now + detail::Qpc::fromUs(kStreamCheckUs);
            const uint64_t ss = ++seq_;
            const uint8_t kind[1] = {0x01};
            streamSeq_.store(ss, std::memory_order_release);
            awaitT_.store(now, std::memory_order_relaxed);
            awaitSeq_.store(ss, std::memory_order_release);
            if (!sendFrameLocked(ss, kOpInputStream, kind, 1)) {
                streamSeq_ = 0;
                clearAwait(ss);
                return false;
            }
            return true;
        }
        // Опрос кнопок по расписанию: одна команда, ответ ~1 мс, потом снова движение.
        int64_t poll = pollTicks_.load(std::memory_order_acquire);
        if (poll > 0 && eventsMissing()) poll = std::min(poll, detail::Qpc::fromUs(kFallbackPollMs * 1000.0));
        const bool pollNow = pollNow_.exchange(false, std::memory_order_acq_rel);  // после переполнения потока
        if (poll > 0 && buttonsOk_.load() && (pollNow || now >= nextPollT_)) {
            nextPollT_ = now - nextPollT_ > poll ? now + poll : nextPollT_ + poll;  // опоздали - от текущего момента
            const uint64_t ps = ++seq_;
            physSeq_.store(ps, std::memory_order_release);
            awaitT_.store(now, std::memory_order_relaxed);
            awaitSeq_.store(ps, std::memory_order_release);
            if (!sendFrameLocked(ps, kOpPhysButtons, nullptr, 0)) {
                physSeq_.store(0, std::memory_order_relaxed);
                clearAwait(ps);
                return false;
            }
            buttonPolls_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // Движения нет и плата отдала свою очередь: опрос позиции физической мыши.
        if (trackOn_.load(std::memory_order_acquire)) {
            bool idle = false;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                idle = pend_.empty() && !flight_.active && now >= injectQuietUntil_;
            }
            if (idle) return sendTrackLocked(now);
        }
        Flight f;
        bool haveX = false;
        uint32_t x = 0;
        uint64_t seq = 0;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (pend_.empty()) return false;
            seq = ++seq_;
            f.active = true;
            f.seq = seq;
            f.firstId = nextId_;
            f.n = static_cast<uint8_t>(std::min(pend_.size(), kMaxRecords));
            for (uint8_t i = 0; i < f.n; ++i) {
                f.recs[i] = pend_.front();
                pend_.pop_front();
            }
            haveX = haveX_;
            x = maxTerm_;
            flight_ = f;
        }
        std::array<uint8_t, 8 + 10 * kMaxRecords> pl{};
        pl[0] = 0x01;
        pl[1] = haveX ? 0x01 : 0x00;  // bit0: освободить терминальные записи с id <= X
        pl[2] = f.n;
        pl[3] = 0x00;
        detail::put32(pl.data() + 4, haveX ? x : 0);
        for (uint8_t i = 0; i < f.n; ++i) {
            uint8_t* r = pl.data() + 8 + 10 * i;
            detail::put32(r, f.firstId + i);
            detail::put16(r + 4, f.recs[i].dx);
            detail::put16(r + 6, f.recs[i].dy);
            detail::put16(r + 8, 0);  // dt
        }
        metaSeq_.store(0, std::memory_order_relaxed);
        batchSeq_.store(seq, std::memory_order_release);
        awaitT_.store(now, std::memory_order_relaxed);
        awaitSeq_.store(seq, std::memory_order_release);
        if (!sendFrameLocked(seq, kOpBatch, pl.data(), static_cast<uint16_t>(8 + 10 * f.n))) {
            batchSeq_.store(0, std::memory_order_relaxed);
            awaitSeq_.store(0, std::memory_order_release);
            std::lock_guard<std::mutex> lock(stateMutex_);
            requeueLocked(f, 0);
            flight_.active = false;
            return false;
        }
        std::lock_guard<std::mutex> lock(stateMutex_);
        ++st_.packets;
        return true;
    }

    // Под sendMutex_: одна команда кнопки или блокировки, если запрошенное отличается от
    // отправленного. Кнопки раньше блокировок; из блокировок первыми направления движения -
    // все четыре одной 0x16, затем колесо одной 0x17, затем кнопки по одной 0x60.
    // После команды - пауза и сверка снимком.
    bool sendControlLocked(int64_t now) {
        const uint8_t wantB = static_cast<uint8_t>(injectWanted_.load(std::memory_order_acquire) & 0x1F);
        if (const uint8_t diff = static_cast<uint8_t>(wantB ^ sentButtons_); diff) {
            uint8_t i = 0;
            while (!(diff & (1u << i))) ++i;
            const uint8_t state = static_cast<uint8_t>((wantB >> i) & 1u);
            if (!sendFrameLocked(++seq_, static_cast<uint8_t>(kOpButtonBase + i), &state, 1)) return false;
            sentButtons_ = static_cast<uint8_t>(sentButtons_ ^ (1u << i));
            buttonCommands_.fetch_add(1, std::memory_order_relaxed);
        } else {
            const uint16_t wantL = locksWanted_.load(std::memory_order_acquire);
            const uint16_t diffL = static_cast<uint16_t>((wantL ^ sentLocks_) & kLockSendable);
            if (!diffL) return false;
            auto bit = [wantL](int i) { return static_cast<uint8_t>((wantL >> i) & 1u); };
            if (diffL & kLockMoveDirs) {
                const uint8_t pl[4] = {bit(7), bit(6), bit(9), bit(10)};  // left=-X right=+X down=+Y up=-Y
                if (!sendFrameLocked(++seq_, kOpMoveMask, pl, 4)) return false;
                sentLocks_ = normalizeLocks(static_cast<uint16_t>((sentLocks_ & ~kLockMoveDirs) | (wantL & kLockMoveDirs)));
            } else if (diffL & kLockWheelDirs) {
                const uint8_t pl[2] = {bit(13), bit(12)};  // down=wheel- up=wheel+
                if (!sendFrameLocked(++seq_, kOpWheelMask, pl, 2)) return false;
                sentLocks_ = normalizeLocks(static_cast<uint16_t>((sentLocks_ & ~kLockWheelDirs) | (wantL & kLockWheelDirs)));
            } else {
                uint8_t i = 0;
                while (!(diffL & (1u << i))) ++i;
                const uint8_t pl[2] = {i, bit(i)};
                if (!sendFrameLocked(++seq_, kOpLock, pl, 2)) return false;
                sentLocks_ = normalizeLocks(static_cast<uint16_t>(sentLocks_ ^ (1u << i)));
            }
            lockCommands_.fetch_add(1, std::memory_order_relaxed);
        }
        ++ctlEpoch_;
        verifyDue_ = true;
        gapUntil_ = now + detail::Qpc::fromUs(kNoReplyGapUs);
        return true;
    }

    // Под sendMutex_: перецентровка или опрос позиции 0x63 (слежение включено, инжекта нет).
    bool sendTrackLocked(int64_t now) {
        // Оси заблокированы целиком, плата подтвердила: физика до ПК не доходит, позиция
        // стоит - слот оставить инжекту (через kLockSettleUs: утёкшие отсчёты уже учтены).
        const uint16_t want = locksWanted_.load(std::memory_order_acquire);
        const int64_t lockedT = fullLockT_.load(std::memory_order_acquire);
        if ((want & kLockMoveDirs) == kLockMoveDirs && lockedT && now - lockedT > detail::Qpc::fromUs(kLockSettleUs)) {
            return false;
        }
        if (recentreWanted_.exchange(false, std::memory_order_acq_rel)) {
            const uint8_t pl[4] = {kTrackScreen & 0xFF, kTrackScreen >> 8, kTrackScreen & 0xFF, kTrackScreen >> 8};
            if (!sendFrameLocked(++seq_, kOpScreen, pl, 4)) {
                recentreWanted_ = true;
                return false;
            }
            posEpoch_.fetch_add(1, std::memory_order_acq_rel);  // позиция в центре: новый отрезок
            recentres_.fetch_add(1, std::memory_order_relaxed);
            gapUntil_ = now + detail::Qpc::fromUs(kNoReplyGapUs);
            return true;
        }
        const uint64_t ps = ++seq_;
        posSentEpoch_.store(posEpoch_.load(std::memory_order_acquire), std::memory_order_release);
        posSeq_.store(ps, std::memory_order_release);
        awaitT_.store(now, std::memory_order_relaxed);
        awaitSeq_.store(ps, std::memory_order_release);
        if (!sendFrameLocked(ps, kOpGetPos, nullptr, 0)) {
            posSeq_.store(0, std::memory_order_relaxed);
            clearAwait(ps);
            return false;
        }
        posPolls_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Поток приёма: позиция 0x63. Разность с прошлым ответом того же отрезка - физика.
    // Опрос, отправленный до первого move(), ещё без инжекта (пакет ушёл после ответа):
    // его засчитать, а отрезок закрыть.
    void onPos(const uint8_t* p, size_t plen) {
        if (plen < 4) return;
        const int x = p[0] | p[1] << 8, y = p[2] | p[3] << 8;
        const uint64_t e = posSentEpoch_.load(std::memory_order_acquire);
        const int64_t now = detail::Qpc::now();
        std::lock_guard<std::mutex> pl(posMutex_);
        ++posReplies_;
        const bool same = posBaseValid_ && posBaseEpoch_ == e;
        if (same) {
            physX_ += x - posLastX_;
            physY_ += y - posLastY_;
        }
        posStill_ = same && x == posLastX_ && y == posLastY_ ? posStill_ + 1 : 0;
        posLastX_ = x;
        posLastY_ = y;
        posSampleT_ = now;
        ++posSamples_;
        posBaseEpoch_ = e;
        posBaseValid_ = e == posEpoch_.load(std::memory_order_acquire);
        if (x <= 0 || y <= 0 || x >= kTrackScreen - 1 || y >= kTrackScreen - 1) ++clamped_;
        const int off = std::max(std::abs(x - kTrackCentre), std::abs(y - kTrackCentre));
        if (off > kRecentreForce || (off > kRecentreIdle && posStill_ >= 2)) recentreWanted_ = true;
    }

    // Снять ожидание, если ждём именно этот nonce (иначе там уже другая команда).
    void clearAwait(uint64_t seq) {
        uint64_t expected = seq;
        awaitSeq_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }

    // Поток приёма: новая маска кнопок; переходы 0 -> 1 считаются нажатиями (после стартовой
    // маски). fromPoll: изменение нашёл опрос, а не событие - признак потерянных событий.
    void applyMask(uint8_t mask, bool fromPoll) {
        const uint8_t old = mask_.exchange(mask, std::memory_order_acq_rel);
        if (!primed_.load(std::memory_order_acquire)) return;
        if (old == mask) return;
        if (fromPoll) buttonMissed_.fetch_add(1, std::memory_order_relaxed);
        buttonChanges_.fetch_add(1, std::memory_order_relaxed);
        const uint8_t pressed = static_cast<uint8_t>(mask & ~old);
        for (uint8_t i = 0; i < kMouseButtonCount; ++i) {
            if (pressed & (1u << i)) presses_[i].fetch_add(1, std::memory_order_acq_rel);
        }
    }

    // Поток приёма: событие 0x53. Мышь: id = номер кнопки, value 0/1; FF FF - переполнение.
    void onInputChange(const uint8_t* p, size_t plen) {
        if (plen != 3 || p[0] != 1) return;  // не мышь (или колесо в тест-сборках, plen 4)
        if (p[1] == 0xFF && p[2] == 0xFF) {
            buttonOverflows_.fetch_add(1, std::memory_order_relaxed);
            pollNow_.store(true, std::memory_order_release);
            return;
        }
        if (p[1] >= 8 || p[2] > 1) return;
        buttonEvents_.fetch_add(1, std::memory_order_relaxed);
        const uint8_t bit = static_cast<uint8_t>(1u << p[1]);
        const uint8_t old = mask_.load(std::memory_order_acquire);
        applyMask(p[2] ? static_cast<uint8_t>(old | bit) : static_cast<uint8_t>(old & ~bit), false);
    }

    // Поток приёма: снимок 0x56 - инжектированные кнопки и блокировки на плате.
    void onSnapshot(const uint8_t* p, size_t plen) {
        if (plen < 3) return;
        boardButtons_.store(static_cast<uint8_t>(p[0] & 0x1F), std::memory_order_release);
        const uint16_t locks = normalizeLocks(static_cast<uint16_t>(p[1] | p[2] << 8));
        boardLocks_.store(locks, std::memory_order_release);
        if ((locks & kLockMoveDirs) == kLockMoveDirs) {
            int64_t zero = 0;
            fullLockT_.compare_exchange_strong(zero, detail::Qpc::now(), std::memory_order_acq_rel);
        } else {
            fullLockT_.store(0, std::memory_order_release);
        }
        boardEpoch_.store(snapEpoch_.load(std::memory_order_acquire), std::memory_order_release);
        snapFresh_.store(true, std::memory_order_release);
    }

    // События не доходят: ни одного, а опрос уже находил изменения.
    [[nodiscard]] bool eventsMissing() const {
        return buttonEvents_.load(std::memory_order_relaxed) == 0 &&
               buttonMissed_.load(std::memory_order_relaxed) >= kMissedForFallback;
    }

    void resetButtons() {
        mask_.store(0, std::memory_order_release);
        primed_.store(false, std::memory_order_release);
        pollNow_.store(false, std::memory_order_release);
        for (auto& c : presses_) c.store(0, std::memory_order_release);
        buttonEvents_ = buttonOverflows_ = buttonPolls_ = buttonReplies_ = buttonLost_ = buttonMissed_ = buttonChanges_ = 0;
    }

    void resetControls() {
        injectWanted_ = 0;
        locksWanted_ = 0;
        boardButtons_ = 0;
        boardLocks_ = 0;
        sentButtons_ = 0;
        sentLocks_ = 0;
        snapFresh_ = false;
        streamReenable_ = false;
        buttonCommands_ = lockCommands_ = snapshots_ = snapshotLost_ = repairs_ = streamRestores_ = 0;
        batchSeq_ = metaSeq_ = physSeq_ = snapSeq_ = streamSeq_ = posSeq_ = 0;
        awaitSeq_ = 0;
        gapUntil_ = 0;
    }

    void resetLinkStats() {
        linkLost_ = reconnects_ = droppedRecords_ = 0;
        lastOutageMs_ = 0.0;
        connected_ = false;
    }

    // Под stateMutex_: записи пакета с индекса from - обратно в начало очереди (порядок сохраняется).
    void requeueLocked(const Flight& f, uint8_t from) {
        for (int i = static_cast<int>(f.n) - 1; i >= static_cast<int>(from); --i) pend_.push_front(f.recs[i]);
    }

    // Под stateMutex_: пакет в полёте разобран.
    void settleLocked() {
        flight_.active = false;
        batchSeq_.store(0, std::memory_order_relaxed);
        metaSeq_.store(0, std::memory_order_relaxed);
        awaitSeq_.store(0, std::memory_order_release);
    }

    // ---- приём ----------------------------------------------------------------------------
    // Ждёт датаграмму или тик 1 мс (опросы, тайм-ауты). Тик - high-resolution waitable
    // timer: он точен без timeBeginPeriod(1), а select()/Sleep() без него просыпаются по
    // тику планировщика (~15.6 мс) и опросы шли бы реже.
    void rxLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, kTimerHighResolution, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);  // старая Windows: грубее
        LARGE_INTEGER due{};
        due.QuadPart = -10000;  // 1 мс, затем период 1 мс
        if (timer) SetWaitableTimer(timer, &due, 1, nullptr, nullptr, FALSE);
        WSAEVENT readable = WSACreateEvent();
        WSAEventSelect(sock_, readable, FD_READ);
        const HANDLE handles[2] = {readable, timer};
        std::array<uint8_t, 2048> buf{};
        while (!stopRx_.load()) {
            const DWORD w = WaitForMultipleObjects(timer ? 2 : 1, handles, FALSE, 20);
            if (w == WAIT_OBJECT_0) WSAResetEvent(readable);
            for (;;) {
                sockaddr_in from{};
                int fromLen = sizeof(from);
                const int n = ::recvfrom(sock_, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                    reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (n <= 0) break;  // WSAEWOULDBLOCK - всё прочитано
                // Любой порт платы (ответы с 8080, события - с порта, равного нашему), чужие IP - мимо.
                if (from.sin_addr.s_addr != remote_.sin_addr.s_addr) continue;
                onDatagram(buf.data(), static_cast<size_t>(n));
            }
            // Сразу по ответу следующая команда. Отправку держит другой поток - не ждём.
            if (ready_.load(std::memory_order_relaxed)) {
                std::unique_lock<std::mutex> lock(sendMutex_, std::try_to_lock);
                if (lock.owns_lock()) (void)pumpLocked();
            }
        }
        WSAEventSelect(sock_, readable, 0);
        WSACloseEvent(readable);
        if (timer) {
            CancelWaitableTimer(timer);
            CloseHandle(timer);
        }
    }

    void onDatagram(const uint8_t* d, size_t n) {
        if (n < 14 || d[0] != 0x55 || d[9] != 0xDE || d[10] != 0xAD) return;
        uint64_t seq = 0;
        for (int i = 0; i < 8; ++i) seq |= static_cast<uint64_t>(d[1 + i]) << (8 * i);
        const size_t plen = static_cast<size_t>(d[11]) | static_cast<size_t>(d[12]) << 8;
        if (14 + plen > n) return;
        lastRxT_.store(detail::Qpc::now(), std::memory_order_release);  // пульс: плата на связи
        const uint8_t* p = d + 14;  // d[13] - опкод ответа
        // Событие 0x53 несёт nonce одной из наших прошлых команд - разбирать по опкоду
        // ДО сопоставления по nonce, иначе его можно принять за квитанцию пакета.
        if (d[13] == kOpInputChange) {
            onInputChange(p, plen);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(syncMutex_);
            if (syncWaiting_ && !syncDone_ && seq == syncSeq_) {
                syncReply_.assign(p, p + plen);
                syncDone_ = true;
                clearAwait(seq);
                syncCv_.notify_all();
                return;
            }
        }
        if (seq != 0 && seq == posSeq_.load(std::memory_order_acquire)) {
            onPos(p, plen);
            posSeq_.store(0, std::memory_order_relaxed);
            clearAwait(seq);
            return;
        }
        if (seq != 0 && seq == physSeq_.load(std::memory_order_acquire)) {
            if (plen == 1) {
                applyMask(p[0], true);
                buttonReplies_.fetch_add(1, std::memory_order_relaxed);
            }
            physSeq_.store(0, std::memory_order_relaxed);
            clearAwait(seq);
            return;
        }
        if (seq != 0 && seq == snapSeq_.load(std::memory_order_acquire)) {
            onSnapshot(p, plen);
            snapSeq_.store(0, std::memory_order_relaxed);
            clearAwait(seq);
            return;
        }
        if (seq != 0 && seq == streamSeq_.load(std::memory_order_acquire)) {
            if (plen == 1 && p[0] != 1) streamReenable_.store(true, std::memory_order_release);
            streamSeq_.store(0, std::memory_order_relaxed);
            clearAwait(seq);
            return;
        }
        if (seq == batchSeq_.load(std::memory_order_acquire)) {
            onReceipt(seq, p, plen);
        } else if (seq == metaSeq_.load(std::memory_order_acquire)) {
            if (plen == 8) onMeta(detail::le32(p + 2));
        } else {
            std::lock_guard<std::mutex> lock(stateMutex_);
            ++st_.lateReplies;
        }
    }

    // Квитанция 0x6C: [0]=1 [1]=0 все / 1 частично / 255 отклонено [2]=N [3]=принято
    // [4]=первый отказ [5]=причина [6]=освобождено [7]=свободно [8]=K строк (<=7) [9]=more
    // [10]=в очереди [11]=0 [12..15]=last_id, затем K x (id u32 | state u8 | reason u8).
    void onReceipt(uint64_t seq, const uint8_t* p, size_t plen) {
        const double rttUs = detail::Qpc::toUs(detail::Qpc::now() - awaitT_.load(std::memory_order_relaxed));
        std::lock_guard<std::mutex> lock(stateMutex_);
        const Flight f = flight_;
        if (!f.active || f.seq != seq) {
            ++st_.lateReplies;
            return;
        }
        ++st_.receipts;
        rttSumUs_ += rttUs;
        ++rttCount_;
        rttMaxUs_ = std::max(rttMaxUs_, rttUs);
        if (plen < 16 || p[0] != 1) {
            // Не квитанция 0x6C: записи назад, транспорт дальше не работает.
            ++st_.badReplies;
            requeueLocked(f, 0);
            failed_ = true;
            settleLocked();
            return;
        }
        const uint8_t result = p[1];
        const uint8_t adm = std::min(p[3], f.n);
        const uint8_t reason = p[5];
        const uint8_t k = p[8];
        const uint32_t lastId = detail::le32(p + 12);
        if (result == 255) {
            ++st_.envelope;
            st_.lastReason = reason;
            if (reason == 1) {
                ++st_.xRejected;
                haveX_ = false;
            }
            requeueLocked(f, 0);
        } else {
            if (result == 1) {
                ++st_.partial;
                st_.lastReason = reason;
            }
            st_.admitted += adm;
            requeueLocked(f, adm);
            nextId_ = std::max(f.firstId + adm, adm ? lastId + 1 : f.firstId);
        }
        st_.freed += p[6];
        st_.minFree = std::min<uint32_t>(st_.minFree, p[7]);
        boardQueued_ = p[10];
        receiptT_ = detail::Qpc::now();
        injectQuietUntil_ = receiptT_ + detail::Qpc::fromUs(p[10] * 1000.0 + kInjectSettleUs);
        st_.boardQueued = p[10];
        st_.maxBoardQueued = std::max<uint32_t>(st_.maxBoardQueued, p[10]);
        if (p[9]) ++st_.moreSet;
        for (size_t i = 0; i < k && 16 + 6 * i + 6 <= plen; ++i) {
            const uint8_t* row = p + 16 + 6 * i;
            const uint32_t id = detail::le32(row);
            if (row[4] == 2) ++st_.termSubmitted;
            else if (row[4] == 3) ++st_.termRejected;
            maxTerm_ = std::max(maxTerm_, id);
            haveX_ = true;
        }
        settleLocked();
    }

    // Ответ на опрос после потерянной квитанции: last_accepted_id показывает, сколько
    // записей пакета плата приняла (приём идёт подряд с первой).
    void onMeta(uint32_t lastAccepted) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const Flight f = flight_;
        if (f.active) {
            uint8_t adm = 0;
            if (lastAccepted >= f.firstId) adm = static_cast<uint8_t>(std::min<uint32_t>(lastAccepted - f.firstId + 1, f.n));
            st_.admitted += adm;
            requeueLocked(f, adm);
            nextId_ = std::max(f.firstId + adm, lastAccepted + 1);
        }
        // Очередь платы неизвестна (квитанция потеряна): до опроса позиции - вся ёмкость, 16 мс.
        injectQuietUntil_ = detail::Qpc::now() + detail::Qpc::fromUs(16000.0 + kInjectSettleUs);
        settleLocked();
    }

    // ---- состояние ------------------------------------------------------------------------
    SOCKET sock_ = INVALID_SOCKET;
    bool wsaStarted_ = false;
    std::string endpoint_;
    std::string info_;
    uint32_t firmware_ = 0;
    std::atomic<bool> ready_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> closing_{false};

    std::mutex sendMutex_;  // отправка, seq_ и всё, что помечено "под sendMutex_"
    uint64_t seq_ = 0;
    std::atomic<uint64_t> awaitSeq_{0};   // nonce команды в полёте (0 - можно слать)
    std::atomic<uint64_t> batchSeq_{0};   // nonce пакета, ждущего квитанции
    std::atomic<uint64_t> metaSeq_{0};    // nonce опроса после потери
    std::atomic<uint64_t> physSeq_{0};    // nonce опроса кнопок 0x55
    std::atomic<uint64_t> snapSeq_{0};    // nonce снимка 0x56
    std::atomic<uint64_t> streamSeq_{0};  // nonce проверки потока 0x52
    std::atomic<uint64_t> posSeq_{0};     // nonce опроса позиции 0x63
    int64_t gapUntil_ = 0;                // под sendMutex_: до этого момента не слать (после команды без ответа)
    int64_t nextPollT_ = 0;               // под sendMutex_: когда следующий опрос кнопок
    int64_t nextSnapT_ = 0;               // под sendMutex_: когда следующий пульс-снимок
    int64_t nextStreamCheckT_ = 0;        // под sendMutex_: когда проверить поток событий
    std::atomic<int64_t> pollTicks_{detail::Qpc::fromUs(kDefaultButtonPollMs * 1000.0)};
    std::atomic<int64_t> awaitT_{0};
    std::atomic<uint64_t> datagrams_{0};
    std::atomic<uint64_t> sendErrors_{0};

    // Чтение кнопок: маску пишет только поток приёма (и рукопожатие), читают все.
    std::atomic<bool> buttonsOk_{false};
    std::atomic<uint8_t> mask_{0};
    std::array<std::atomic<uint32_t>, kMouseButtonCount> presses_{};
    std::atomic<uint64_t> buttonEvents_{0}, buttonOverflows_{0}, buttonPolls_{0}, buttonReplies_{0}, buttonLost_{0},
        buttonMissed_{0}, buttonChanges_{0};
    std::atomic<bool> primed_{false};   // стартовая маска получена (раньше - не нажатия)
    std::atomic<bool> pollNow_{false};  // переполнение потока: опросить сразу
    std::optional<uint8_t> streamRestore_;  // INPUT_STREAM mouse до open(), если меняли
    bool streamOk_ = false;                 // плата ответила на GET 0x52
    std::atomic<bool> streamReenable_{false};  // проверка нашла поток выключенным
    sockaddr_in remote_{};                  // адрес платы (сокет без connect())

    // Инжект и блокировки: запрошенное пишут вызывающие (любой поток), отправленное -
    // цикл под sendMutex_, состояние платы - поток приёма по снимку.
    std::atomic<uint8_t> injectWanted_{0};
    std::atomic<uint16_t> locksWanted_{0};
    std::atomic<uint8_t> boardButtons_{0};
    std::atomic<uint16_t> boardLocks_{0};
    std::atomic<bool> snapFresh_{false};
    uint8_t sentButtons_ = 0;    // под sendMutex_
    uint16_t sentLocks_ = 0;     // под sendMutex_ (нормальная форма)
    uint64_t ctlEpoch_ = 0;                // под sendMutex_: номер последней команды кнопки/блокировки
    std::atomic<uint64_t> snapEpoch_{0};   // ctlEpoch_ на момент отправки снимка (читает поток приёма)
    std::atomic<uint64_t> boardEpoch_{0};  // ctlEpoch_, к которому относится последний снимок
    bool verifyDue_ = false;     // под sendMutex_: после команд нужен снимок
    std::atomic<uint64_t> buttonCommands_{0}, lockCommands_{0}, snapshots_{0}, snapshotLost_{0}, repairs_{0},
        streamRestores_{0};

    // Физическая мышь (слежение по позиции платы)
    std::mutex trackMutex_;                          // setTracking/close: km.screen
    std::optional<std::pair<uint16_t, uint16_t>> screenRestore_;  // под trackMutex_: km.screen до слежения
    std::atomic<bool> trackOn_{false};
    std::atomic<uint64_t> posEpoch_{0};              // растёт на move() и перецентровке: отрезок закрыт
    std::atomic<uint64_t> posSentEpoch_{0};          // posEpoch_ на момент отправки опроса
    std::atomic<bool> recentreWanted_{false};
    std::atomic<int64_t> fullLockT_{0};              // снимок впервые показал обе оси заблокированными
    std::atomic<uint64_t> posPolls_{0}, posLost_{0}, recentres_{0};
    mutable std::mutex posMutex_;                    // всё ниже - под ним (пишет поток приёма)
    int64_t physX_ = 0, physY_ = 0;
    uint64_t posSamples_ = 0, posReplies_ = 0, clamped_ = 0;
    int64_t posSampleT_ = 0;
    int posLastX_ = 0, posLastY_ = 0, posStill_ = 0;
    bool posBaseValid_ = false;
    uint64_t posBaseEpoch_ = 0;

    // Связь
    std::atomic<bool> connected_{false};
    std::atomic<int64_t> lastRxT_{0};
    int64_t lostT_ = 0;
    std::atomic<uint64_t> linkLost_{0}, reconnects_{0}, droppedRecords_{0};
    std::atomic<double> lastOutageMs_{0.0};
    std::thread watchdog_;

    std::thread rx_;
    std::atomic<bool> stopRx_{false};

    std::mutex syncMutex_;
    std::condition_variable syncCv_;
    bool syncWaiting_ = false;
    bool syncDone_ = false;
    uint64_t syncSeq_ = 0;
    std::vector<uint8_t> syncReply_;

    mutable std::mutex stateMutex_;  // очередь, пакет в полёте, X, счётчики
    std::deque<Rec> pend_;
    uint32_t boardQueued_ = 0;   // под stateMutex_: в очереди платы по последней квитанции
    int64_t receiptT_ = 0;       //   и когда она пришла
    std::atomic<uint32_t> maxLag_{kDefaultMaxLag};
    int64_t injectQuietUntil_ = 0;  // под stateMutex_: плата отдаст очередь движения к этому моменту
    Flight flight_;
    uint32_t nextId_ = 1;
    uint32_t maxTerm_ = 0;  // старший терминальный id из квитанций
    bool haveX_ = false;
    Stats st_;
    double rttSumUs_ = 0.0;
    uint64_t rttCount_ = 0;
    double rttMaxUs_ = 0.0;
};

}  // namespace makcu
