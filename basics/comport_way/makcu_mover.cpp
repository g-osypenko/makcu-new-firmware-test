// MAKCU mover (COM): стабильная инжекция движения 500..2000 Гц с режимами
// для проверки поллинга. Подробности и методика - README.md рядом.
//
// Сборка: build.bat   (MinGW g++ + статическая libmakxd-cpp из mak-suite)
#include "makcu_transport.h"
#include "motion_engine.h"
#include "mover_common.h"
#include "hook_monitor.h"
#include "rawinput_monitor.h"

#include <makxd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace mover;

namespace {

enum class TxKind { Async, Sync, Km, Sdk };

const char* txName(TxKind k) {
    switch (k) {
    case TxKind::Async: return "async";
    case TxKind::Sync: return "sync";
    case TxKind::Km: return "km";
    case TxKind::Sdk: return "sdk";
    }
    return "?";
}

struct Options {
    std::string port;
    double hz = 1000.0;
    TxKind tx = TxKind::Async;
    Pattern pattern = Pattern::Circle;
    double speed = 4.0;
    double size = 150.0;
    double durationS = 10.0;
    int burst = 0;
    double burstAt = 3.0;
    double pauseEveryS = 0.0;
    double pauseMs = 0.0;
    bool hybridWait = false;
    int core = -1;
    int spread = -1;
    bool keepAccel = false;
    bool runOnce = false;
    bool check = false;
    bool sweep = false;
    bool sweepSpeed = false;
    double startDelayS = 0.0;
};

std::atomic<bool> g_exit{false};
LiveStats g_live;
PointerAccelGuard g_accel;
HookMonitor g_hook;
RawInputMonitor* g_rawmon = nullptr;

// Длина пути, которую Windows получила от основного устройства мыши.
int64_t receivedPathAbs() {
    if (!g_rawmon) return 0;
    const auto dev = g_rawmon->primary();
    return dev ? dev->sumAbs : 0;
}

bool pointerAccelOn() {
    int params[3]{};
    return SystemParametersInfoW(SPI_GETMOUSE, 0, params, 0) && params[2] != 0;
}

BOOL WINAPI consoleHandler(DWORD event) {
    g_exit = true;
    g_live.stopRequest = true;
    if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        g_accel.restore();
    }
    return TRUE;
}

void printUsage() {
    std::printf(
        "makcu_mover.exe [опции]\n"
        "  --port COM5          порт (по умолчанию - автопоиск через mak-suite SDK)\n"
        "  --hz N               частота команд, Гц (по умолчанию 1000)\n"
        "  --tx async|sync|km|sdk  транспорт (по умолчанию async; km = текстовый km.move)\n"
        "  --pause-every S      диагностика: каждые S секунд пауза --pause-ms\n"
        "  --pause-ms M         длительность паузы, мс\n"
        "  --pattern circle|eight|line\n"
        "  --speed V            скорость, пикс/мс (по умолчанию 4.0; для оверсэмплинга нужно >= 3)\n"
        "  --size S             радиус/амплитуда, пикс (150)\n"
        "  --duration S         длительность прогона, с (10)\n"
        "  --burst N            имитация руки: N команд подряд на секунде --burst-at\n"
        "  --burst-at S         когда делать всплеск, с (3)\n"
        "  --wait spin|hybrid   spin = мин. джиттер (по умолчанию), hybrid = меньше CPU\n"
        "  --core N             закрепить поток тайминга на ядре N\n"
        "  --spread N           временно задать mouse spread 0..100%% (вернётся при выходе)\n"
        "  --keep-accel         НЕ отключать 'Повышенную точность указателя'\n"
        "  --run                один прогон с этими параметрами и выход (без меню)\n"
        "  --check              проверка связи + тест тайминга MOVE(0,0) 2 с (курсор не двигается)\n");
}

bool parseOptions(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::printf("Не хватает значения для %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--port") o.port = next("--port");
        else if (a == "--hz") o.hz = std::atof(next("--hz"));
        else if (a == "--tx") {
            const std::string v = next("--tx");
            if (v == "async") o.tx = TxKind::Async;
            else if (v == "sync") o.tx = TxKind::Sync;
            else if (v == "km") o.tx = TxKind::Km;
            else if (v == "sdk") o.tx = TxKind::Sdk;
            else return false;
        } else if (a == "--pattern") {
            const std::string v = next("--pattern");
            if (v == "circle") o.pattern = Pattern::Circle;
            else if (v == "eight") o.pattern = Pattern::Eight;
            else if (v == "line") o.pattern = Pattern::Line;
            else if (v == "ramp") o.pattern = Pattern::Ramp;
            else if (v == "wobble") o.pattern = Pattern::Wobble;
            else return false;
        } else if (a == "--speed") o.speed = std::atof(next("--speed"));
        else if (a == "--size") o.size = std::atof(next("--size"));
        else if (a == "--duration") o.durationS = std::atof(next("--duration"));
        else if (a == "--burst") o.burst = std::atoi(next("--burst"));
        else if (a == "--burst-at") o.burstAt = std::atof(next("--burst-at"));
        else if (a == "--pause-every") o.pauseEveryS = std::atof(next("--pause-every"));
        else if (a == "--pause-ms") o.pauseMs = std::atof(next("--pause-ms"));
        else if (a == "--wait") o.hybridWait = std::string(next("--wait")) == "hybrid";
        else if (a == "--core") o.core = std::atoi(next("--core"));
        else if (a == "--spread") o.spread = std::atoi(next("--spread"));
        else if (a == "--keep-accel") o.keepAccel = true;
        else if (a == "--run") o.runOnce = true;
        else if (a == "--check") o.check = true;
        else if (a == "--sweep") o.sweep = true;
        else if (a == "--sweep-speed") o.sweepSpeed = true;
        else if (a == "--delay") o.startDelayS = std::atof(next("--delay"));
        else if (a == "--help" || a == "-h") { printUsage(); std::exit(0); }
        else {
            std::printf("Неизвестная опция: %s\n", a.c_str());
            return false;
        }
    }
    if (o.hz < 10.0 || o.hz > 8000.0 || o.speed <= 0.0 || o.size <= 1.0 || o.durationS <= 0.0 ||
        o.spread > 100) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Связь с MAKCU: SDK для обнаружения/handshake/настроек и режима "sdk",
// собственный overlapped-порт для режимов async/sync. Порт эксклюзивный,
// поэтому одновременно открыт только один из них.
// ---------------------------------------------------------------------------
class MakcuLink {
public:
    bool init(const Options& opt) {
        std::printf("[*] Поиск MAKCU через mak-suite SDK...\n");
        const auto devices = makxd::Device::findDevices();
        for (const auto& d : devices) {
            std::printf("    найден %s  (%s, VID %04X PID %04X)\n", d.port.c_str(), d.description.c_str(), d.vid, d.pid);
        }
        port_ = !opt.port.empty() ? opt.port : (devices.empty() ? std::string{} : devices.front().port);
        if (port_.empty()) {
            std::printf("[-] MAKCU (CH343/CH340) не найден. Укажите --port COMx.\n");
            return false;
        }

        // SDK на занятом порту пишет только "probe failed at every supported
        // baud" - проверяем занятость сами, чтобы сообщение было понятным.
        {
            const std::string path = "\\\\.\\" + port_;
            HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                const DWORD err = GetLastError();
                if (err == ERROR_ACCESS_DENIED) {
                    std::printf("[-] %s занят другой программой: второй makcu_mover (выход - END), вкладка браузера\n"
                                "    с app.makcu.com / web-flasher, MAKCU app или монитор порта. Закройте её и повторите.\n",
                        port_.c_str());
                } else {
                    std::printf("[-] Не удалось открыть %s (ошибка %lu)\n", port_.c_str(), err);
                }
                return false;
            }
            CloseHandle(h);
        }

        if (!sdk_.connect(port_)) {
            std::printf("[-] SDK connect(%s) не удался: %s\n", port_.c_str(), sdk_.getLastError().c_str());
            std::printf("    Порт свободен, но MAKCU не ответил на MAK_API DEVICE (4M/1M/115200).\n"
                        "    После прошивки проверьте скорость (кнопка у USB1 переключает 115200/4M)\n"
                        "    и что прошивка поддерживает MAK_API (протокол mak-suite).\n");
            return false;
        }
        const auto kinds = sdk_.device();
        const auto fw = sdk_.firmwareVersion();
        std::printf("[+] SDK: подключено к %s | прошивка %u | kinds 0x%02X%s\n", port_.c_str(),
            fw ? *fw : 0u, kinds ? kinds->kinds : 0u, (kinds && kinds->hasMouse()) ? " (mouse OK)" : "");
        if (!kinds || !kinds->hasMouse()) {
            std::printf("[-] Маршрут мыши не активен (DEVICE не содержит mouse).\n");
            sdk_.disconnect();
            return false;
        }

        try {
            auto live = sdk_.readDeviceSettings();
            spreadOriginal_ = live.settings.mouse_spread_percent;
            currentSpread_ = *spreadOriginal_;
            std::printf("[+] mouse spread = %u%%\n", static_cast<unsigned>(*spreadOriginal_));
            if (opt.spread >= 0 && opt.spread != *spreadOriginal_) {
                live.settings.mouse_spread_percent = static_cast<uint8_t>(opt.spread);
                (void)sdk_.applyDeviceSettings(live, MAKXD_SETTINGS_MOUSE);
                spreadChanged_ = true;
                currentSpread_ = opt.spread;
                std::printf("[+] mouse spread временно = %d%% (без сохранения в NOR)\n", opt.spread);
            }
        } catch (const std::exception& e) {
            std::printf("[!] Настройки не прочитаны: %s\n", e.what());
        }
        active_ = Active::Sdk;
        return true;
    }

    Transport* acquire(TxKind kind) {
        if (kind == TxKind::Sdk) {
            if (active_ == Active::Raw) {
                raw_.close();
                active_ = Active::None;
            }
            if (!sdk_.isConnected() && !sdk_.connect(port_)) {
                std::printf("[-] SDK connect: %s\n", sdk_.getLastError().c_str());
                return nullptr;
            }
            active_ = Active::Sdk;
            if (!sdkTx_) {
                sdkTx_ = std::make_unique<SdkTransport>(sdk_);
            }
            return sdkTx_.get();
        }

        if (active_ == Active::Sdk) {
            sdkTx_.reset();
            sdk_.disconnect();
            Sleep(150);
            active_ = Active::None;
        }
        if (!raw_.isOpen() || !raw_.healthy()) {
            std::string error;
            if (!raw_.open(port_, error)) {
                std::printf("[-] Raw COM: %s\n", error.c_str());
                return nullptr;
            }
            std::printf("[+] Raw COM %s открыт: overlapped, %u бод, DEVICE kinds 0x%02X\n", port_.c_str(),
                raw_.baud(), raw_.deviceKinds().value_or(0));
            if (auto km = raw_.kmQuery("km.device()", std::chrono::milliseconds(300))) {
                reportUsbPeriod(*km);
            }
        }
        active_ = Active::Raw;
        raw_.setMode(kind == TxKind::Sync ? RawComTransport::Mode::Sync : RawComTransport::Mode::Async);
        raw_.setKmText(kind == TxKind::Km);
        return &raw_;
    }

    // Live-смена mouse spread через SDK (без NOR). Исходное значение
    // вернётся в shutdown().
    bool setSpread(int percent) {
        if (active_ == Active::Raw) {
            raw_.close();
            active_ = Active::None;
            Sleep(100);
        }
        sdkTx_.reset();
        if (!sdk_.isConnected() && !sdk_.connect(port_)) {
            std::printf("[-] SDK connect: %s\n", sdk_.getLastError().c_str());
            return false;
        }
        active_ = Active::Sdk;
        try {
            auto live = sdk_.readDeviceSettings();
            if (!spreadOriginal_) spreadOriginal_ = live.settings.mouse_spread_percent;
            live.settings.mouse_spread_percent = static_cast<uint8_t>(percent);
            live = sdk_.applyDeviceSettings(live, MAKXD_SETTINGS_MOUSE);
            spreadChanged_ = live.settings.mouse_spread_percent != *spreadOriginal_;
            currentSpread_ = live.settings.mouse_spread_percent;
            std::printf("[+] mouse spread = %d%% (live, без записи в NOR)\n", currentSpread_);
            return true;
        } catch (const std::exception& e) {
            std::printf("[!] spread не применён: %s\n", e.what());
            return false;
        }
    }

    [[nodiscard]] int currentSpread() const { return currentSpread_; }

    void shutdown() {
        raw_.close();
        sdkTx_.reset();
        if (spreadChanged_ && spreadOriginal_) {
            if (sdk_.isConnected() || sdk_.connect(port_)) {
                try {
                    auto live = sdk_.readDeviceSettings();
                    live.settings.mouse_spread_percent = *spreadOriginal_;
                    (void)sdk_.applyDeviceSettings(live, MAKXD_SETTINGS_MOUSE);
                    std::printf("[+] mouse spread возвращён в %u%%\n", static_cast<unsigned>(*spreadOriginal_));
                } catch (const std::exception& e) {
                    std::printf("[!] Не удалось вернуть spread: %s\n", e.what());
                }
            }
        }
        if (sdk_.isConnected()) {
            sdk_.disconnect();
        }
        active_ = Active::None;
    }

    [[nodiscard]] int usbMouseHz() const { return usbMouseHz_; }

private:
    void reportUsbPeriod(const std::string& km) {
        // "R:MK;M:8uf;K:8uf;C:0uf" - период HID-отчётов в микрокадрах (125 мкс).
        std::printf("[+] km.device(): %s\n", km.c_str());
        const auto m = km.find("M:");
        if (m == std::string::npos) return;
        const int uf = std::atoi(km.c_str() + m + 2);
        if (uf <= 0) return;
        usbMouseHz_ = 8000 / uf;
        std::printf("[+] USB-интервал мыши MAKCU: %d мкфр = %.3f мс -> потолок %d Гц\n", uf, uf * 0.125, usbMouseHz_);
        if (usbMouseHz_ < 1000) {
            std::printf("[!] Потолок ниже 1000 Гц: >700 Гц недостижимо, пока интервал дескриптора не 1 мс.\n");
        }
    }

    enum class Active { None, Sdk, Raw };
    std::string port_;
    makxd::Device sdk_;
    RawComTransport raw_;
    std::unique_ptr<SdkTransport> sdkTx_;
    Active active_ = Active::None;
    std::optional<uint8_t> spreadOriginal_;
    bool spreadChanged_ = false;
    int usbMouseHz_ = 0;
    int currentSpread_ = -1;
};

// ---------------------------------------------------------------------------
// Пресеты для тестов: глобальные F-клавиши работают, пока в фокусе тестер.
// ---------------------------------------------------------------------------
struct Preset {
    int vk;
    const char* key;
    const char* title;
    double hz;
    TxKind tx;
    double pauseEveryS;
    double pauseMs;
    std::optional<Pattern> pattern;
    std::optional<double> durationS;
    int sweep = 0;    // 1 - перебор без касания, 2 - перебор после касания
    int spread = -1;  // mouse spread перед стартом (live, без NOR); -1 - не трогать
};

// Прошивка 4094 (замеры 2026-10-10): частоту выдачи инжекта задаёт прошивка
// (~615-650 Гц, после касания реальной мыши 250-460 Гц), от частоты команд не
// зависит начиная с ~250/с. Путь точен при <= 1000 команд/с; 2000+ теряют
// движение. Поэтому рабочие режимы - 125/250/1000, без оверсэмплинга.
const Preset kPresets[] = {
    {VK_F1, "F1", "1000 Гц | spread 0%  | 30 с (мин. задержка)", 1000, TxKind::Async, 0, 0, {}, 30.0, 0, 0},
    {VK_F2, "F2", " 250 Гц | spread 0%  | 30 с (рекомендуемый)", 250, TxKind::Async, 0, 0, {}, 30.0, 0, 0},
    {VK_F3, "F3", " 125 Гц | spread 50% | 30 с (сглаживание ~17 мс)", 125, TxKind::Async, 0, 0, {}, 30.0, 0, 50},
    {VK_F4, "F4", " 125 Гц | spread 0%  | 30 с", 125, TxKind::Async, 0, 0, {}, 30.0, 0, 0},
    {VK_F5, "F5", "ПЕРЕБОР 125..2000 Гц: HID + путь, мышь не трогать (~25 с)", 0, TxKind::Async, 0, 0, {}, {}, 1},
    {VK_F6, "F6", "ПЕРЕБОР ПОСЛЕ КАСАНИЯ: 6 с с касанием, затем 125..2000 Гц (~25 с)", 0, TxKind::Async, 0, 0, {},
        {}, 2},
    {VK_F7, "F7", " 250 Гц | spread 0%  | линия 60 с (тест с рукой)", 250, TxKind::Async, 0, 0, Pattern::Line,
        60.0, 0, 0},
    {VK_F8, "F8", " 125 Гц | spread 50% | линия 60 с (тест с рукой)", 125, TxKind::Async, 0, 0, Pattern::Line,
        60.0, 0, 50},
};

constexpr int kSpreadSteps[] = {0, 5, 10, 25, 50, 100};

class KeyEdge {
public:
    bool pressed(int vk) {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool edge = down && !state_[vk & 0xFF];
        state_[vk & 0xFF] = down;
        return edge;
    }
    void sync() {
        for (int vk = 0; vk < 256; ++vk) {
            state_[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        }
    }

private:
    bool state_[256]{};
};

// ---------------------------------------------------------------------------
// Прогон + живой вывод + итог.
// HID-частота считается LL-хуком (каждое событие мыши, без фонового
// прореживания Raw Input). Сумма пути - через Raw Input.
// ---------------------------------------------------------------------------
struct HidWindowStats {
    double avgHz = 0.0;
    double medianHz = 0.0;
    double worstHz = 0.0;
    double pathRatio = 0.0;  // пришло/отправлено по длине пути, без первой секунды
};

void printSummary(const RunConfig& cfg, const Transport& tx, RawInputMonitor& monitor, const HidWindowStats& hid) {
    const LiveStats& l = g_live;
    const double elapsed = std::max(1e-6, l.elapsedS.load());
    const uint64_t ticks = l.ticks.load();
    const uint64_t writes = tx.stats.writes.load();

    std::printf("\n================ ИТОГ: %s ================\n", cfg.label.c_str());
    std::printf("Траектория: %s, %.2f пикс/мс, размер %.0f | %.0f Гц | %.2f с\n",
        patternName(cfg.trajectory.pattern), cfg.trajectory.speed, cfg.trajectory.size, cfg.hz, elapsed);
    std::printf("Транспорт %-13s записей %llu (%.0f/с) | вызов ср %.1f / max %.1f мкс | в полёте max %u | отложено %llu | ошибок %llu\n",
        tx.name(), static_cast<unsigned long long>(writes), writes / elapsed,
        writes ? tx.stats.sumCallUs.load() / writes : 0.0, tx.stats.maxCallUs.load(),
        tx.stats.maxInflight.load(), static_cast<unsigned long long>(l.deferred.load()),
        static_cast<unsigned long long>(tx.stats.errors.load()));
    std::printf("Тайминг: тиков %llu | пропущено %llu | фризов %llu | опоздание max %.0f мкс\n",
        static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(l.skippedTicks.load()),
        static_cast<unsigned long long>(l.stalls.load()), l.runLateMaxUs.load());

    std::printf("Повышенная точность указателя сейчас: %s\n",
        pointerAccelOn() ? "ВКЛЮЧЕНА (курсор будет ускоряться при крупных дельтах!)" : "выключена");
    std::printf("HID (LL-хук): в среднем %.0f Гц | окна 1 с: медиана %.0f, худшее %.0f Гц\n", hid.avgHz,
        hid.medianHz, hid.worstHz);
    const auto h = g_hook.histogram();
    uint64_t total = 0;
    for (auto v : h) total += v;
    std::printf("  интервалы:");
    for (size_t i = 0; i < h.size(); ++i) {
        if (h[i]) std::printf("  %s: %.1f%%", HookMonitor::kBucketNames[i], 100.0 * h[i] / std::max<uint64_t>(1, total));
    }
    std::printf("\n");

    if (const auto dev = monitor.primary()) {
        const int64_t sx = l.sentX.load();
        const int64_t sy = l.sentY.load();
        std::printf("Путь (%s): отправлено (%lld, %lld), пришло в Windows (%lld, %lld) -> %s\n", dev->name.c_str(),
            static_cast<long long>(sx), static_cast<long long>(sy), static_cast<long long>(dev->sumX),
            static_cast<long long>(dev->sumY),
            (sx == dev->sumX && sy == dev->sumY)
                ? "СОВПАДАЕТ (если при этом двигали мышь рукой - прошивка СЪЕЛА движение руки!)"
                : "расхождение (если трогали мышь - это её путь, так и должно быть)");
    }
}

// Запускает движение и раз в секунду печатает живую строку.
// Возвращает статистику HID по 1-секундным окнам (первое окно пропускается).
HidWindowStats runAndWatch(const RunConfig& cfg, Transport& tx, int stopVk, bool printLive) {
    g_live.reset();
    tx.stats.reset();
    g_hook.reset();

    std::thread worker([&] { runMotion(cfg, tx, g_live); });

    KeyEdge keys;
    keys.sync();
    const int64_t waitStart = Clock::now();
    while (!g_live.running.load() && Clock::toUs(Clock::now() - waitStart) < 500000) {
        Sleep(1);
    }
    int64_t lastPrint = Clock::now();
    uint64_t lastMoves = 0;
    uint64_t lastHid = g_hook.count();
    int64_t lastSentAbs = 0;
    int64_t lastRecvAbs = receivedPathAbs();
    std::vector<double> windows;
    uint64_t hidAfterFirst = 0;
    double tAfterFirst = 0.0;
    int64_t sentAfterFirst = 0;
    int64_t recvAfterFirst = 0;
    while (g_live.running.load()) {
        Sleep(10);
        if (keys.pressed(VK_ESCAPE) || (stopVk && keys.pressed(stopVk)) || g_exit.load()) {
            g_live.stopRequest = true;
        }
        const int64_t now = Clock::now();
        const double dtUs = Clock::toUs(now - lastPrint);
        if (dtUs >= 1000000.0 && g_live.running.load()) {
            const uint64_t moves = g_live.moves.load();
            const uint64_t hid = g_hook.count();
            const double hidHz = (hid - lastHid) * 1e6 / dtUs;
            if (windows.empty()) {
                hidAfterFirst = hid;
                tAfterFirst = Clock::toUs(now) / 1e6;
                sentAfterFirst = g_live.sentAbs.load();
                recvAfterFirst = receivedPathAbs();
            }
            windows.push_back(hidHz);
            const int64_t sentAbs = g_live.sentAbs.load();
            const int64_t recvAbs = receivedPathAbs();
            if (printLive) {
                // путь x1.00 = Windows получила ровно столько, сколько отправлено;
                // > 1 - лишнее движение (рука или дублирование в прошивке).
                const double sentD = static_cast<double>(sentAbs - lastSentAbs);
                const double ratio = sentD > 0 ? static_cast<double>(recvAbs - lastRecvAbs) / sentD : 0.0;
                std::printf("  [%5.1f с] HID %5.0f Гц | команд %5.0f/с | путь x%.2f | отложено %llu | опозд.max %5.0f мкс\n",
                    g_live.elapsedS.load(), hidHz, (moves - lastMoves) * 1e6 / dtUs, ratio,
                    static_cast<unsigned long long>(g_live.deferred.load()), g_live.windowLateMaxUs.exchange(0.0));
                std::fflush(stdout);
            }
            lastSentAbs = sentAbs;
            lastRecvAbs = recvAbs;
            lastMoves = moves;
            lastHid = hid;
            lastPrint = now;
        }
    }
    const double tEnd = Clock::toUs(Clock::now()) / 1e6;
    const uint64_t hidEnd = g_hook.count();
    worker.join();

    HidWindowStats s;
    Sleep(15);  // хвост Raw Input
    {
        const double sentD = static_cast<double>(g_live.sentAbs.load() - sentAfterFirst);
        s.pathRatio = sentD > 0 ? static_cast<double>(receivedPathAbs() - recvAfterFirst) / sentD : 0.0;
    }
    if (windows.size() >= 2) {
        s.avgHz = (hidEnd - hidAfterFirst) / std::max(1e-3, tEnd - tAfterFirst);
        std::vector<double> w(windows.begin() + 1, windows.end());
        std::sort(w.begin(), w.end());
        s.worstHz = w.front();
        s.medianHz = w[w.size() / 2];
    } else if (!windows.empty()) {
        s.avgHz = s.medianHz = s.worstHz = windows.front();
    }
    return s;
}

void runOnce(const RunConfig& cfg, Transport& tx, RawInputMonitor& monitor, int stopVk) {
    monitor.reset();
    std::printf("\n>>> СТАРТ: %s | %s | %s | %.1f с  (стоп: %s / ESC)\n", cfg.label.c_str(), tx.name(),
        patternName(cfg.trajectory.pattern), cfg.durationS, stopVk ? "та же F-клавиша" : "ESC");
    const HidWindowStats hid = runAndWatch(cfg, tx, stopVk, true);
    Sleep(50);  // дать Raw Input дособрать последние сообщения
    printSummary(cfg, tx, monitor, hid);
}

RunConfig makeConfig(const Options& o, double hz, int burst, Pattern pattern, double durationS, std::string label,
    double pauseEveryS = 0.0, double pauseMs = 0.0) {
    RunConfig cfg;
    cfg.label = std::move(label);
    cfg.hz = hz;
    cfg.trajectory.pattern = pattern;
    cfg.trajectory.speed = o.speed;
    cfg.trajectory.size = o.size;
    cfg.durationS = durationS;
    cfg.burstFrames = burst;
    cfg.burstAtS = o.burstAt;
    cfg.pauseEveryS = pauseEveryS;
    cfg.pauseMs = pauseMs;
    cfg.hybridWait = o.hybridWait;
    cfg.core = o.core;
    return cfg;
}

// Калибровка замера: без инжекта, человек водит реальной мышью. Если хук
// покажет ~1000 Гц, как и тестер, хуку можно верить и в остальных режимах.
void measureHand(int stopVk) {
    std::printf("\n>>> ЗАМЕР РУКИ: 6 с, инжекта нет - быстро и непрерывно водите реальной мышью\n");
    g_hook.reset();
    KeyEdge keys;
    keys.sync();
    uint64_t last = 0;
    int64_t t = Clock::now();
    std::vector<double> rates;
    for (int sec = 0; sec < 6 && !g_exit.load(); ++sec) {
        const int64_t until = t + Clock::fromUs(1e6);
        bool stop = false;
        while (Clock::now() < until) {
            Sleep(10);
            if (keys.pressed(VK_ESCAPE) || keys.pressed(stopVk)) stop = true;
        }
        const int64_t now = Clock::now();
        const uint64_t c = g_hook.count();
        const double hz = (c - last) * 1e6 / Clock::toUs(now - t);
        rates.push_back(hz);
        std::printf("  [%d с] HID %5.0f Гц\n", sec + 1, hz);
        last = c;
        t = now;
        if (stop) break;
    }
    const auto h = g_hook.histogram();
    uint64_t total = 0;
    for (auto v : h) total += v;
    std::printf("  интервалы:");
    for (size_t i = 0; i < h.size(); ++i) {
        if (h[i]) std::printf("  %s: %.1f%%", HookMonitor::kBucketNames[i], 100.0 * h[i] / std::max<uint64_t>(1, total));
    }
    std::printf("\n  Сравните с тестером: при ~1000 Гц в обоих замер хука верный.\n");
}

// Перебор частот команд: таблица "команд/с -> HID Гц и путь". Траектория -
// линия (монотонные отрезки), чтобы длина пути через Raw Input была точной.
//  afterTouch = false: перед каждой частотой пауза 0.5 с (свежее состояние
//                      прошивки, мышь не трогать).
//  afterTouch = true : сначала 6 с при 1000 Гц, во время которых нужно
//                      коснуться мыши и убрать руку; дальше частоты идут
//                      подряд без пауз (состояние "после руки" сохраняется).
void runSweep(const Options& o, Transport& tx, int stopVk, bool afterTouch) {
    static constexpr double kRates[] = {125, 250, 500, 1000, 2000};
    std::printf("\n>>> ПЕРЕБОР %s: линия %.1f пикс/мс, по 4 с на частоту (стоп: та же F-клавиша / ESC)\n",
        afterTouch ? "ПОСЛЕ КАСАНИЯ" : "БЕЗ КАСАНИЯ", o.speed);
    g_live.stopRequest = false;
    if (afterTouch) {
        std::printf("  Сейчас 6 с при 1000 Гц: на 2-3-й секунде коснитесь/подвигайте мышью 1 с и УБЕРИТЕ руку.\n");
        const RunConfig warm = makeConfig(o, 1000, 0, Pattern::Line, 6.0, "touch");
        (void)runAndWatch(warm, tx, stopVk, true);
        std::printf("  Дальше мышь НЕ трогать.\n");
    } else {
        std::printf("  Мышь НЕ трогать.\n");
    }
    std::printf("  команд/с | HID ср, Гц | HID худш., Гц | путь пришло/отпр. | 1 кадр %% | 2 кадра %% | >2.5 мс %%\n");
    for (double hz : kRates) {
        if (g_live.stopRequest.load() || g_exit.load()) break;
        if (!afterTouch) Sleep(500);
        const Pattern pat = o.pattern == Pattern::Wobble ? Pattern::Wobble : Pattern::Ramp;
        const RunConfig cfg = makeConfig(o, hz, 0, pat, 4.0, "sweep");
        const HidWindowStats hid = runAndWatch(cfg, tx, stopVk, false);
        if (g_live.stopRequest.load()) break;
        const auto h = g_hook.histogram();
        uint64_t total = 0;
        for (auto v : h) total += v;
        const double tot = static_cast<double>(std::max<uint64_t>(1, total));
        std::printf("  %8.0f | %10.0f | %13.0f | %17.2f | %8.1f | %9.1f | %9.1f\n", hz, hid.avgHz, hid.worstHz,
            hid.pathRatio, 100.0 * h[1] / tot, 100.0 * h[3] / tot, 100.0 * (h[4] + h[5]) / tot);
        std::fflush(stdout);
    }
    std::printf("  путь 1.00 = прошивка доставила всё движение; < 1 - потеряла часть (скорость курсора 'плывёт').\n");
}

// Перебор скорости при фиксированной частоте команд (--hz): проверяет,
// зависит ли частота HID от величины дельт (лимит дельты на один отчёт).
void runSpeedSweep(const Options& o, Transport& tx, int stopVk) {
    static constexpr double kSpeeds[] = {1, 2, 4, 6, 8, 12, 16};
    std::printf("\n>>> ПЕРЕБОР СКОРОСТИ: линия, %.0f команд/с, по 4 с (мышь НЕ трогать)\n", o.hz);
    std::printf("  пикс/мс | HID ср, Гц | пикс на отчёт | путь пришло/отпр. | 1 кадр %% | 2 кадра %%\n");
    g_live.stopRequest = false;
    for (double v : kSpeeds) {
        if (g_live.stopRequest.load() || g_exit.load()) break;
        Sleep(500);
        Options ov = o;
        ov.speed = v;
        const RunConfig cfg = makeConfig(ov, o.hz, 0, Pattern::Ramp, 4.0, "speed");
        const HidWindowStats hid = runAndWatch(cfg, tx, stopVk, false);
        const auto h = g_hook.histogram();
        uint64_t total = 0;
        for (auto x : h) total += x;
        const double tot = static_cast<double>(std::max<uint64_t>(1, total));
        std::printf("  %7.0f | %10.0f | %13.2f | %17.2f | %8.1f | %9.1f\n", v, hid.avgHz,
            hid.avgHz > 0 ? v * 1000.0 * hid.pathRatio / hid.avgHz : 0.0, hid.pathRatio, 100.0 * h[1] / tot,
            100.0 * h[3] / tot);
        std::fflush(stdout);
    }
}

// Самопроверка: MOVE(0,0) с заданной частотой - курсор стоит на месте, а мы
// видим, успевает ли транспорт и поток тайминга.
void transportSelfTest(Transport& tx, double hz, double seconds) {
    tuneTimingThread(-1);
    tx.stats.reset();
    PreciseWaiter waiter(false);
    const double period = static_cast<double>(Clock::freq()) / hz;
    const auto ticks = static_cast<uint64_t>(hz * seconds);
    const int64_t t0 = Clock::now() + Clock::fromUs(2000.0);
    double lateMax = 0.0;
    double lateSum = 0.0;
    uint64_t rejected = 0;
    for (uint64_t k = 0; k < ticks; ++k) {
        const int64_t due = t0 + static_cast<int64_t>(std::llround(static_cast<double>(k) * period));
        waiter.waitUntil(due);
        const double late = Clock::toUs(Clock::now() - due);
        lateMax = std::max(lateMax, late);
        lateSum += late;
        if (!tx.move(0, 0)) ++rejected;
    }
    const uint64_t writes = tx.stats.writes.load();
    std::printf("[check] %s @ %.0f Гц, %.1f с: записей %llu, отклонено %llu, ошибок %llu\n", tx.name(), hz, seconds,
        static_cast<unsigned long long>(writes), static_cast<unsigned long long>(rejected),
        static_cast<unsigned long long>(tx.stats.errors.load()));
    std::printf("[check] вызов отправки: ср %.1f мкс, max %.1f мкс | в полёте max %u | опоздание тика: ср %.1f, max %.1f мкс\n",
        writes ? tx.stats.sumCallUs.load() / writes : 0.0, tx.stats.maxCallUs.load(), tx.stats.maxInflight.load(),
        lateSum / std::max<uint64_t>(1, ticks), lateMax);
}

void printMenu(Pattern pattern, const Options& o, int spread) {
    std::printf("\n=================================================================================\n");
    std::printf(" Горячие клавиши (глобальные - держите в фокусе тестер поллинга):\n");
    for (const auto& p : kPresets) {
        std::printf("   [%s] %s\n", p.key, p.title);
    }
    std::printf("   [F9] сменить траекторию (сейчас: %s)   скорость %.2f пикс/мс, размер %.0f, %.0f с\n",
        patternName(pattern), o.speed, o.size, o.durationS);
    std::printf("   [F11] mouse spread: сейчас %d%% -> по кругу 0/5/10/25/50/100 (live, при выходе вернётся)\n",
        spread);
    std::printf("   [F12] замер руки 6 с без инжекта (калибровка: сравнить HID с тестером)\n");
    std::printf("   [ESC] или та же F-клавиша - стоп прогона      [END] - выход\n");
    std::printf("=================================================================================\n");
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    Options opt;
    if (!parseOptions(argc, argv, opt)) {
        printUsage();
        return 2;
    }

    tuneProcessForTiming();
    std::printf("=== MAKCU mover (COM) | mak-suite SDK + overlapped hot path ===\n");

    MakcuLink link;
    if (!link.init(opt)) {
        restoreProcessTiming();
        return 1;
    }

    if (!opt.keepAccel) {
        if (g_accel.disable()) {
            std::printf(g_accel.wasEnabled()
                    ? "[+] 'Повышенная точность указателя' ВЫКЛЮЧЕНА на время работы (вернётся при выходе)\n"
                    : "[+] 'Повышенная точность указателя' уже выключена\n");
        } else {
            std::printf("[!] Не удалось прочитать/изменить акселерацию указателя\n");
        }
    }

    if (!g_hook.start()) {
        std::printf("[!] LL-хук мыши не установился - HID-частоту программа не покажет\n");
    }
    RawInputMonitor monitor;
    g_rawmon = &monitor;
    if (!monitor.start()) {
        std::printf("[!] Raw Input монитор не запустился - будет только статистика отправки\n");
    }

    // Сразу открываем рабочий транспорт, чтобы первый запуск был мгновенным.
    if (!link.acquire(opt.tx)) {
        g_hook.stop();
        monitor.stop();
        g_accel.restore();
        link.shutdown();
        restoreProcessTiming();
        return 1;
    }

    if (opt.startDelayS > 0.0) {
        std::printf("[*] Ожидание %.0f с перед стартом (мышь не трогать)...\n", opt.startDelayS);
        std::fflush(stdout);
        Sleep(static_cast<DWORD>(opt.startDelayS * 1000.0));
    }
    if (opt.check) {
        if (Transport* tx = link.acquire(opt.tx)) {
            transportSelfTest(*tx, opt.hz, 2.0);
        }
    } else if (opt.sweepSpeed) {
        if (Transport* tx = link.acquire(opt.tx)) {
            Sleep(300);
            runSpeedSweep(opt, *tx, 0);
        }
    } else if (opt.sweep) {
        if (Transport* tx = link.acquire(opt.tx)) {
            Sleep(300);
            runSweep(opt, *tx, 0, false);
        }
    } else if (opt.runOnce) {
        if (Transport* tx = link.acquire(opt.tx)) {
            char label[96];
            std::snprintf(label, sizeof(label), "%.0f Гц | %s", opt.hz, txName(opt.tx));
            Sleep(300);
            runOnce(makeConfig(opt, opt.hz, opt.burst, opt.pattern, opt.durationS, label, opt.pauseEveryS, opt.pauseMs),
                *tx, monitor, 0);
        }
    } else {
        Pattern pattern = opt.pattern;
        printMenu(pattern, opt, link.currentSpread());
        KeyEdge keys;
        keys.sync();
        while (!g_exit.load()) {
            Sleep(15);
            if (keys.pressed(VK_END)) {
                break;
            }
            if (keys.pressed(VK_F11)) {
                int next = kSpreadSteps[0];
                for (size_t i = 0; i < std::size(kSpreadSteps); ++i) {
                    if (kSpreadSteps[i] == link.currentSpread()) {
                        next = kSpreadSteps[(i + 1) % std::size(kSpreadSteps)];
                        break;
                    }
                }
                link.setSpread(next);
            }
            if (keys.pressed(VK_F12)) {
                measureHand(VK_F12);
                printMenu(pattern, opt, link.currentSpread());
                keys.sync();
            }
            if (keys.pressed(VK_F9)) {
                pattern = pattern == Pattern::Circle ? Pattern::Eight
                        : pattern == Pattern::Eight  ? Pattern::Line
                                                     : Pattern::Circle;
                std::printf("[*] Траектория: %s\n", patternName(pattern));
            }
            for (const auto& p : kPresets) {
                if (!keys.pressed(p.vk)) {
                    continue;
                }
                if (p.spread >= 0 && p.spread != link.currentSpread()) {
                    link.setSpread(p.spread);
                }
                Transport* tx = link.acquire(p.tx);
                if (!tx) {
                    std::printf("[-] Транспорт недоступен\n");
                    break;
                }
                if (p.sweep) {
                    runSweep(opt, *tx, p.vk, p.sweep == 2);
                    printMenu(pattern, opt, link.currentSpread());
                    keys.sync();
                    break;
                }
                const std::string label = std::string(p.key) + " " + p.title + " | spread " +
                    std::to_string(link.currentSpread()) + "%";
                const RunConfig cfg = makeConfig(opt, p.hz, 0, p.pattern.value_or(pattern),
                    p.durationS.value_or(opt.durationS), label, p.pauseEveryS, p.pauseMs);
                runOnce(cfg, *tx, monitor, p.vk);
                if (!tx->healthy()) {
                    std::printf("[-] Транспорт сообщил об ошибке (устройство отключено?)\n");
                }
                printMenu(pattern, opt, link.currentSpread());
                keys.sync();
                break;
            }
        }
    }

    std::printf("\n[*] Завершение...\n");
    g_hook.stop();
    monitor.stop();
    g_accel.restore();
    link.shutdown();
    restoreProcessTiming();
    return 0;
}
