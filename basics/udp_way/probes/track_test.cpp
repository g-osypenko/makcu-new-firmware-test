// Отслеживание физической мыши через MAKCU по UDP: замеры (стенд, не эталон).
//
//   track_test.exe [--host 192.168.50.175] [--screen W H] MODE
//
// Режимы и окно - basics/track_bench.h (общие с COM-стендом comport_way/track_test_com.cpp).
//
// Сборка: g++ -std=c++17 -O2 -Wall -Wextra -I ..\perfect-transport-udp track_test.cpp
//         -lws2_32 -lwinmm -lgdi32 -static -o track_test.exe
#include "makcu_udp_batch_transport.h"

#include "../../track_bench.h"

namespace {

using namespace bench;

// ---- плата: свой сокет, одна команда в полёте ------------------------------------------
class Board {
public:
    ~Board() { close(); }
    bool open(const std::string& host, uint16_t port, std::string& err) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            err = "WSAStartup";
            return false;
        }
        s_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_ == INVALID_SOCKET) {
            err = "socket";
            return false;
        }
        sockaddr_in any{};
        any.sin_family = AF_INET;
        bind(s_, reinterpret_cast<sockaddr*>(&any), sizeof(any));
        DWORD off = FALSE, ret = 0;
        WSAIoctl(s_, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
        u_long nb = 1;
        ioctlsocket(s_, FIONBIO, &nb);
        to_.sin_family = AF_INET;
        to_.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &to_.sin_addr) != 1) {
            err = "плохой адрес " + host;
            return false;
        }
        seq_ = static_cast<uint64_t>(Q::now()) << 8;
        return true;
    }
    void close() {
        if (s_ != INVALID_SOCKET) {
            closesocket(s_);
            s_ = INVALID_SOCKET;
            WSACleanup();
        }
    }
    // Команда с ответом: payload ответа или nullopt (нет ответа за timeoutUs).
    std::optional<std::vector<uint8_t>> ask(uint8_t op, const uint8_t* pl = nullptr, uint16_t len = 0,
        double timeoutUs = 15000.0, int64_t* tSend = nullptr, int64_t* tRecv = nullptr) {
        const uint64_t seq = ++seq_;
        const int64_t ts = send(seq, op, pl, len);
        if (tSend) *tSend = ts;
        const int64_t end = ts + Q::fromUs(timeoutUs);
        uint8_t buf[2048];
        for (;;) {
            const int n = recv(s_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n <= 0) {
                if (Q::now() >= end) {
                    ++lost_;
                    return std::nullopt;
                }
                YieldProcessor();
                continue;
            }
            if (n < 14 || buf[9] != 0xDE || buf[10] != 0xAD) continue;
            if (buf[13] == 0x53) continue;  // событие кнопок: несёт чужой nonce
            uint64_t got = 0;
            std::memcpy(&got, buf + 1, 8);
            if (got != seq || buf[13] != op) continue;
            if (tRecv) *tRecv = Q::now();
            const uint16_t plen = static_cast<uint16_t>(buf[11] | buf[12] << 8);
            if (14 + plen > n) continue;
            return std::vector<uint8_t>(buf + 14, buf + 14 + plen);
        }
    }
    // Команда без ответа; следующая команда - не раньше чем через kGapUs.
    int64_t noReply(uint8_t op, const uint8_t* pl, uint16_t len) {
        const int64_t ts = send(++seq_, op, pl, len);
        spinUntilRaw(ts + Q::fromUs(kGapUs));
        return ts;
    }
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
        }
        const auto l = locks();
        return l && *l == 0;
    }
    uint64_t lost() const { return lost_; }

    // Серия 4 x 0x60: +X, -X, +Y, -Y по одной.
    void lockSeries(uint8_t s, int64_t& first, int64_t& last) {
        for (const uint8_t t : {6, 7, 9, 10}) {
            const uint8_t pl[2] = {t, s};
            last = noReply(0x60, pl, 2);
            if (!first) first = last;
        }
    }

    static constexpr double kGapUs = 2000.0;
    static constexpr double kLossGapUs[3] = {2000.0, 3000.0, 4000.0};  // --lockloss: паузы серии 0x60

private:
    int64_t send(uint64_t seq, uint8_t op, const uint8_t* pl, uint16_t len) {
        uint8_t buf[300];
        buf[0] = 0x55;
        std::memcpy(buf + 1, &seq, 8);
        buf[9] = 0xDE;
        buf[10] = 0xAD;
        buf[11] = static_cast<uint8_t>(len);
        buf[12] = static_cast<uint8_t>(len >> 8);
        buf[13] = op;
        if (len) std::memcpy(buf + 14, pl, len);
        const int64_t ts = Q::now();
        sendto(s_, reinterpret_cast<const char*>(buf), 14 + len, 0, reinterpret_cast<const sockaddr*>(&to_), sizeof(to_));
        return ts;
    }
    static void spinUntilRaw(int64_t t) {
        while (Q::now() < t) YieldProcessor();
    }
    SOCKET s_ = INVALID_SOCKET;
    sockaddr_in to_{};
    uint64_t seq_ = 0;
    uint64_t lost_ = 0;
};

std::string g_host = "192.168.50.175";
uint16_t g_port = 8080;

struct Udp {
    using Tx = makcu::UdpBatchTransport;
    using Board = ::Board;
    static bool open(Tx& tx, std::string& err) { return tx.open(g_host, g_port, err); }
    static bool openBoard(Board& b, std::string& err) { return b.open(g_host, g_port, err); }
    static uint32_t depth(const Tx::Stats& s) { return s.maxBoardQueued; }
    static uint64_t merged(const Tx::Stats& s) { return s.merged; }
    static uint64_t retries(const Tx::Stats&) { return 0; }
    static uint64_t records(const Tx::Stats& s) { return s.records; }
};

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Args a;
    auto usage = [] {
        std::printf("track_test.exe [--host IP] --accuracy S | --lock N | --lockloss S | --inject N | --reflock N | --track S | --trackinject N\n");
        return 2;
    };
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) g_host = argv[++i];
        else if (!parseArg(a, i, argc, argv)) return usage();
    }
    if (!a.any()) return usage();
    return benchMain<Udp>(a);
}
