// MAKCU mover (COM): стабильная инжекция движения 500..2000 Гц с режимами
// для проверки поллинга. Подробности и методика - README.md рядом.
//
// Сборка: build.bat   (MinGW g++ + статическая libmakxd-cpp из mak-suite)
#include "makcu_transport.h"
#include "motion_engine.h"
#include "mover_common.h"
#include "abc_renderer.h"
#include "hook_monitor.h"
#include "rawinput_monitor.h"
#include "rate_probe.h"

#include <makxd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace mover;

namespace {

// async/sync - бинарный MAK_API MOVE 0x18; km - текст km.move;
// kmnow/kmnowf - km.move_now текстом / в рамке MAK_API ('k');
// rawmove - 0x69 RAW_MOVE с квитанциями 0x6A (один отчёт на запрос, мимо AUTO).
enum class TxKind { Async, Sync, Km, KmNow, KmNowFramed, RawMove, Sdk };

const char* txName(TxKind k) {
    switch (k) {
    case TxKind::Async: return "async";
    case TxKind::Sync: return "sync";
    case TxKind::Km: return "km";
    case TxKind::KmNow: return "kmnow";
    case TxKind::KmNowFramed: return "kmnowf";
    case TxKind::RawMove: return "rawmove";
    case TxKind::Sdk: return "sdk";
    }
    return "?";
}

std::optional<TxKind> parseTx(const std::string& v) {
    for (TxKind k : {TxKind::Async, TxKind::Sync, TxKind::Km, TxKind::KmNow, TxKind::KmNowFramed, TxKind::RawMove,
             TxKind::Sdk}) {
        if (v == txName(k)) return k;
    }
    return std::nullopt;
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

struct Options {
    std::string port;
    double hz = 1000.0;
    TxKind tx = TxKind::RawMove;
    std::vector<TxKind> compare;  // --compare: способы отправки для сравнения
    std::vector<double> rates;    // --rates: частоты команд для --sweep/--compare
    bool probe = false;
    bool dumpSettings = false;    // --dump-settings: снимок всех читаемых настроек платы
    bool pollDevice = false;      // печатать km.device() M:..uf в живой строке
    bool buttonsOff = false;      // выключить поток кнопок (BUTTONS=0), который включает SDK
    bool noSdk = false;           // не подключать SDK вообще: только свой overlapped-handle
    int interp = -1;              // --interp: 0..100 или 255 (AUTO), live через MAK_API 0x1F; -1 - не трогать
    bool touchTest = false;       // сценарий с касанием и подсказками
    bool patternSet = false;      // --pattern задан явно
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
    bool abc = false;            // прогон --run через ABCurves Renderer
    std::string abcModel;        // путь к renderer_global_h80.bin
    std::string abcProfile;      // CSV из 256 отчётов
    uint64_t abcSeed = 0;        // 0 - новый seed на каждый прогон
    bool substep = false;        // прогон --run через субстеппер с накопителем (1000 Гц)
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
AbcRenderer g_abc;
bool g_abcReady = false;
uint64_t g_runCounter = 0;

std::string exeDir() {
    char buf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf);
    const auto slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Модель и профиль ABCurves: по умолчанию относительно exe
// (basics\comport_way -> ..\..\ABCurves\models).
void initAbcurves(const Options& o) {
    const std::string dir = exeDir();
    const std::string model = !o.abcModel.empty() ? o.abcModel
                                                  : dir + "\\..\\..\\ABCurves\\models\\renderer_global_h80.bin";
    const std::string profilePath = !o.abcProfile.empty() ? o.abcProfile : dir + "\\profiles\\human_start_256.csv";
    std::string err;
    std::vector<Report16> profile;
    if (!g_abc.loadModel(model, err) || !AbcRenderer::readProfileCsv(profilePath, profile, err) ||
        !g_abc.prepareProfile(profile, err)) {
        std::printf("[!] ABCurves Renderer недоступен: %s\n", err.c_str());
        return;
    }
    g_abcReady = true;
    std::printf("[+] ABCurves Renderer готов: модель %s, профиль %s\n", model.c_str(), profilePath.c_str());
}
RawInputMonitor* g_rawmon = nullptr;
// --poll-device: запрос km.device() раз в секунду в живой строке.
std::function<std::string()> g_devicePoll;

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
        // После возврата из обработчика Windows сразу завершает процесс. Дать
        // прогону остановиться: runMotion() вызывает tx.finish(), который
        // подтверждает raw-квитанции (иначе они висят на плате до её перезагрузки).
        for (int i = 0; i < 200 && g_live.running.load(); ++i) Sleep(10);
        g_accel.restore();
    }
    return TRUE;
}

void printUsage() {
    std::printf(
        "makcu_mover.exe [опции]\n"
        "  --port COM5          порт (по умолчанию - автопоиск через mak-suite SDK)\n"
        "  --hz N               частота команд, Гц (по умолчанию 1000)\n"
        "  --tx rawmove|async|sync|km|kmnow|kmnowf|sdk  транспорт (по умолчанию rawmove = 0x69 RAW_MOVE\n"
        "                       с квитанциями 0x6A: FIFO, один HID-отчёт на запрос, мимо AUTO; если прошивка его\n"
        "                       не умеет - MOVE 0x18). async = MAK_API 0x18; km = текст km.move;\n"
        "                       kmnow = текст km.move_now; kmnowf = move_now в рамке MAK_API\n"
        "  --probe              снимок состояния платы: сырые ответы 0x04/0x1F/0x10, km.device() и т.п.\n"
        "  --dump-settings      все читаемые настройки платы -> консоль, makcu_settings_fw<N>.txt и\n"
        "                       .makxd-settings (официальный экспорт). Запускать с --no-sdk. Ничего не меняет\n"
        "  --compare A,B,..     сравнить способы отправки (напр. async,km,kmnow,kmnowf) на частотах --rates\n"
        "  --rates N,N,..       частоты команд для --sweep/--compare (по умолчанию 125,250,500,1000,2000)\n"
        "  --poll-device        в живой строке показывать km.device() M:..uf\n"
        "  --buttons-off        выключить поток кнопок BUTTONS (его включает SDK при connect)\n"
        "  --no-sdk             не подключать SDK (без handshake/BUTTONS/чтения настроек), сразу свой COM-handle\n"
        "  --interp auto|N      интерполяция инжекта (MAK_API 0x1F, как слайдер app.makcu.com): auto = 255\n"
        "                       (заводское 'по частоте команд'), N = 0..100; live, при выходе вернётся исходное\n"
        "  --touch-test         30 с со звуковыми подсказками: база / касание / после / резкий мах / после\n"
        "                       (частота --hz, траектория по умолчанию ramp - точный учёт пути)\n"
        "  --pause-every S      диагностика: каждые S секунд пауза --pause-ms\n"
        "  --pause-ms M         длительность паузы, мс\n"
        "  --pattern circle|eight|line|ramp|wobble|flicks|steps  (steps - лестница скоростей 0.05..8\n"
        "                       отсчётов/мс по 2 с, для замера скорость -> частота)\n"
        "  --speed V            скорость, пикс/мс (по умолчанию 4.0; для оверсэмплинга нужно >= 3)\n"
        "  --size S             радиус/амплитуда, пикс (150)\n"
        "  --duration S         длительность прогона, с (10)\n"
        "  --burst N            имитация руки: N команд подряд на секунде --burst-at\n"
        "  --burst-at S         когда делать всплеск, с (3)\n"
        "  --wait spin|hybrid   spin = мин. джиттер (по умолчанию), hybrid = меньше CPU\n"
        "  --core N             закрепить поток тайминга на ядре N\n"
        "  --spread N           временно задать mouse spread 0..100%% (вернётся при выходе)\n"
        "  --keep-accel         НЕ отключать 'Повышенную точность указателя'\n"
        "  --abc                прогон --run через ABCurves Renderer (1000 Гц, лучше с --pattern flicks)\n"
        "  --abc-model PATH     модель рендерера (по умолч. ..\\..\\ABCurves\\models\\renderer_global_h80.bin)\n"
        "  --abc-profile PATH   профиль: CSV из 256 отчётов dx,dy (по умолч. profiles\\human_start_256.csv)\n"
        "  --abc-seed N         seed рендерера и рывков (по умолчанию новый на каждый прогон)\n"
        "  --substep            прогон --run через субстеппер с накопителем: шаг 1 мс, отчёт = округлённое\n"
        "                       накопленное намерение, без шума; после прогона - таблица скорость -> частота\n"
        "                       (лучше с --pattern steps или flicks)\n"
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
            const auto k = parseTx(next("--tx"));
            if (!k) return false;
            o.tx = *k;
        } else if (a == "--compare") {
            for (const auto& v : splitList(next("--compare"))) {
                const auto k = parseTx(v);
                if (!k || *k == TxKind::Sdk) return false;
                o.compare.push_back(*k);
            }
        } else if (a == "--rates") {
            for (const auto& v : splitList(next("--rates"))) {
                const double hz = std::atof(v.c_str());
                if (hz < 10.0 || hz > 8000.0) return false;
                o.rates.push_back(hz);
            }
        } else if (a == "--probe") o.probe = true;
        else if (a == "--dump-settings") o.dumpSettings = true;
        else if (a == "--poll-device") o.pollDevice = true;
        else if (a == "--buttons-off") o.buttonsOff = true;
        else if (a == "--no-sdk") o.noSdk = true;
        else if (a == "--interp") {
            const std::string v = next("--interp");
            o.interp = v == "auto" ? 255 : std::atoi(v.c_str());
            if (o.interp != 255 && (o.interp < 0 || o.interp > 100)) return false;
        }
        else if (a == "--touch-test") o.touchTest = true;
        else if (a == "--pattern") {
            o.patternSet = true;
            const std::string v = next("--pattern");
            if (v == "circle") o.pattern = Pattern::Circle;
            else if (v == "eight") o.pattern = Pattern::Eight;
            else if (v == "line") o.pattern = Pattern::Line;
            else if (v == "ramp") o.pattern = Pattern::Ramp;
            else if (v == "wobble") o.pattern = Pattern::Wobble;
            else if (v == "flicks") o.pattern = Pattern::Flicks;
            else if (v == "steps") o.pattern = Pattern::Steps;
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
        else if (a == "--abc") o.abc = true;
        else if (a == "--substep") o.substep = true;
        else if (a == "--abc-model") o.abcModel = next("--abc-model");
        else if (a == "--abc-profile") o.abcProfile = next("--abc-profile");
        else if (a == "--abc-seed") o.abcSeed = std::strtoull(next("--abc-seed"), nullptr, 10);
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
        buttonsOff_ = opt.buttonsOff;
        interpWanted_ = opt.interp;
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

        if (opt.noSdk) {
            // Контроль "общего фактора": порт ни разу не открывается SDK.
            std::printf("[*] --no-sdk: SDK не подключается, порт откроет только raw-транспорт\n");
            active_ = Active::None;
            return true;
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
            if (buttonsOff_) {
                const uint8_t off[6] = {0xDE, 0xAD, 0x01, 0x00, static_cast<uint8_t>(makxd::ApiOpcode::BUTTONS), 0x00};
                (void)raw_.transact(off, sizeof(off), std::chrono::milliseconds(30));
                const auto state = raw_.kmQuery("km.buttons()", std::chrono::milliseconds(200));
                std::printf("[+] Поток кнопок выключен (BUTTONS=0), km.buttons() = %s\n", state ? state->c_str() : "?");
            }
            if (interpWanted_ >= 0 && !interpOriginal_) {
                interpOriginal_ = getInterp();
                if (!interpOriginal_) {
                    std::printf("[!] Интерполяция 0x1F не прочитана - не меняю\n");
                } else if (setInterp(static_cast<uint8_t>(interpWanted_))) {
                    interpChanged_ = *interpOriginal_ != interpWanted_;
                    std::printf("[+] Интерполяция 0x1F: %s -> %s (live, без сохранения; при выходе вернётся)\n",
                        interpName(*interpOriginal_).c_str(), interpName(static_cast<uint8_t>(interpWanted_)).c_str());
                } else {
                    std::printf("[!] Плата не приняла интерполяцию %s\n", interpName(static_cast<uint8_t>(interpWanted_)).c_str());
                }
            }
        }
        active_ = Active::Raw;
        raw_.setMode(kind == TxKind::Sync ? RawComTransport::Mode::Sync : RawComTransport::Mode::Async);
        using Format = RawComTransport::Format;
        raw_.setFormat(kind == TxKind::Km            ? Format::KmMove
                       : kind == TxKind::KmNow       ? Format::KmMoveNow
                       : kind == TxKind::KmNowFramed ? Format::KmMoveNowFramed
                       : kind == TxKind::RawMove     ? Format::RawMove
                                                     : Format::Mak);
        if (kind == TxKind::RawMove) {
            std::string info;
            if (!raw_.prepareRaw(info)) {
                // Прошивка без Raw API: работаем через MAK_API 0x18 (со сглаживанием прошивки).
                std::printf("[!] Raw movement 0x69/0x6A недоступен (%s) - использую MOVE 0x18\n", info.c_str());
                raw_.setFormat(Format::Mak);
            } else if (!rawAnnounced_) {
                rawAnnounced_ = true;
                std::printf("[+] Raw movement 0x69/0x6A: %s\n", info.c_str());
            }
        }
        return &raw_;
    }

    [[nodiscard]] RawComTransport* raw() { return active_ == Active::Raw ? &raw_ : nullptr; }

    // Период мыши из km.device() ("R:MK;M:8uf;..." -> "8uf"), "?" если недоступно.
    std::string mouseUf() {
        if (active_ != Active::Raw) return "?";
        const auto km = raw_.kmQuery("km.device()", std::chrono::milliseconds(200));
        if (!km) return "?";
        const auto m = km->find("M:");
        if (m == std::string::npos) return *km;
        const auto end = km->find(';', m);
        return km->substr(m + 2, end == std::string::npos ? std::string::npos : end - m - 2);
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
        restoreInterp();
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

    // Снимок всех читаемых настроек платы в консоль и в файлы рядом с exe:
    // makcu_settings_fw<N>.txt и makcu_settings_fw<N>.makxd-settings (официальный
    // экспорт, можно импортировать в app.makcu.com). Настройки не меняются.
    // SDK connect включает поток кнопок (BUTTONS=1), поэтому сырые запросы
    // идут первыми (запускать с --no-sdk), а BUTTONS в конце возвращается.
    void dumpSettings() {
        std::string text;
        auto line = [&](const std::string& s) {
            text += s + "\n";
            std::printf("%s\n", s.c_str());
        };
        auto hex = [](const std::vector<uint8_t>& v, size_t from, size_t n) {
            std::string s;
            char b[4];
            for (size_t i = from; i < from + n && i < v.size(); ++i) {
                std::snprintf(b, sizeof(b), "%02X ", v[i]);
                s += b;
            }
            if (!s.empty()) s.pop_back();
            return s;
        };
        if (active_ == Active::Sdk) {
            line("[!] SDK уже подключался: BUTTONS ниже может быть 1 из-за SDK (для точного снимка - --no-sdk)");
        }
        if (!acquire(TxKind::Async)) {
            line("[-] COM не открыт");
            return;
        }
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char when[64];
        std::snprintf(when, sizeof(when), "%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        line(std::string("# Снимок настроек MAKCU, ") + when + ", порт " + port_ + ", " + std::to_string(raw_.baud()) +
             " бод");

        // --- MAK_API GET: ответ DE AD LEN CMD PAYLOAD ---
        line("");
        line("## MAK_API (GET)");
        uint32_t fw = 0;
        std::optional<uint8_t> buttonsOriginal;
        struct BinQuery {
            const char* label;
            std::vector<uint8_t> frame;
        };
        const std::vector<BinQuery> bins = {
            {"0x02 DEVICE (kinds)", {0xDE, 0xAD, 0x00, 0x00, 0x02}},
            {"0x04 FIRMWARE_VERSION", {0xDE, 0xAD, 0x00, 0x00, 0x04}},
            {"0x1F interpolation (255=AUTO)", {0xDE, 0xAD, 0x00, 0x00, 0x1F}},
            {"0x10 BUTTONS stream", {0xDE, 0xAD, 0x00, 0x00, 0x10}},
            {"0x2B KEY_KEYS stream", {0xDE, 0xAD, 0x00, 0x00, 0x2B}},
            {"0x41 CONTROLLER_STREAM", {0xDE, 0xAD, 0x00, 0x00, 0x41}},
            {"0x52 INPUT_STREAM mouse", {0xDE, 0xAD, 0x01, 0x00, 0x52, 0x01}},
            {"0x52 INPUT_STREAM keyboard", {0xDE, 0xAD, 0x01, 0x00, 0x52, 0x02}},
            {"0x52 INPUT_STREAM controller", {0xDE, 0xAD, 0x01, 0x00, 0x52, 0x03}},
            {"0x6A RAW_STATUS id=0 (metadata)", {0xDE, 0xAD, 0x05, 0x00, 0x6A, 0, 0, 0, 0, 0}},
        };
        for (const auto& q : bins) {
            const auto rx = raw_.transact(q.frame.data(), q.frame.size(), std::chrono::milliseconds(80));
            std::string value = "(нет ответа)";
            for (size_t i = 0; i + 5 <= rx.size(); ++i) {
                if (rx[i] != 0xDE || rx[i + 1] != 0xAD || rx[i + 4] != q.frame[4]) continue;
                const size_t len = static_cast<size_t>(rx[i + 2]) | static_cast<size_t>(rx[i + 3]) << 8;
                if (i + 5 + len > rx.size()) break;
                value = len ? hex(rx, i + 5, len) : std::string("(пусто)");
                const uint8_t* p = &rx[i + 5];
                if (q.frame[4] == 0x04 && len == 4) {
                    fw = static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                         static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
                    value += " = " + std::to_string(fw);
                } else if (q.frame[4] == 0x1F && len == 1) {
                    value += p[0] == 255 ? " = AUTO (или отказ)" : " = " + std::to_string(p[0]);
                } else if (q.frame[4] == 0x10 && len == 1) {
                    buttonsOriginal = p[0];
                } else if (q.frame[4] == 0x6A && len == 8) {
                    const uint32_t last = static_cast<uint32_t>(p[2]) | static_cast<uint32_t>(p[3]) << 8 |
                                          static_cast<uint32_t>(p[4]) << 16 | static_cast<uint32_t>(p[5]) << 24;
                    value += " = schema " + std::to_string(p[1]) + ", last_accepted_id " + std::to_string(last) +
                             ", свободно " + std::to_string(p[6]) + "/" + std::to_string(p[7]);
                }
                break;
            }
            line(std::string("- ") + q.label + ": " + value);
        }

        // --- KM_API запросы (только чтение) ---
        line("");
        line("## KM_API (запросы)");
        const char* kmQueries[] = {"km.version()", "km.device()", "km.baud()", "km.echo()", "km.buttons()",
            "km.keys()", "km.controller()", "km.stream(mouse)", "km.stream(keyboard)", "km.stream(controller)",
            "km.screen()", "km.getpos()", "km.moving()", "km.left()", "km.right()", "km.middle()", "km.side1()",
            "km.side2()", "km.lock_mx()", "km.lock_my()", "km.lock_mw()", "km.lock_mx+()", "km.lock_mx-()",
            "km.lock_my+()", "km.lock_my-()", "km.lock_mw+()", "km.lock_mw-()", "km.lock_ml()", "km.lock_mr()",
            "km.lock_mm()", "km.lock_ms1()", "km.lock_ms2()"};
        for (const char* q : kmQueries) {
            const auto r = raw_.kmQuery(q, std::chrono::milliseconds(200));
            line(std::string("- ") + q + " -> " + (r ? *r : std::string("(нет ответа)")));
        }

        // --- SDK: настройки (Toolkit / app.makcu.com) ---
        line("");
        line("## Настройки устройства (SDK readDeviceSettings)");
        raw_.close();
        active_ = Active::None;
        Sleep(150);
        std::vector<uint8_t> exportFile;
        if (!sdk_.connect(port_)) {
            line("- SDK connect не удался: " + sdk_.getLastError());
        } else {
            active_ = Active::Sdk;
            try {
                const auto info = sdk_.deviceSettingsInfo();
                const auto snap = sdk_.readDeviceSettings();
                const auto& s = snap.settings;
                char b[256];
                std::snprintf(b, sizeof(b), "- info: sections 0x%02X (1=controller 2=translation 4=mouse), kinds 0x%02X, "
                    "save_state %u, revision %lu", info.sections, info.kinds, info.save_state,
                    static_cast<unsigned long>(info.revision));
                line(b);
                line("- mouse_spread_percent (= интерполяция 0x1F): " + std::to_string(s.mouse_spread_percent));
                const auto& c = s.controller;
                std::snprintf(b, sizeof(b), "- controller: interpolation %u (0 off, 1 fixed, 2 auto), buffer_ms %u, "
                    "timing_variance %u%%, curve_enabled %u, legacy_strengths %u/%u/%u, profile %u из %u",
                    c.interpolation, c.buffer_ms, c.timing_variance_percent, c.curve_enabled, c.legacy_strengths[0],
                    c.legacy_strengths[1], c.legacy_strengths[2], c.selected_profile, c.profile_count);
                line(b);
                const char* channels[] = {"right stick", "left stick", "left trigger", "right trigger"};
                for (int i = 0; i < 4; ++i) {
                    const auto& v = c.behaviors[i];
                    std::snprintf(b, sizeof(b), "  - behavior %d (%s) '%s': enabled %u, strength %u%% (present %u), "
                        "curve %u, advanced %u, inertia %u, micro %u, limit %u, magnitude_var %u, angle_var %u",
                        i, channels[i], v.name, v.enabled, v.strength_percent, v.strength_present, v.curve_enabled,
                        v.advanced_enabled, v.inertia_percent, v.micro_percent, v.limit_percent,
                        v.magnitude_variance_percent, v.angle_variance_percent);
                    line(b);
                    const auto& cv = v.curves[i];
                    std::snprintf(b, sizeof(b), "    curve[%d]: deadzone %u, anti_deadzone %u, deadband %u, "
                        "points (%u,%u) (%u,%u) (%u,%u) (%u,%u) (%u,%u)", i, cv.center_deadzone_percent,
                        cv.anti_deadzone_percent, cv.change_deadband_percent, cv.points[0].input_percent,
                        cv.points[0].output_percent, cv.points[1].input_percent, cv.points[1].output_percent,
                        cv.points[2].input_percent, cv.points[2].output_percent, cv.points[3].input_percent,
                        cv.points[3].output_percent, cv.points[4].input_percent, cv.points[4].output_percent);
                    line(b);
                }
                const char* tr[] = {"left stick/WASD", "right stick/mouse", "left trigger", "right trigger"};
                for (int i = 0; i < 4; ++i) {
                    std::snprintf(b, sizeof(b), "- translation %d (%s): enabled %u, scale %u, timeout_ms %u", i, tr[i],
                        s.translation[i].enabled, s.translation[i].scale, s.translation[i].timeout_ms);
                    line(b);
                }
                const auto image = makxd::detail::settingsEncode(s);
                const std::vector<uint8_t> img(image.begin(), image.end());
                line("- образ настроек (400 байт: 0..371 controller, 372..395 translation, 396 mouse spread, 397..399):");
                for (size_t off = 0; off < img.size(); off += 32) {
                    char o[16];
                    std::snprintf(o, sizeof(o), "    %03zu: ", off);
                    line(o + hex(img, off, 32));
                }
                exportFile = sdk_.exportDeviceSettings(snap);
            } catch (const std::exception& e) {
                line(std::string("- ошибка чтения настроек: ") + e.what());
            }
        }

        // Вернуть поток кнопок, который включил SDK connect.
        if (acquire(TxKind::Async) && buttonsOriginal) {
            const uint8_t set[6] = {0xDE, 0xAD, 0x01, 0x00, 0x10, *buttonsOriginal};
            (void)raw_.transact(set, sizeof(set), std::chrono::milliseconds(30));
            line("");
            line("(BUTTONS возвращён в " + std::to_string(*buttonsOriginal) + ")");
        }

        const std::string base = exeDir() + "\\makcu_settings_fw" + std::to_string(fw);
        if (FILE* f = std::fopen((base + ".txt").c_str(), "wb")) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
            std::printf("[+] Снимок записан: %s.txt\n", base.c_str());
        }
        if (!exportFile.empty()) {
            if (FILE* f = std::fopen((base + ".makxd-settings").c_str(), "wb")) {
                std::fwrite(exportFile.data(), 1, exportFile.size(), f);
                std::fclose(f);
                std::printf("[+] Экспорт настроек (app.makcu.com / SDK import): %s.makxd-settings (%zu байт)\n",
                    base.c_str(), exportFile.size());
            }
        }
    }

private:
    // MAK_API 0x1F - интерполяция инжекта мыши (слайдер "Mouse injection" в
    // app.makcu.com): 0..100 или 255 = AUTO. GET: пустой payload, SET: 1 байт,
    // ответ на оба - DE AD 01 00 1F <значение>. Отказ (FF) от AUTO по байтам
    // не отличить, поэтому SET проверяем повторным GET.
    static std::string interpName(uint8_t v) { return v == 255 ? std::string("AUTO") : std::to_string(v); }

    std::optional<uint8_t> getInterp() {
        const uint8_t req[5] = {0xDE, 0xAD, 0x00, 0x00, 0x1F};
        const auto rx = raw_.transact(req, sizeof(req), std::chrono::milliseconds(60));
        for (size_t i = 0; i + 6 <= rx.size(); ++i) {
            if (rx[i] == 0xDE && rx[i + 1] == 0xAD && rx[i + 2] == 0x01 && rx[i + 3] == 0x00 && rx[i + 4] == 0x1F) {
                return rx[i + 5];
            }
        }
        return std::nullopt;
    }

    bool setInterp(uint8_t value) {
        const uint8_t req[6] = {0xDE, 0xAD, 0x01, 0x00, 0x1F, value};
        (void)raw_.transact(req, sizeof(req), std::chrono::milliseconds(60));
        const auto now = getInterp();
        return now && *now == value;
    }

    void restoreInterp() {
        if (!interpChanged_ || !interpOriginal_) return;
        if (active_ == Active::Sdk) {
            sdkTx_.reset();
            sdk_.disconnect();
            Sleep(150);
            active_ = Active::None;
        }
        if (!raw_.isOpen()) {
            std::string error;
            if (!raw_.open(port_, error)) {
                std::printf("[!] Интерполяцию вернуть не удалось: %s (вернётся после переподключения платы)\n", error.c_str());
                return;
            }
        }
        if (setInterp(*interpOriginal_)) {
            interpChanged_ = false;
            std::printf("[+] Интерполяция 0x1F возвращена в %s\n", interpName(*interpOriginal_).c_str());
        } else {
            std::printf("[!] Интерполяцию вернуть не удалось (вернётся после переподключения платы)\n");
        }
    }

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
    bool buttonsOff_ = false;
    bool rawAnnounced_ = false;
    int interpWanted_ = -1;
    std::optional<uint8_t> interpOriginal_;
    bool interpChanged_ = false;
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
    double pauseEveryS;
    double pauseMs;
    std::optional<Pattern> pattern;
    std::optional<double> durationS;
    int sweep = 0;    // 1 - перебор без касания, 2 - перебор после касания
    int spread = -1;  // mouse spread перед стартом (live, без NOR); -1 - не трогать
    bool abc = false; // через ABCurves Renderer
    bool substep = false; // через субстеппер с накопителем
};

// Транспорт пресетов - из --tx (по умолчанию rawmove). Замеры на 4094
// (2026-10-10): MOVE 0x18 / km.move / km.move_now размазываются прошивкой на
// несколько отчётов, выход ~640-690 Гц; raw 0x69 - ровно один отчёт на
// команду, 1000 команд/с -> ~1000 Гц, путь точный.
const Preset kPresets[] = {
    {VK_F1, "F1", " 250 Гц | 30 с", 250, 0, 0, {}, 30.0},
    {VK_F2, "F2", "1000 Гц | 30 с (основной режим)", 1000, 0, 0, {}, 30.0},
    {VK_F3, "F3", "ABCurves | рывки к целям | 30 с", 1000, 0, 0, Pattern::Flicks, 30.0, 0, -1, true},
    {VK_F4, "F4", "накопитель (без рендерера) | те же рывки | 30 с (сравнение)", 1000, 0, 0, Pattern::Flicks, 30.0,
        0, -1, false, true},
    {VK_F5, "F5", "ABCurves | круг | 30 с", 1000, 0, 0, Pattern::Circle, 30.0, 0, -1, true},
    {VK_F6, "F6", "ПЕРЕБОР 125..2000 Гц: HID + путь, мышь не трогать (~25 с)", 0, 0, 0, {}, {}, 1},
    {VK_F7, "F7", "ПЕРЕБОР ПОСЛЕ КАСАНИЯ: 6 с с касанием, затем 125..2000 Гц", 0, 0, 0, {}, {}, 2},
    {VK_F8, "F8", "1000 Гц | линия 60 с (тест с рукой)", 1000, 0, 0, Pattern::Line, 60.0},
    {VK_F10, "F10", "накопитель | лестница скоростей 0.05..8 отсч/мс по 2 с | 24 с (скорость -> частота)", 1000, 0,
        0, Pattern::Steps, 24.0, 0, -1, false, true},
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
    if (const std::string extra = tx.extraStats(); !extra.empty()) {
        std::printf("%s\n", extra.c_str());
    }

    std::printf("Повышенная точность указателя сейчас: %s\n",
        pointerAccelOn() ? "ВКЛЮЧЕНА (курсор будет ускоряться при крупных дельтах!)" : "выключена");
    if (cfg.substep && !cfg.renderer) {
        std::printf("Накопитель: ненулевых отчётов %.0f/с\n", l.rendererReports.load() / elapsed);
    }
    if (cfg.renderer) {
        std::printf("ABCurves Renderer: ненулевых отчётов %.0f/с (seed %llu)%s\n",
            l.rendererReports.load() / elapsed, static_cast<unsigned long long>(cfg.rendererSeed),
            l.rendererError.load() ? " | ОШИБКА рендерера, прогон прерван" : "");
    }
    std::printf("HID (LL-хук): в среднем %.0f Гц | окна 1 с: медиана %.0f, худшее %.0f Гц | отчётов на команду %.2f\n",
        hid.avgHz, hid.medianHz, hid.worstHz, writes ? static_cast<double>(g_hook.count()) / writes : 0.0);
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
    tx.resetStats();
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
                std::printf("  [%5.1f с] HID %5.0f Гц | команд %5.0f/с | путь x%.2f | отложено %llu | опозд.max %5.0f мкс",
                    g_live.elapsedS.load(), hidHz, (moves - lastMoves) * 1e6 / dtUs, ratio,
                    static_cast<unsigned long long>(g_live.deferred.load()), g_live.windowLateMaxUs.exchange(0.0));
                if (g_devicePoll) {
                    std::printf(" | M:%s", g_devicePoll().c_str());
                }
                std::printf("\n");
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

void runOnce(const RunConfig& base, Transport& tx, RawInputMonitor& monitor, int stopVk) {
    // Шаг 1 мс (рендерер или накопитель) - пишем журнал тиков и событий хука
    // для таблицы "скорость -> частота".
    RunConfig cfg = base;
    TickLog log;
    const bool probe = cfg.renderer || cfg.substep;
    if (probe) {
        log.reserve(static_cast<size_t>(cfg.durationS * 1000.0) + 2000);
        cfg.log = &log;
        g_hook.startLog();
    }
    monitor.reset();
    std::printf("\n>>> СТАРТ: %s | %s | %s | %.1f с  (стоп: %s / ESC)\n", cfg.label.c_str(), tx.name(),
        patternName(cfg.trajectory.pattern), cfg.durationS, stopVk ? "та же F-клавиша" : "ESC");
    const HidWindowStats hid = runAndWatch(cfg, tx, stopVk, true);
    Sleep(50);  // дать Raw Input дособрать последние сообщения
    printSummary(cfg, tx, monitor, hid);
    if (probe) {
        const RateSeries series = seriesFromRun(log, g_hook.stopLog());
        printRateTable(series, cfg.renderer ? "ABCurves Renderer" : "Накопитель");
        const std::string csv = exeDir() + (cfg.renderer ? "\\rate_abc.csv" : "\\rate_substep.csv");
        if (writeRateCsv(series, csv)) std::printf("  по миллисекундам: %s\n", csv.c_str());
    }
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
    // Каждый прогон - свои рывки и свой поток рендерера (если seed не задан).
    const uint64_t seed = o.abcSeed != 0 ? o.abcSeed : GetTickCount64() + (++g_runCounter) * 7919u;
    if (pattern == Pattern::Flicks) {
        cfg.trajectory.flicks = makeFlicks(seed, durationS * 1000.0, o.size * 2.0);
    }
    cfg.rendererSeed = seed;
    return cfg;
}

RunConfig withSubstep(RunConfig cfg) {
    cfg.substep = true;
    cfg.hz = 1000.0;  // накопитель шагает по 1 мс, как рендерер
    return cfg;
}

RunConfig withRenderer(RunConfig cfg) {
    cfg.renderer = &g_abc;
    cfg.hz = 1000.0;  // модель обучена на тиках 1 мс
    return cfg;
}

// Калибровка замера: без инжекта, человек водит реальной мышью. Если хук
// покажет ~1000 Гц, как и тестер, хуку можно верить и в остальных режимах.
void measureHand(int stopVk) {
    std::printf("\n>>> ЗАМЕР РУКИ: 15 с, инжекта нет - водите реальной мышью в середине экрана с РАЗНОЙ\n"
                "    скоростью: очень медленно, медленно, средне, быстро (по несколько секунд)\n");
    int pointerSpeed = 0;
    if (SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &pointerSpeed, 0) && pointerSpeed != 10) {
        std::printf("[!] Скорость указателя Windows %d/20 (не 10): скорость руки в таблице будет не в отсчётах\n",
            pointerSpeed);
    }
    g_hook.reset();
    g_hook.startLog();
    KeyEdge keys;
    keys.sync();
    uint64_t last = 0;
    int64_t t = Clock::now();
    std::vector<double> rates;
    for (int sec = 0; sec < 15 && !g_exit.load(); ++sec) {
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
    const RateSeries series = seriesFromHand(g_hook.stopLog());
    printRateTable(series, "Рука");
    const std::string csv = exeDir() + "\\rate_hand.csv";
    if (writeRateCsv(series, csv)) std::printf("  по миллисекундам: %s\n", csv.c_str());
}

// Перебор частот команд: таблица "команд/с -> HID Гц и путь". Траектория -
// линия (монотонные отрезки), чтобы длина пути через Raw Input была точной.
//  afterTouch = false: перед каждой частотой пауза 0.5 с (свежее состояние
//                      прошивки, мышь не трогать).
//  afterTouch = true : сначала 6 с при 1000 Гц, во время которых нужно
//                      коснуться мыши и убрать руку; дальше частоты идут
//                      подряд без пауз (состояние "после руки" сохраняется).
// Один замер перебора: монотонная траектория 4 с на частоте hz.
struct RateRow {
    HidWindowStats hid;
    double reportsPerCmd = 0.0;  // HID-отчётов на одну команду (сглаживание > 1, склейка < 1)
    double oneFramePct = 0.0;
    double twoFramePct = 0.0;
    double longPct = 0.0;        // интервалы > 2.5 мс
};

RateRow measureRate(const Options& o, Transport& tx, double hz, int stopVk) {
    const Pattern pat = o.pattern == Pattern::Wobble ? Pattern::Wobble : Pattern::Ramp;
    const RunConfig cfg = makeConfig(o, hz, 0, pat, 4.0, "sweep");
    RateRow r;
    r.hid = runAndWatch(cfg, tx, stopVk, false);
    const auto h = g_hook.histogram();
    uint64_t total = 0;
    for (auto v : h) total += v;
    const double tot = static_cast<double>(std::max<uint64_t>(1, total));
    r.oneFramePct = 100.0 * h[1] / tot;
    r.twoFramePct = 100.0 * h[3] / tot;
    r.longPct = 100.0 * (h[4] + h[5]) / tot;
    const uint64_t writes = tx.stats.writes.load();
    r.reportsPerCmd = writes ? static_cast<double>(g_hook.count()) / static_cast<double>(writes) : 0.0;
    return r;
}

std::vector<double> sweepRates(const Options& o) {
    return o.rates.empty() ? std::vector<double>{125, 250, 500, 1000, 2000} : o.rates;
}

void runSweep(const Options& o, Transport& tx, int stopVk, bool afterTouch) {
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
    std::printf("  команд/с | HID ср, Гц | HID худш., Гц | отч/команду | путь пришло/отпр. | 1 кадр %% | 2 кадра %% | >2.5 мс %%\n");
    for (double hz : sweepRates(o)) {
        if (g_live.stopRequest.load() || g_exit.load()) break;
        if (!afterTouch) Sleep(500);
        const RateRow r = measureRate(o, tx, hz, stopVk);
        if (g_live.stopRequest.load()) break;
        std::printf("  %8.0f | %10.0f | %13.0f | %11.2f | %17.2f | %8.1f | %9.1f | %9.1f\n", hz, r.hid.avgHz,
            r.hid.worstHz, r.reportsPerCmd, r.hid.pathRatio, r.oneFramePct, r.twoFramePct, r.longPct);
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

// Сравнение способов отправки: для каждой частоты все способы подряд, в одном
// и том же состоянии прошивки (мышь не трогать). Отчётов на команду > 1 -
// прошивка размазывает команду на несколько отчётов, < 1 - склеивает команды.
void runCompare(const Options& o, MakcuLink& link) {
    std::printf("\n>>> СРАВНЕНИЕ СПОСОБОВ ОТПРАВКИ: ramp %.1f пикс/мс, по 4 с на ячейку. Мышь НЕ трогать.\n", o.speed);
    std::printf("  M до начала: %s\n", link.mouseUf().c_str());
    std::printf("  команд/с | способ        | HID ср | HID худш | отч/команду | путь | 1 кадр %% | 2 кадра %% | >2.5 мс %% | M после\n");
    g_live.stopRequest = false;
    for (double hz : sweepRates(o)) {
        for (TxKind kind : o.compare) {
            if (g_live.stopRequest.load() || g_exit.load()) return;
            Transport* tx = link.acquire(kind);
            if (!tx) return;
            Sleep(500);
            const RateRow r = measureRate(o, *tx, hz, 0);
            if (g_live.stopRequest.load()) return;
            std::printf("  %8.0f | %-13s | %6.0f | %8.0f | %11.2f | %4.2f | %8.1f | %9.1f | %9.1f | %s\n", hz, tx->name(),
                r.hid.avgHz, r.hid.worstHz, r.reportsPerCmd, r.hid.pathRatio, r.oneFramePct, r.twoFramePct, r.longPct,
                link.mouseUf().c_str());
            if (const std::string extra = tx->extraStats(); !extra.empty()) {
                std::printf("           %s\n", extra.c_str());
            }
            std::fflush(stdout);
        }
    }
}

// Сценарий касания без масок. В фазах "руки нет" весь путь в Windows - это
// инжект, поэтому путь пришло/отправлено там точный (на ramp), и видно,
// меняет ли касание траекторию и темп выдачи ПОСЛЕ того, как рука убрана.
struct TouchPhase {
    double startS;
    const char* label;
    const char* instruction;
    DWORD beepHz;   // 0 - без сигнала
    bool handsOff;  // путь фазы - чистый инжект
};

constexpr TouchPhase kTouchPhases[] = {
    {0.0, "A база", "мышь НЕ трогать", 0, true},
    {6.0, "B касание", ">>> СЕЙЧАС: медленно и чуть-чуть подвигайте мышь (2 с)", 880, false},
    {8.0, "C после касания", ">>> УБЕРИТЕ руку и не трогайте", 440, true},
    {18.0, "D резкий мах", ">>> СЕЙЧАС: один быстрый резкий мах мышью", 880, false},
    {19.0, "E после маха", ">>> УБЕРИТЕ руку и не трогайте до конца", 440, true},
};
constexpr double kTouchTestS = 30.0;

// printf("%-Ns") считает байты, а кириллица в UTF-8 занимает 2 байта на букву.
std::string padUtf8(const std::string& s, size_t width) {
    size_t chars = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++chars;
    }
    return chars >= width ? s : s + std::string(width - chars, ' ');
}

void runTouchTest(const Options& o, Transport& tx, MakcuLink& link) {
    constexpr size_t kN = std::size(kTouchPhases);
    struct Acc {
        double t0 = 0.0, t1 = 0.0;
        uint64_t hid0 = 0, hid1 = 0;
        std::array<uint64_t, 6> h0{}, h1{};
        int64_t sent0 = 0, sent1 = 0, recv0 = 0, recv1 = 0;
        std::string uf;
    };
    std::array<Acc, kN> acc{};

    const Pattern pat = o.patternSet ? o.pattern : Pattern::Ramp;
    std::printf("\n>>> ТЕСТ КАСАНИЯ: %.0f команд/с, %s %.1f пикс/мс, %.0f с. Следуйте подсказкам и сигналам.\n", o.hz,
        patternName(pat), o.speed, kTouchTestS);
    for (const auto& p : kTouchPhases) {
        std::printf("    %4.0f с  %s %s\n", p.startS, padUtf8(p.label, 16).c_str(), p.instruction);
    }
    std::printf("  Начало через 3 с, мышь не трогать...\n");
    std::fflush(stdout);
    Sleep(3000);

    g_live.reset();
    tx.resetStats();
    g_hook.reset();
    if (g_rawmon) g_rawmon->reset();
    const RunConfig cfg = makeConfig(o, o.hz, 0, pat, kTouchTestS, "touch-test");
    std::thread worker([&] { runMotion(cfg, tx, g_live); });
    const int64_t waitStart = Clock::now();
    while (!g_live.running.load() && Clock::toUs(Clock::now() - waitStart) < 500000) Sleep(1);

    auto open = [&](Acc& a) {
        a.t0 = g_live.elapsedS.load();
        a.hid0 = g_hook.count();
        a.h0 = g_hook.histogram();
        a.sent0 = g_live.sentAbs.load();
        a.recv0 = receivedPathAbs();
    };
    auto close = [&](Acc& a) {
        a.t1 = g_live.elapsedS.load();
        a.hid1 = g_hook.count();
        a.h1 = g_hook.histogram();
        a.sent1 = g_live.sentAbs.load();
        a.recv1 = receivedPathAbs();
        a.uf = link.mouseUf();
    };

    KeyEdge keys;
    keys.sync();
    size_t phase = 0;
    open(acc[0]);
    std::printf("  [ 0 с] %s: %s\n", kTouchPhases[0].label, kTouchPhases[0].instruction);
    double nextPrint = 1.0;
    uint64_t lastHid = g_hook.count();
    int64_t lastSent = g_live.sentAbs.load(), lastRecv = receivedPathAbs();
    int64_t lastT = Clock::now();
    while (g_live.running.load()) {
        Sleep(10);
        if (keys.pressed(VK_ESCAPE) || g_exit.load()) g_live.stopRequest = true;
        const double t = g_live.elapsedS.load();
        if (phase + 1 < kN && t >= kTouchPhases[phase + 1].startS) {
            close(acc[phase]);
            ++phase;
            open(acc[phase]);
            std::printf("  [%2.0f с] %s: %s\n", t, kTouchPhases[phase].label, kTouchPhases[phase].instruction);
            std::fflush(stdout);
            if (kTouchPhases[phase].beepHz) Beep(kTouchPhases[phase].beepHz, 150);
        }
        if (t >= nextPrint) {
            nextPrint += 1.0;
            const int64_t now = Clock::now();
            const uint64_t hid = g_hook.count();
            const int64_t sent = g_live.sentAbs.load(), recv = receivedPathAbs();
            const double dtUs = Clock::toUs(now - lastT);
            std::printf("      %4.0f с | HID %4.0f Гц | путь x%.2f | M:%s\n", t, (hid - lastHid) * 1e6 / dtUs,
                sent > lastSent ? static_cast<double>(recv - lastRecv) / static_cast<double>(sent - lastSent) : 0.0,
                link.mouseUf().c_str());
            std::fflush(stdout);
            lastHid = hid;
            lastSent = sent;
            lastRecv = recv;
            lastT = now;
        }
    }
    worker.join();
    Sleep(50);  // хвост Raw Input
    close(acc[phase]);

    std::printf("\n  %s |   с     | HID ср | 1 кадр %% | 2 кадра %% | >2.5 мс %% | %s | M в конце\n",
        padUtf8("фаза", 16).c_str(), padUtf8("путь пришло/отпр.", 21).c_str());
    for (size_t i = 0; i <= phase; ++i) {
        const Acc& a = acc[i];
        const double dt = std::max(1e-3, a.t1 - a.t0);
        uint64_t total = 0;
        std::array<uint64_t, 6> d{};
        for (size_t b = 0; b < d.size(); ++b) {
            d[b] = a.h1[b] - a.h0[b];
            total += d[b];
        }
        const double tot = static_cast<double>(std::max<uint64_t>(1, total));
        const double path = a.sent1 > a.sent0
            ? static_cast<double>(a.recv1 - a.recv0) / static_cast<double>(a.sent1 - a.sent0) : 0.0;
        std::printf("  %s | %3.0f-%-3.0f | %6.0f | %8.1f | %9.1f | %9.1f | %5.2f %s | %s\n",
            padUtf8(kTouchPhases[i].label, 16).c_str(), a.t0, a.t1, (a.hid1 - a.hid0) / dt, 100.0 * d[1] / tot,
            100.0 * d[3] / tot, 100.0 * (d[4] + d[5]) / tot, path,
            padUtf8(kTouchPhases[i].handsOff ? "(чистый инжект)" : "(с рукой)", 15).c_str(), a.uf.c_str());
    }
    if (const std::string extra = tx.extraStats(); !extra.empty()) {
        std::printf("  %s\n", extra.c_str());
    }
    std::printf("  В фазах без руки путь 1.00 = траектория из кода дошла точно; HID там - темп прошивки после касания.\n");
}

void printBytes(const std::string& label, const std::vector<uint8_t>& rx) {
    std::printf("  %-28s -> ", label.c_str());
    if (rx.empty()) {
        std::printf("(тишина)\n");
        return;
    }
    for (uint8_t b : rx) std::printf("%02X ", b);
    std::printf("| \"");
    for (uint8_t b : rx) std::printf("%c", (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.');
    std::printf("\"\n");
}

// Снимок состояния платы: только запросы и нулевые движения, курсор стоит.
void runProbe(RawComTransport& raw) {
    using namespace std::chrono_literals;
    std::printf("\n>>> PROBE (курсор не двигается)\n");
    auto bin = [&](const std::string& label, const std::vector<uint8_t>& frame) {
        printBytes(label, raw.transact(frame.data(), frame.size(), 150ms));
    };
    auto text = [&](const std::string& cmd) {
        const std::string line = cmd + "\r\n";
        printBytes(cmd, raw.transact(reinterpret_cast<const uint8_t*>(line.data()), line.size(), 150ms));
    };
    bin("MAK 0x04 FIRMWARE_VERSION", {0xDE, 0xAD, 0x00, 0x00, 0x04});
    bin("MAK 0x1F interpolation", {0xDE, 0xAD, 0x00, 0x00, 0x1F});
    bin("MAK 0x10 BUTTONS stream", {0xDE, 0xAD, 0x00, 0x00, 0x10});
    text("km.version()");
    text("km.device()");
    text("km.echo()");
    text("km.buttons()");
    text("km.moving()");
    text("km.move(0,0)");
    text("km.move_now(0,0)");
    const std::string payload = "m.move_now(0,0)";
    std::vector<uint8_t> framed{0xDE, 0xAD, static_cast<uint8_t>(payload.size()), 0x00, 'k'};
    framed.insert(framed.end(), payload.begin(), payload.end());
    bin("framed 'k' m.move_now(0,0)", framed);
    std::printf("  0x1F: 00 = интерполяция 0; FF = AUTO(255) или отказ (по байтам неразличимо).\n"
                "  Мутации: тишина = принята (echo выкл), ERR = отклонена.\n");
}

// Самопроверка: MOVE(0,0) с заданной частотой - курсор стоит на месте, а мы
// видим, успевает ли транспорт и поток тайминга.
void transportSelfTest(Transport& tx, double hz, double seconds) {
    tuneTimingThread(-1);
    tx.resetStats();
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
    std::printf("   [F12] замер руки 15 с без инжекта: HID + таблица скорость -> частота (эталон для F4/F10)\n");
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

    initAbcurves(opt);

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

    if (opt.pollDevice) {
        g_devicePoll = [&link] { return link.mouseUf(); };
    }
    if (opt.startDelayS > 0.0) {
        std::printf("[*] Ожидание %.0f с перед стартом (мышь не трогать)...\n", opt.startDelayS);
        std::fflush(stdout);
        Sleep(static_cast<DWORD>(opt.startDelayS * 1000.0));
    }
    if (opt.dumpSettings) {
        link.dumpSettings();
    } else if (opt.probe) {
        if (link.acquire(TxKind::Async)) {
            runProbe(*link.raw());
        }
    } else if (!opt.compare.empty()) {
        runCompare(opt, link);
    } else if (opt.touchTest) {
        if (Transport* tx = link.acquire(opt.tx)) {
            runTouchTest(opt, *tx, link);
        }
    } else if (opt.check) {
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
            RunConfig cfg =
                makeConfig(opt, opt.hz, opt.burst, opt.pattern, opt.durationS, label, opt.pauseEveryS, opt.pauseMs);
            if (opt.substep && !opt.abc) {
                cfg = withSubstep(cfg);
                cfg.label += " | накопитель";
                runOnce(cfg, *tx, monitor, 0);
            } else if (opt.abc) {
                if (!g_abcReady) {
                    std::printf("[-] --abc: ABCurves Renderer не загружен\n");
                } else {
                    cfg = withRenderer(cfg);
                    cfg.label += " | ABCurves";
                    runOnce(cfg, *tx, monitor, 0);
                }
            } else {
                runOnce(cfg, *tx, monitor, 0);
            }
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
                        : pattern == Pattern::Line   ? Pattern::Flicks
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
                Transport* tx = link.acquire(opt.tx);
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
                const std::string label = std::string(p.key) + " " + p.title + " | " + tx->name();
                if (p.abc && !g_abcReady) {
                    std::printf("[-] ABCurves Renderer не загружен - пресет недоступен\n");
                    break;
                }
                RunConfig cfg = makeConfig(opt, p.hz, 0, p.pattern.value_or(pattern),
                    p.durationS.value_or(opt.durationS), label, p.pauseEveryS, p.pauseMs);
                if (p.abc) cfg = withRenderer(cfg);
                if (p.substep) cfg = withSubstep(cfg);
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
    g_devicePoll = nullptr;
    g_hook.stop();
    monitor.stop();
    g_accel.restore();
    link.shutdown();
    restoreProcessTiming();
    return 0;
}
