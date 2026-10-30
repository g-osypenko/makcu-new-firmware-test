// Отслеживание физической мыши и инжект через MAKCU по COM: замеры (стенд, не эталон).
//
//   track_test_com.exe [--port COM5] [--screen W H] MODE
//     --state      показать, в каком состоянии плата (km.screen, блокировки, инжект, поток
//                  событий), окна нет, курсор не двигается
//
// Режимы и окно - basics/track_bench.h (общие с UDP-стендом udp_way/probes/track_test.cpp).
// COM-порт эксклюзивный: Toolkit, SDK mak-suite и другие тесты должны быть закрыты.
//
// Сборка: g++ -std=c++17 -O2 -Wall -Wextra -I perfect-transport-comport track_test_com.cpp
//         -lsetupapi -lwinmm -lgdi32 -static -o track_test_com.exe
#include "makcu_raw_transport.h"

#include "../track_bench.h"

namespace {

using namespace bench;

// ---- плата: свой порт, команда - ответ -----------------------------------------------------
// Голый клиент: без кольца и потоков, синхронный хэндл, чтение сразу возвращает, что есть.
class Board {
public:
    ~Board() { close(); }
    bool open(const std::string& name, std::string& err) {
        const std::string path = "\\\\.\\" + name;
        h_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            err = "CreateFile(" + name + "): ошибка " + std::to_string(GetLastError()) + " (порт занят?)";
            return false;
        }
        SetupComm(h_, 4096, 4096);
        COMMTIMEOUTS t{};
        t.ReadIntervalTimeout = MAXDWORD;  // чтение возвращается сразу
        t.WriteTotalTimeoutConstant = 200;
        SetCommTimeouts(h_, &t);
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(h_, &dcb)) {
            err = "GetCommState";
            return false;
        }
        dcb.BaudRate = 4000000;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;  // иначе можно перезапустить ESP32
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fErrorChar = FALSE;
        dcb.fNull = FALSE;
        dcb.fAbortOnError = FALSE;
        if (!SetCommState(h_, &dcb)) {
            err = "SetCommState";
            return false;
        }
        PurgeComm(h_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        return true;
    }
    void close() {
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }
    // Команда с ответом: payload ответа или nullopt (нет ответа за timeoutUs).
    std::optional<std::vector<uint8_t>> ask(uint8_t op, const uint8_t* pl = nullptr, uint16_t len = 0,
        double timeoutUs = 15000.0, int64_t* tSend = nullptr, int64_t* tRecv = nullptr) {
        const int64_t ts = send(op, pl, len);
        if (tSend) *tSend = ts;
        const int64_t end = ts + Q::fromUs(timeoutUs);
        std::vector<uint8_t> out;
        for (;;) {
            uint8_t got = 0;
            while (nextFrame(got, out)) {
                if (got != op) continue;  // событие 0x53 и т.п.
                if (tRecv) *tRecv = Q::now();
                return out;
            }
            if (!pump() && Q::now() >= end) {
                ++lost_;
                return std::nullopt;
            }
        }
    }
    // Команда без ответа.
    int64_t noReply(uint8_t op, const uint8_t* pl, uint16_t len) { return send(op, pl, len); }
    bool pos(int& x, int& y, int64_t* ts = nullptr, int64_t* tr = nullptr) {
        auto r = ask(0x63, nullptr, 0, 15000.0, ts, tr);
        if (!r || r->size() < 4) return false;
        x = (*r)[0] | (*r)[1] << 8;
        y = (*r)[2] | (*r)[3] << 8;
        return true;
    }
    std::optional<std::pair<int, int>> screen() {
        auto r = ask(0x64);
        if (!r || r->size() < 4) return std::nullopt;
        return std::make_pair((*r)[0] | (*r)[1] << 8, (*r)[2] | (*r)[3] << 8);
    }
    void setScreen(int w, int h) {
        const uint8_t pl[4] = {static_cast<uint8_t>(w), static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(h),
            static_cast<uint8_t>(h >> 8)};
        noReply(0x64, pl, 4);
        Sleep(2);
    }
    std::optional<uint16_t> locks(int64_t* tr = nullptr) {
        auto r = ask(0x56, nullptr, 0, 15000.0, nullptr, tr);
        if (!r || r->size() < 3) return std::nullopt;
        return static_cast<uint16_t>((*r)[1] | (*r)[2] << 8);
    }
    // Снять все блокировки мыши и проверить снимком (до 20 попыток).
    bool unlockAll() {
        for (int attempt = 0; attempt < 20; ++attempt) {
            const auto l = locks();
            if (l && *l == 0) return true;
            const uint8_t mm[4] = {0, 0, 0, 0}, wm[2] = {0, 0};
            noReply(0x16, mm, 4);
            noReply(0x17, wm, 2);
            if (l) {
                for (uint8_t t = 0; t < 5; ++t) {
                    if (*l & (1u << t)) {
                        const uint8_t pl[2] = {t, 0};
                        noReply(0x60, pl, 2);
                    }
                }
            }
            Sleep(1);
        }
        const auto l = locks();
        return l && *l == 0;
    }
    // Серия 4 x 0x60 одной записью в порт - так её слал эталон ревизии 3.
    void lockSeries(uint8_t s, int64_t& first, int64_t& last) {
        uint8_t buf[4 * 7];
        size_t n = 0;
        for (const uint8_t t : {6, 7, 9, 10}) {
            const uint8_t f[7] = {0xDE, 0xAD, 0x02, 0x00, 0x60, t, s};
            std::memcpy(buf + n, f, sizeof(f));
            n += sizeof(f);
        }
        first = last = write(buf, n);
    }
    uint64_t lost() const { return lost_; }

    static constexpr double kLossGapUs[3] = {0.0, 1000.0, 3000.0};  // --lockloss: 0 - одной записью

private:
    int64_t send(uint8_t op, const uint8_t* pl, uint16_t len) {
        uint8_t buf[300];
        buf[0] = 0xDE;
        buf[1] = 0xAD;
        buf[2] = static_cast<uint8_t>(len);
        buf[3] = static_cast<uint8_t>(len >> 8);
        buf[4] = op;
        if (len) std::memcpy(buf + 5, pl, len);
        return write(buf, 5u + len);
    }
    int64_t write(const uint8_t* d, size_t n) {
        const int64_t ts = Q::now();
        DWORD done = 0;
        WriteFile(h_, d, static_cast<DWORD>(n), &done, nullptr);
        return ts;
    }
    bool pump() {
        uint8_t buf[512];
        DWORD n = 0;
        if (!ReadFile(h_, buf, sizeof(buf), &n, nullptr) || n == 0) return false;
        rx_.insert(rx_.end(), buf, buf + n);
        return true;
    }
    bool nextFrame(uint8_t& op, std::vector<uint8_t>& payload) {
        size_t i = 0;
        for (; i + 5 <= rx_.size(); ++i) {
            if (rx_[i] != 0xDE || rx_[i + 1] != 0xAD) continue;
            const size_t plen = static_cast<size_t>(rx_[i + 2]) | static_cast<size_t>(rx_[i + 3]) << 8;
            if (plen > 512) continue;
            if (i + 5 + plen > rx_.size()) break;
            op = rx_[i + 4];
            payload.assign(rx_.begin() + static_cast<std::ptrdiff_t>(i + 5), rx_.begin() + static_cast<std::ptrdiff_t>(i + 5 + plen));
            rx_.erase(rx_.begin(), rx_.begin() + static_cast<std::ptrdiff_t>(i + 5 + plen));
            return true;
        }
        rx_.erase(rx_.begin(), rx_.begin() + static_cast<std::ptrdiff_t>(i));
        return false;
    }
    HANDLE h_ = INVALID_HANDLE_VALUE;
    std::vector<uint8_t> rx_;
    uint64_t lost_ = 0;
};

std::string g_port = "COM5";

// Сборка с -DBENCH_REV3 и заголовком ревизии 3: замер "до правок" (API ревизии 5 - заглушки).
#ifdef BENCH_REV3
struct Rev3Tx : makcu::RawTransport {
    struct Physical {
        int64_t x = 0, y = 0;
        uint64_t samples = 0;
        int64_t sampleQpc = 0;
        bool live = false;
    };
    void setMaxLag(uint32_t) {}
    uint32_t maxLag() const { return 0; }
    uint32_t stopMovement(int32_t* = nullptr, int32_t* = nullptr) { return 0; }
    bool setTracking(bool) { return false; }
    Physical physical() const { return {}; }
};
#endif

struct Com {
#ifdef BENCH_REV3
    using Tx = Rev3Tx;
#else
    using Tx = makcu::RawTransport;
#endif
    using Board = ::Board;
    static bool open(Tx& tx, std::string& err) { return tx.open(g_port, err); }
    static bool openBoard(Board& b, std::string& err) { return b.open(g_port, err); }
    static uint32_t depth(const Tx::Stats& s) { return s.maxLive; }
#ifdef BENCH_REV3
    static uint64_t merged(const Tx::Stats&) { return 0; }
#else
    static uint64_t merged(const Tx::Stats& s) { return s.merged; }
#endif
    static uint64_t retries(const Tx::Stats& s) { return s.ackRetry; }
    static uint64_t records(const Tx::Stats& s) { return s.submitted; }
};

// Что осталось на плате после опытов.
int printState() {
    Board b;
    std::string err;
    if (!b.open(g_port, err)) {
        std::printf("[-] %s\n", err.c_str());
        return 1;
    }
    const auto scr = b.screen();
    const auto snap = b.ask(0x56);
    const uint8_t kind[1] = {0x01};
    const auto stream = b.ask(0x52, kind, 1);
    if (!scr || !snap || snap->size() < 3) {
        std::printf("[-] плата не ответила\n");
        return 1;
    }
    std::printf("km.screen %dx%d | инжект %02X | блокировки %04X | поток событий мыши %s\n", scr->first, scr->second,
        (*snap)[0] & 0x1F, static_cast<unsigned>((*snap)[1] | (*snap)[2] << 8),
        stream && !stream->empty() ? ((*stream)[0] ? "вкл" : "выкл") : "?");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    Args a;
    auto usage = [] {
        std::printf("track_test_com.exe [--port COM5] --accuracy S | --lock N | --lockloss S | --inject N | --reflock N | --track S | --trackinject N\n");
        return 2;
    };
    bool state = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) g_port = argv[++i];
        else if (std::strcmp(argv[i], "--state") == 0) state = true;
        else if (!parseArg(a, i, argc, argv)) return usage();
    }
    if (state) return printState();
    if (!a.any()) return usage();
    return benchMain<Com>(a);
}
