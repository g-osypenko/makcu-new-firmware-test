// Отслеживание физической мыши через MAKCU по UDP: замеры (стенд, не эталон).
//
//   track_test.exe [--host 192.168.50.175] [--screen W H] MODE
//     --screen W H   km.screen на время замера (accuracy/lock: по умолчанию 32767x32767,
//                    lockloss: не трогать); после замера возвращается прежний
//     --accuracy S   S секунд опрос GET 0x63 GETPOS в замкнутом цикле (одна команда в полёте)
//                    против Raw Input тестового окна. Водите мышью как угодно, последние 2 с -
//                    не трогать. На время замера km.screen = 32767x32767 (позиция в центре,
//                    запас +-16k отсчётов), потом размер возвращается.
//     --lock N       N циклов: 1 с свободно (опрос 0x63), блокировка X и Y, 400 мс держать,
//                    снять. Через раз: 4 x SET 0x60 с паузой 2 мс (так делает эталон) и одна
//                    SET 0x16 MOVE_MASK. Всё время водите мышью кругами. Задержка = от первой
//                    команды блокировки до последнего физического отсчёта в Windows.
//     --lockloss S   S секунд попыток блокировки X+Y (4 x 0x60 с паузой 2/3/4 мс и одна 0x16) со
//                    снимком 0x56: сколько команд дошло, отдельно - когда рука двигала мышь и когда
//                    нет. Водите мышью кругами с остановками. --prepoll MS: перед каждой попыткой
//                    MS мс опроса 0x63, команда блокировки - сразу после ответа (как в жизни).
//     --inject N     N раз: 300 мс круг 1 кГц через эталон (batch 0x6C), стоп. Хвост = от
//                    последнего принятого move() до последнего отчёта в Windows. Мышь не трогать.
//     --reflock N    N раз блокировка через эталон, 400 мс, снять; через раз tx.setLock(X) +
//                    tx.setLock(Y) и tx.setLocks({X, Y}).
//                    Задержка = от вызова setLock() до последнего физического отсчёта в Windows.
//                    Всё время водите мышью кругами.
//
// Тестовое окно должно быть в фокусе: Raw Input фонового окна Windows 11 прореживает.
// Курсор заперт в окне. Esc - выход; блокировки снимаются и проверяются снимком 0x56.
//
// Сборка: g++ -std=c++17 -O2 -Wall -Wextra -I ..\perfect-transport-udp track_test.cpp
//         -lws2_32 -lwinmm -lgdi32 -static -o track_test.exe
#include "makcu_udp_batch_transport.h"

#include <timeapi.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using Q = makcu::detail::Qpc;

std::atomic<bool> g_stop{false};
std::atomic<bool> g_cleanDone{false};
int g_scrW = 0, g_scrH = 0;  // --screen: km.screen на время замера (0 - по умолчанию режима)
double g_prePollMs = 0.0;    // --prepoll: lockloss опрашивает 0x63 столько мс перед каждой попыткой

BOOL WINAPI onCtrl(DWORD type) {
    g_stop = true;
    if (type == CTRL_CLOSE_EVENT) {  // консоль закрывают: дождаться снятия блокировок
        for (int i = 0; i < 300 && !g_cleanDone.load(); ++i) Sleep(10);
    }
    return TRUE;
}

// ---- Raw Input: каждое сообщение с меткой QPC ------------------------------------------
struct RawEv {
    int64_t t;
    int32_t dx, dy;
    HANDLE dev;
};
constexpr size_t kRawCap = size_t{1} << 20;  // ~17 мин при 1 кГц
std::vector<RawEv> g_raw(kRawCap);
std::atomic<size_t> g_rawN{0};

struct Ui {
    HWND hwnd = nullptr;
    RECT clip{};
    std::atomic<bool> ready{false};
    std::atomic<const wchar_t*> big{L""};
    std::atomic<const wchar_t*> small{L""};
    std::atomic<double> rate{0.0};
    std::atomic<uint32_t> polls{0};
};
Ui g_ui;
HFONT g_bigFont = nullptr;

bool foreground() { return GetForegroundWindow() == g_ui.hwnd; }

void drawStatus(HDC dc, HWND h) {
    RECT r;
    GetClientRect(h, &r);
    FillRect(dc, &r, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    wchar_t line[256];
    swprintf(line, 256, L"Raw Input: %llu сообщ., %.0f /с | опросов 0x63: %u | окно %ls   (Esc - выход)",
        static_cast<unsigned long long>(g_rawN.load()), g_ui.rate.load(), g_ui.polls.load(),
        foreground() ? L"в фокусе" : L"НЕ В ФОКУСЕ - кликните в окно");
    TextOutW(dc, 10, 8, line, static_cast<int>(wcslen(line)));
    const wchar_t* s = g_ui.small.load();
    TextOutW(dc, 10, 30, s, static_cast<int>(wcslen(s)));
    const wchar_t* b = g_ui.big.load();
    HGDIOBJ old = SelectObject(dc, g_bigFont);
    const COLORREF oc = SetTextColor(dc, RGB(200, 0, 0));
    RECT br{0, 70, r.right, r.bottom};
    DrawTextW(dc, b, -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, oc);
    SelectObject(dc, old);
}

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_INPUT: {
        RAWINPUT raw{};
        UINT size = sizeof(raw);
        const int64_t now = Q::now();
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) !=
                static_cast<UINT>(-1) &&
            raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            const size_t n = g_rawN.load(std::memory_order_relaxed);
            if (n < kRawCap) {
                g_raw[n] = RawEv{now, raw.data.mouse.lLastX, raw.data.mouse.lLastY, raw.header.hDevice};
                g_rawN.store(n + 1, std::memory_order_release);
            }
        }
        return DefWindowProcW(h, m, wp, lp);
    }
    case WM_TIMER: {
        if (foreground()) ClipCursor(&g_ui.clip);  // Windows снимает ограничение при смене активного окна
        static size_t prevN = 0;
        static int64_t prevT = 0;
        const int64_t now = Q::now();
        const size_t n = g_rawN.load();
        if (prevT) g_ui.rate = static_cast<double>(n - prevN) * 1e6 / Q::toUs(now - prevT);
        prevN = n;
        prevT = now;
        HDC dc = GetDC(h);
        drawStatus(dc, h);
        ReleaseDC(h, dc);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        drawStatus(dc, h);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONUP:
    case WM_MBUTTONUP: case WM_CONTEXTMENU:
        return 0;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_APPCOMMAND:
        return TRUE;
    case WM_CLOSE:
        g_stop = true;
        return 0;
    default:
        return DefWindowProcW(h, m, wp, lp);
    }
}

void uiThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MakcuTrackTest";
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    g_bigFont = CreateFontW(44, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
    const int w = 900, h = 420;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_ui.hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"MAKCU track test (Esc - выход)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    RECT client;
    GetClientRect(g_ui.hwnd, &client);
    POINT tl{client.left + 20, client.top + 60}, br{client.right - 20, client.bottom - 20};
    ClientToScreen(g_ui.hwnd, &tl);
    ClientToScreen(g_ui.hwnd, &br);
    g_ui.clip = RECT{tl.x, tl.y, br.x, br.y};
    SetForegroundWindow(g_ui.hwnd);  // из фонового процесса может не сработать: тест ждёт клика в окно
    if (foreground()) {
        SetCursorPos((tl.x + br.x) / 2, (tl.y + br.y) / 2);
        ClipCursor(&g_ui.clip);
    }
    SetTimer(g_ui.hwnd, 1, 100, nullptr);
    RAWINPUTDEVICE rid{0x01, 0x02, RIDEV_INPUTSINK, g_ui.hwnd};
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    g_ui.ready = true;
    MSG msg;
    while (!g_stop.load()) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    }
    ClipCursor(nullptr);
    DestroyWindow(g_ui.hwnd);
}

void say(const wchar_t* big, const wchar_t* small = L"") {
    g_ui.big = big;
    g_ui.small = small;
    if (g_ui.hwnd) InvalidateRect(g_ui.hwnd, nullptr, FALSE);
}

void checkEsc() {
    static int64_t next = 0;
    const int64_t now = Q::now();
    if (now < next) return;
    next = now + Q::fromUs(20000.0);
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) g_stop = true;
}

void spinUntil(int64_t t) {
    while (Q::now() < t && !g_stop.load()) {
        checkEsc();
        YieldProcessor();
    }
}

void sleepMs(double ms) { spinUntil(Q::now() + Q::fromUs(ms * 1000.0)); }

bool waitForeground(double seconds) {
    const int64_t end = Q::now() + Q::fromUs(seconds * 1e6);
    while (!foreground() && Q::now() < end && !g_stop.load()) Sleep(20);
    return foreground();
}

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

    static constexpr double kGapUs = 2000.0;

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

// ---- разбор: Raw Input главного устройства и опросы позиции -----------------------------
struct Poll {
    int64_t ts = 0, tr = 0;
    int x = 0, y = 0;
    int phase = 0;  // 0 свободно, 1 под блокировкой
    int cycle = -1;
};

struct Track {
    HANDLE dev = nullptr;
    std::vector<int64_t> t;
    std::vector<int64_t> cx{0}, cy{0};  // сумма первых k сообщений
    std::vector<int32_t> dx, dy;
    size_t devices = 0;
    // Сколько сообщений пришло не позже at.
    size_t upto(int64_t at) const { return static_cast<size_t>(std::upper_bound(t.begin(), t.end(), at) - t.begin()); }
};

// Главное устройство = больше всего сообщений в окне [from, to].
Track buildTrack(int64_t from, int64_t to) {
    const size_t n = g_rawN.load(std::memory_order_acquire);
    std::map<HANDLE, size_t> count;
    for (size_t i = 0; i < n; ++i) {
        if (g_raw[i].t >= from && g_raw[i].t <= to) ++count[g_raw[i].dev];
    }
    Track tr;
    tr.devices = count.size();
    size_t best = 0;
    for (auto& [d, c] : count) {
        if (c > best) {
            best = c;
            tr.dev = d;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        const RawEv& e = g_raw[i];
        if (e.dev != tr.dev || e.t < from || e.t > to) continue;
        tr.t.push_back(e.t);
        tr.dx.push_back(e.dx);
        tr.dy.push_back(e.dy);
        tr.cx.push_back(tr.cx.back() + e.dx);
        tr.cy.push_back(tr.cy.back() + e.dy);
    }
    return tr;
}

double pct(size_t a, size_t b) { return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; }

// Доля интервалов ~1 мс между сообщениями при движении: прорежен ли Raw Input.
void printRawRate(const Track& tr) {
    size_t moving = 0, oneMs = 0;
    for (size_t i = 1; i < tr.t.size(); ++i) {
        const double us = Q::toUs(tr.t[i] - tr.t[i - 1]);
        if (us > 20000.0) continue;
        ++moving;
        if (us >= 500.0 && us < 1500.0) ++oneMs;
    }
    std::printf("Raw Input: %zu сообщений от главного устройства (устройств %zu), интервалов ~1 мс %.1f %%%s\n",
        tr.t.size(), tr.devices, pct(oneMs, moving),
        pct(oneMs, moving) < 60.0 ? "  <-- прорежен? окно было не в фокусе" : "");
}

struct Fit {
    double lagUs = 0.0;
    int64_t offX = 0, offY = 0;
    size_t used = 0, exact = 0, near1 = 0;  // точно / с точностью до соседнего отчёта
    int64_t maxErr = 0;
    std::vector<int64_t> errs;  // по модулю, для полезных опросов
};

// Позиция платы P_i против суммы Raw Input до момента ts_i + lag. Разность D_i = P_i - R должна
// быть постоянной; lag ищется по максимуму совпадений. phase < 0 - все опросы.
Fit fit(const std::vector<Poll>& polls, const Track& tr, int phase) {
    Fit best;
    size_t bestHits = 0;
    bool first = true;
    for (double lag = -3000.0; lag <= 6000.0; lag += 50.0) {
        std::map<std::pair<int64_t, int64_t>, size_t> hist;
        for (const Poll& p : polls) {
            if (phase >= 0 && p.phase != phase) continue;
            const size_t k = tr.upto(p.ts + Q::fromUs(lag));
            ++hist[{p.x - tr.cx[k], p.y - tr.cy[k]}];
        }
        for (auto& [d, c] : hist) {
            if (first || c > bestHits) {
                first = false;
                bestHits = c;
                best.lagUs = lag;
                best.offX = d.first;
                best.offY = d.second;
            }
        }
    }
    for (const Poll& p : polls) {
        if (phase >= 0 && p.phase != phase) continue;
        ++best.used;
        const size_t k = tr.upto(p.ts + Q::fromUs(best.lagUs));
        int64_t err = INT64_MAX;
        for (int dk = -1; dk <= 1; ++dk) {
            const int64_t kk = static_cast<int64_t>(k) + dk;
            if (kk < 0 || kk >= static_cast<int64_t>(tr.cx.size())) continue;
            const int64_t ex = p.x - tr.cx[kk] - best.offX, ey = p.y - tr.cy[kk] - best.offY;
            const int64_t e = std::max(std::llabs(ex), std::llabs(ey));
            if (dk == 0 && e == 0) ++best.exact;
            err = std::min(err, e);
        }
        if (err == 0) ++best.near1;
        best.maxErr = std::max(best.maxErr, err);
        best.errs.push_back(err);
    }
    return best;
}

int64_t quantile(std::vector<int64_t> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size())))];
}

void printFit(const char* label, const Fit& f) {
    std::printf("%s: опросов %zu, совпало точно %.2f %%, с точностью до соседнего отчёта %.2f %% | "
                "ошибка (отсчёты) p99 %lld, max %lld | сдвиг времени %+.2f мс\n",
        label, f.used, pct(f.exact, f.used), pct(f.near1, f.used), static_cast<long long>(quantile(f.errs, 0.99)),
        static_cast<long long>(f.maxErr), f.lagUs / 1000.0);
}

// ---- --accuracy ------------------------------------------------------------------------
int runAccuracy(Board& b, double seconds) {
    const auto scr = b.screen();
    if (!scr) {
        std::printf("[-] нет ответа на GET 0x64\n");
        return 1;
    }
    b.setScreen(g_scrW ? g_scrW : 32767, g_scrH ? g_scrH : 32767);
    int cx = 0, cy = 0;
    b.pos(cx, cy);
    std::printf("km.screen был %dx%d, на время замера %dx%d, позиция %d,%d\n", scr->first, scr->second,
        g_scrW ? g_scrW : 32767, g_scrH ? g_scrH : 32767, cx, cy);
    say(L"ВОДИТЕ МЫШЬЮ", L"как угодно: медленно, быстро, рывки; за 2 с до конца - стоп");
    std::vector<Poll> polls;
    polls.reserve(static_cast<size_t>(seconds * 700.0) + 100);
    const int64_t t0 = Q::now(), tEnd = t0 + Q::fromUs(seconds * 1e6);
    bool stopShown = false;
    size_t clamped = 0;
    while (Q::now() < tEnd && !g_stop.load()) {
        checkEsc();
        if (!stopShown && Q::now() > tEnd - Q::fromUs(2e6)) {
            say(L"СТОП - НЕ ТРОГАЙТЕ МЫШЬ");
            stopShown = true;
        }
        Poll p;
        if (b.pos(p.x, p.y, &p.ts, &p.tr)) {
            if (p.x == 0 || p.y == 0 || p.x >= (g_scrW ? g_scrW : 32767) - 1 || p.y >= (g_scrH ? g_scrH : 32767) - 1) {
                ++clamped;
            }
            polls.push_back(p);
            g_ui.polls = static_cast<uint32_t>(polls.size());
        } else {
            sleepMs(2.0);
        }
    }
    b.setScreen(scr->first, scr->second);
    say(L"ГОТОВО");
    if (polls.size() < 10) {
        std::printf("[-] мало опросов\n");
        return 1;
    }
    Sleep(50);
    const Track tr = buildTrack(polls.front().ts - Q::fromUs(2e6), polls.back().tr + Q::fromUs(50000.0));
    const double dur = Q::toUs(polls.back().tr - polls.front().ts) / 1e6;
    std::printf("опросов 0x63: %zu за %.1f с = %.0f Гц, потеряно %llu, у края экрана %zu\n", polls.size(), dur,
        static_cast<double>(polls.size()) / dur, static_cast<unsigned long long>(b.lost()), clamped);
    printRawRate(tr);
    const Fit f = fit(polls, tr, -1);
    printFit("позиция платы против Raw Input", f);
    // Отчётов между опросами и размер одного шага позиции.
    size_t k0 = tr.upto(polls.front().ts + Q::fromUs(f.lagUs)), maxPer = 0;
    int64_t maxStep = 0, path = 0;
    for (size_t i = 1; i < polls.size(); ++i) {
        const size_t k = tr.upto(polls[i].ts + Q::fromUs(f.lagUs));
        maxPer = std::max(maxPer, k - k0);
        k0 = k;
        maxStep = std::max<int64_t>(maxStep, std::max(std::abs(polls[i].x - polls[i - 1].x), std::abs(polls[i].y - polls[i - 1].y)));
        path += std::abs(polls[i].x - polls[i - 1].x) + std::abs(polls[i].y - polls[i - 1].y);
    }
    std::printf("Raw Input между опросами: в среднем %.2f сообщ., max %zu | путь %lld отсчётов, max шаг за опрос %lld\n",
        static_cast<double>(tr.t.size()) / static_cast<double>(polls.size()), maxPer, static_cast<long long>(path),
        static_cast<long long>(maxStep));
    // Итог после "стоп": расхождение суммы за весь прогон.
    const Poll& last = polls.back();
    const size_t kl = tr.upto(last.ts + Q::fromUs(f.lagUs));
    std::printf("итог (мышь стоит): плата %+lld,%+lld, Raw Input %+lld,%+lld -> расхождение %+lld,%+lld\n",
        static_cast<long long>(last.x - polls.front().x), static_cast<long long>(last.y - polls.front().y),
        static_cast<long long>(tr.cx[kl] - tr.cx[tr.upto(polls.front().ts + Q::fromUs(f.lagUs))]),
        static_cast<long long>(tr.cy[kl] - tr.cy[tr.upto(polls.front().ts + Q::fromUs(f.lagUs))]),
        static_cast<long long>(last.x - tr.cx[kl] - f.offX), static_cast<long long>(last.y - tr.cy[kl] - f.offY));
    return 0;
}

// ---- --lock ----------------------------------------------------------------------------
struct LockCycle {
    int variant = 0;  // 0: 4 x 0x60 с паузой 2 мс, 1: одна 0x16
    int64_t t0 = 0, tLast = 0, tSnap = 0;
    uint16_t snap = 0;
    bool snapOk = false;
    int64_t tU0 = 0, tUSnap = 0;
    uint16_t usnap = 0xFFFF;
};

const char* kVariant[2] = {"4x0x60", "0x16  "};

// Направление: 0 +X, 1 -X, 2 +Y, 3 -Y. Компонента сообщения в этом направлении.
int32_t comp(const Track& tr, size_t i, int d) {
    switch (d) {
    case 0: return std::max(tr.dx[i], 0);
    case 1: return std::max(-tr.dx[i], 0);
    case 2: return std::max(tr.dy[i], 0);
    default: return std::max(-tr.dy[i], 0);
    }
}

void lockCmd(Board& b, LockCycle& c, bool on) {
    const uint8_t s = on ? 1 : 0;
    int64_t first = 0, last = 0;
    if (c.variant == 0) {
        for (const uint8_t t : {6, 7, 9, 10}) {  // как эталон: +X, -X, +Y, -Y по одной
            const uint8_t pl[2] = {t, s};
            last = b.noReply(0x60, pl, 2);
            if (!first) first = last;
        }
    } else {
        const uint8_t pl[4] = {s, s, s, s};  // left right down up
        first = last = b.noReply(0x16, pl, 4);
    }
    int64_t tr = 0;
    const auto l = b.locks(&tr);
    if (on) {
        c.t0 = first;
        c.tLast = last;
        c.tSnap = tr;
        c.snapOk = l.has_value();
        c.snap = l.value_or(0);
    } else {
        c.tU0 = first;
        c.tUSnap = tr;
        c.usnap = l.value_or(0xFFFF);
    }
}

int runLock(Board& b, int cycles) {
    const auto scr = b.screen();
    if (!scr) {
        std::printf("[-] нет ответа на GET 0x64\n");
        return 1;
    }
    if (!b.unlockAll()) {
        std::printf("[-] не удалось снять блокировки перед тестом\n");
        return 1;
    }
    b.setScreen(g_scrW ? g_scrW : 32767, g_scrH ? g_scrH : 32767);
    say(L"ВОДИТЕ МЫШЬЮ КРУГАМИ, НЕ ОСТАНАВЛИВАЯСЬ",
        L"мышь будет замирать на 0.4 с - это блокировка, продолжайте водить");
    sleepMs(1500.0);
    std::vector<Poll> polls;
    std::vector<LockCycle> cs;
    const int64_t tStart = Q::now();
    for (int c = 0; c < cycles && !g_stop.load(); ++c) {
        LockCycle lc;
        lc.variant = c % 2;
        auto pollFor = [&](double ms, int phase) {
            const int64_t end = Q::now() + Q::fromUs(ms * 1000.0);
            while (Q::now() < end && !g_stop.load()) {
                checkEsc();
                Poll p;
                p.phase = phase;
                p.cycle = c;
                if (b.pos(p.x, p.y, &p.ts, &p.tr)) {
                    polls.push_back(p);
                    g_ui.polls = static_cast<uint32_t>(polls.size());
                } else {
                    sleepMs(2.0);
                }
            }
        };
        pollFor(1000.0, 0);
        if (g_stop.load()) break;
        lockCmd(b, lc, true);
        pollFor(400.0, 1);
        lockCmd(b, lc, false);
        if (lc.usnap != 0 && !b.unlockAll()) {
            std::printf("[-] блокировка не снялась\n");
            break;
        }
        cs.push_back(lc);
    }
    {
        // хвост свободного движения после последнего цикла: для сверки сдвигов
        const int64_t end = Q::now() + Q::fromUs(300000.0);
        while (Q::now() < end && !g_stop.load()) {
            Poll p;
            p.cycle = static_cast<int>(cs.size());
            if (b.pos(p.x, p.y, &p.ts, &p.tr)) polls.push_back(p);
        }
    }
    const bool clean = b.unlockAll();
    b.setScreen(scr->first, scr->second);
    say(L"ГОТОВО");
    std::printf("блокировки сняты: %s\n", clean ? "да (снимок 0x56 = 0000)" : "НЕТ - запустите example_line.exe --check");
    if (cs.empty()) return 1;
    Sleep(50);
    const Track tr = buildTrack(tStart - Q::fromUs(1e6), Q::now());
    printRawRate(tr);
    const Fit f = fit(polls, tr, 0);
    printFit("до блокировки (свободно): позиция платы против Raw Input", f);

    std::printf("\nцикл вариант | снимок мс | последний физ. отсчёт после 1-й команды, мс (+X -X +Y -Y; '-' рука не шла) "
                "| любое движение | утечка отсчётов | позиция под блок. | сдвиг после | снятие мс\n");
    std::vector<double> anyLat[2], dirLat[2][4];
    for (size_t ci = 0; ci < cs.size(); ++ci) {
        const LockCycle& c = cs[ci];
        const size_t a = tr.upto(c.t0), z = tr.upto(c.tU0);
        char dirs[128] = {0};
        int64_t leak = 0;
        for (int d = 0; d < 4; ++d) {
            // рука шла в этом направлении перед блокировкой (последние 15 мс)?
            int64_t before = 0;
            for (size_t i = tr.upto(c.t0 - Q::fromUs(15000.0)); i < a; ++i) before += comp(tr, i, d);
            int64_t lastT = 0;
            for (size_t i = a; i < z; ++i) {
                const int32_t v = comp(tr, i, d);
                if (v) {
                    lastT = tr.t[i];
                    leak += v;
                }
            }
            char cell[24];
            if (before < 2) {
                std::snprintf(cell, sizeof(cell), "%6s", "-");
            } else {
                const double ms = lastT ? Q::toUs(lastT - c.t0) / 1000.0 : 0.0;
                std::snprintf(cell, sizeof(cell), "%6.2f", ms);
                dirLat[c.variant][d].push_back(ms);
            }
            std::strncat(dirs, cell, sizeof(dirs) - std::strlen(dirs) - 1);
        }
        int64_t lastAny = 0;
        for (size_t i = a; i < z; ++i) {
            if (tr.dx[i] || tr.dy[i]) lastAny = tr.t[i];
        }
        const double anyMs = lastAny ? Q::toUs(lastAny - c.t0) / 1000.0 : 0.0;
        anyLat[c.variant].push_back(anyMs);
        // позиция платы под блокировкой: от снимка до снятия
        int hx0 = 0, hy0 = 0, hx1 = 0, hy1 = 0;
        bool h0 = false;
        for (const Poll& p : polls) {
            if (p.cycle != static_cast<int>(ci) || p.phase != 1 || p.ts < c.tSnap) continue;
            if (!h0) {
                hx0 = p.x;
                hy0 = p.y;
                h0 = true;
            }
            hx1 = p.x;
            hy1 = p.y;
        }
        // сдвиг "позиция - Raw Input" между последним опросом до блокировки и первым после снятия
        const Poll* pre = nullptr;
        const Poll* post = nullptr;
        for (const Poll& p : polls) {
            if (p.cycle == static_cast<int>(ci) && p.phase == 0) pre = &p;
            if (p.cycle == static_cast<int>(ci) + 1 && p.phase == 0 && !post) post = &p;
        }
        char shift[32] = "?";
        if (pre && post) {
            const size_t k0 = tr.upto(pre->ts + Q::fromUs(f.lagUs)), k1 = tr.upto(post->ts + Q::fromUs(f.lagUs));
            const int64_t sx = (post->x - pre->x) - (tr.cx[k1] - tr.cx[k0]);
            const int64_t sy = (post->y - pre->y) - (tr.cy[k1] - tr.cy[k0]);
            std::snprintf(shift, sizeof(shift), "%+lld,%+lld", static_cast<long long>(sx), static_cast<long long>(sy));
        }
        // снятие: первое движение после первой команды снятия
        double unl = -1.0;
        for (size_t i = z; i < tr.t.size(); ++i) {
            if (tr.dx[i] || tr.dy[i]) {
                unl = Q::toUs(tr.t[i] - c.tU0) / 1000.0;
                break;
            }
        }
        std::printf("%4zu %s | %04X %5.2f | %s | %6.2f | %6lld | %+5d,%+5d | %9s | %6.2f%s\n", ci, kVariant[c.variant],
            c.snap, c.snapOk ? Q::toUs(c.tSnap - c.t0) / 1000.0 : -1.0, dirs, anyMs, static_cast<long long>(leak),
            hx1 - hx0, hy1 - hy0, shift, unl, c.usnap == 0 ? "" : "  (снято повтором)");
    }
    auto med = [](std::vector<double> v) {
        if (v.empty()) return -1.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    auto mx = [](const std::vector<double>& v) { return v.empty() ? -1.0 : *std::max_element(v.begin(), v.end()); };
    std::printf("\nитог: последний физический отсчёт после первой команды блокировки (мс), медиана / max\n");
    for (int v = 0; v < 2; ++v) {
        std::printf("  %s: любое движение %.2f / %.2f |", kVariant[v], med(anyLat[v]), mx(anyLat[v]));
        const char* dn[4] = {"+X", "-X", "+Y", "-Y"};
        for (int d = 0; d < 4; ++d) {
            std::printf(" %s %.2f / %.2f (n=%zu)", dn[d], med(dirLat[v][d]), mx(dirLat[v][d]), dirLat[v][d].size());
        }
        std::printf("\n");
    }
    return 0;
}

// ---- --lockloss ------------------------------------------------------------------------
// Сколько команд блокировки доходит, когда рука двигает мышь и когда нет. Попытка: блокировка
// (4 x 0x60 с паузой 2/3/4 мс или одна 0x16), снимок 0x56, снятие одной 0x16 со сверкой, 30 мс
// свободно. "Рука шла" = в последние 20 мс перед попыткой были сообщения Raw Input.
int runLockLoss(Board& b, double seconds) {
    if (!b.unlockAll()) {
        std::printf("[-] не удалось снять блокировки перед тестом\n");
        return 1;
    }
    const auto scr = b.screen();
    if (g_scrW && scr) {
        b.setScreen(g_scrW, g_scrH);
        std::printf("km.screen был %dx%d, на время замера %dx%d\n", scr->first, scr->second, g_scrW, g_scrH);
    } else if (scr) {
        std::printf("km.screen %dx%d\n", scr->first, scr->second);
    }
    say(L"ВОДИТЕ МЫШЬЮ КРУГАМИ, НЕ ОСТАНАВЛИВАЯСЬ", L"мышь будет дёргаться - это блокировки, продолжайте");
    sleepMs(1500.0);
    const char* names[4] = {"4x0x60 пауза 2 мс", "4x0x60 пауза 3 мс", "4x0x60 пауза 4 мс", "0x16 одна        "};
    // [вариант][рука шла?][дошло направлений 0..4, 5 = снимок не пришёл]
    size_t hist[4][2][6] = {};
    size_t snapLost = 0, unlockRepeats = 0;
    const int64_t tEnd = Q::now() + Q::fromUs(seconds * 1e6);
    for (int n = 0; Q::now() < tEnd && !g_stop.load(); ++n) {
        checkEsc();
        const int v = n % 4;
        const int64_t t0 = Q::now();
        const size_t rawNow = g_rawN.load(std::memory_order_acquire);
        bool moving = false;
        for (size_t i = rawNow; i > 0 && g_raw[i - 1].t > t0 - Q::fromUs(20000.0); --i) {
            if (g_raw[i - 1].dx || g_raw[i - 1].dy) {
                moving = true;
                break;
            }
        }
        if (v < 3) {
            const double gap = 2000.0 + 1000.0 * v;
            for (const uint8_t t : {6, 7, 9, 10}) {
                const uint8_t pl[2] = {t, 1};
                const int64_t ts = b.noReply(0x60, pl, 2);
                spinUntil(ts + Q::fromUs(gap));
            }
        } else {
            const uint8_t pl[4] = {1, 1, 1, 1};
            b.noReply(0x16, pl, 4);
        }
        sleepMs(3.0);
        std::optional<uint16_t> l;
        for (int k = 0; k < 3 && !l; ++k) {
            l = b.locks();
            if (!l) {
                ++snapLost;
                sleepMs(3.0);
            }
        }
        int landed = 5;
        if (l) {
            landed = 0;
            for (const int bit : {6, 7, 9, 10}) landed += (*l >> bit) & 1;
        }
        ++hist[v][moving ? 1 : 0][landed];
        for (int k = 0;; ++k) {
            const uint8_t pl[4] = {0, 0, 0, 0};
            b.noReply(0x16, pl, 4);
            sleepMs(1.0);
            const auto u = b.locks();
            if (u && *u == 0) break;
            ++unlockRepeats;
            if (k >= 20) {
                std::printf("[-] блокировка не снимается\n");
                g_stop = true;
                break;
            }
        }
        if (g_prePollMs > 0.0) {
            // как в жизни: позиция читается до самой блокировки, команда - сразу после ответа
            const int64_t end = Q::now() + Q::fromUs(g_prePollMs * 1000.0);
            int x = 0, y = 0;
            while (Q::now() < end && !g_stop.load()) {
                if (!b.pos(x, y)) sleepMs(2.0);
            }
        } else {
            sleepMs(30.0);
        }
    }
    const bool clean = b.unlockAll();
    if (g_scrW && scr) b.setScreen(scr->first, scr->second);
    say(L"ГОТОВО");
    std::printf("блокировки сняты: %s | снимков без ответа %zu, снятий с повтором %zu\n",
        clean ? "да (снимок 0x56 = 0000)" : "НЕТ - запустите example_line.exe --check", snapLost, unlockRepeats);
    std::printf("вариант            | рука | попыток | дошли все 4 | дошло 3 | 2 | 1 | 0 | снимок потерян\n");
    for (int v = 0; v < 4; ++v) {
        for (int m = 1; m >= 0; --m) {
            const size_t* h = hist[v][m];
            size_t total = 0;
            for (int k = 0; k < 6; ++k) total += h[k];
            if (!total) continue;
            std::printf("%s | %s | %7zu | %6zu %5.1f%% | %7zu | %zu | %zu | %zu | %zu\n", names[v], m ? "шла " : "нет ",
                total, h[4], pct(h[4], total), h[3], h[2], h[1], h[0], h[5]);
        }
    }
    return 0;
}

// ---- --inject --------------------------------------------------------------------------
int runInject(const std::string& host, uint16_t port, int trials) {
    makcu::UdpBatchTransport tx;
    std::string err;
    if (!tx.open(host, port, err)) {
        std::printf("[-] %s\n", err.c_str());
        return 1;
    }
    std::printf("[+] эталон: %s\n", tx.info().c_str());
    say(L"НЕ ТРОГАЙТЕ МЫШЬ", L"курсор будет описывать круги");
    sleepMs(1500.0);
    std::printf("попытка | отправлено X,Y | Windows X,Y | первый отчёт после 1-го move(), мс | хвост после "
                "последнего move(): мс / отчётов\n");
    std::vector<double> tails;
    for (int n = 0; n < trials && !g_stop.load(); ++n) {
        double phase = 0.0, fx = 0.0, fy = 0.0;
        int64_t pendX = 0, pendY = 0, sentX = 0, sentY = 0, tFirst = 0, tLast = 0;
        const int64_t tA = Q::now();
        int64_t due = tA;
        for (int tick = 0; tick < 300 && !g_stop.load(); ++tick) {
            spinUntil(due);
            due += Q::fromUs(1000.0);
            const double r = 120.0, w = 2.0 * 3.14159265358979 / 300.0;
            fx += -r * w * std::sin(phase);
            fy += r * w * std::cos(phase);
            phase += w;
            const int64_t ix = std::llround(fx), iy = std::llround(fy);
            fx -= static_cast<double>(ix);
            fy -= static_cast<double>(iy);
            pendX += ix;
            pendY += iy;
            if ((pendX || pendY) && tx.move(static_cast<int16_t>(pendX), static_cast<int16_t>(pendY))) {
                tLast = Q::now();
                if (!tFirst) tFirst = tLast;
                sentX += pendX;
                sentY += pendY;
                pendX = pendY = 0;
            }
        }
        sleepMs(400.0);
        const Track tr = buildTrack(tA, Q::now());
        int64_t lastT = 0, firstT = 0;
        size_t after = 0;
        for (size_t i = 0; i < tr.t.size(); ++i) {
            if (!tr.dx[i] && !tr.dy[i]) continue;
            if (!firstT) firstT = tr.t[i];
            lastT = tr.t[i];
            if (tr.t[i] > tLast) ++after;
        }
        const double tail = lastT ? Q::toUs(lastT - tLast) / 1000.0 : -1.0;
        tails.push_back(tail);
        std::printf("%7d | %+5lld,%+5lld | %+5lld,%+5lld | %6.2f | %6.2f / %zu%s\n", n, static_cast<long long>(sentX),
            static_cast<long long>(sentY), static_cast<long long>(tr.cx.back()), static_cast<long long>(tr.cy.back()),
            firstT ? Q::toUs(firstT - tFirst) / 1000.0 : -1.0, tail, after,
            (tr.cx.back() != sentX || tr.cy.back() != sentY) ? "  <-- путь не совпал (мышь трогали?)" : "");
    }
    std::printf("%s\n", tx.statsLine().c_str());
    tx.close();
    say(L"ГОТОВО");
    if (!tails.empty()) {
        std::sort(tails.begin(), tails.end());
        std::printf("хвост после последнего move(): медиана %.2f мс, max %.2f мс\n", tails[tails.size() / 2], tails.back());
    }
    return 0;
}

// ---- --reflock -------------------------------------------------------------------------
// Блокировка так, как её делает программа на эталоне: tx.setLock(X), tx.setLock(Y) - и сколько
// после этого физическое движение ещё доходит до Windows. Водить мышью кругами.
int runRefLock(const std::string& host, uint16_t port, int cycles) {
    makcu::UdpBatchTransport tx;
    std::string err;
    if (!tx.open(host, port, err)) {
        std::printf("[-] %s\n", err.c_str());
        return 1;
    }
    std::printf("[+] эталон: %s\n", tx.info().c_str());
    say(L"ВОДИТЕ МЫШЬЮ КРУГАМИ, НЕ ОСТАНАВЛИВАЯСЬ",
        L"мышь будет замирать на 0.4 с - это блокировка, продолжайте водить");
    sleepMs(1500.0);
    using makcu::LockTarget;
    const uint16_t want = static_cast<uint16_t>(makcu::lockBit(LockTarget::X) | makcu::lockBit(LockTarget::XPos) |
        makcu::lockBit(LockTarget::XNeg) | makcu::lockBit(LockTarget::Y) | makcu::lockBit(LockTarget::YPos) |
        makcu::lockBit(LockTarget::YNeg));
    struct C {
        int64_t t0 = 0, tBoard = 0, tU = 0;
        uint64_t repairs = 0;
        int style = 0;  // 0: setLock(X) + setLock(Y), 1: setLocks({X, Y})
    };
    std::vector<C> cs;
    const int64_t tStart = Q::now();
    for (int c = 0; c < cycles && !g_stop.load(); ++c) {
        sleepMs(1000.0);
        if (g_stop.load()) break;
        C r;
        const uint64_t rep0 = tx.stats().repairs;
        r.style = c % 2;
        r.t0 = Q::now();
        if (r.style == 0) {
            tx.setLock(LockTarget::X, true);
            tx.setLock(LockTarget::Y, true);
        } else {
            tx.setLocks({LockTarget::X, LockTarget::Y}, true);
        }
        const int64_t holdEnd = r.t0 + Q::fromUs(400000.0);
        while (Q::now() < holdEnd && !g_stop.load()) {
            if (!r.tBoard && tx.locksOnBoard() == want) r.tBoard = Q::now();
            YieldProcessor();
        }
        r.tU = Q::now();
        tx.unlockAll();
        const int64_t uEnd = Q::now() + Q::fromUs(300000.0);
        while (tx.locksOnBoard() != 0 && Q::now() < uEnd) Sleep(1);
        r.repairs = tx.stats().repairs - rep0;
        cs.push_back(r);
    }
    sleepMs(300.0);
    std::printf("%s\n", tx.statsLine().c_str());
    tx.close();
    say(L"ГОТОВО");
    if (cs.empty()) return 1;
    const Track tr = buildTrack(tStart, Q::now());
    printRawRate(tr);
    std::printf("цикл вызов           | последний физ. отсчёт после setLock(), мс (+X -X +Y -Y; '-' рука не шла) | любое движение | "
                "утечка отсчётов | снимок платы мс | повторов\n");
    std::vector<double> anyLat[2];
    const char* styleName[2] = {"setLock X, Y  ", "setLocks({X,Y})"};
    for (size_t ci = 0; ci < cs.size(); ++ci) {
        const C& c = cs[ci];
        const size_t a = tr.upto(c.t0), z = tr.upto(c.tU);
        char dirs[128] = {0};
        int64_t leak = 0;
        for (int d = 0; d < 4; ++d) {
            int64_t before = 0;
            for (size_t i = tr.upto(c.t0 - Q::fromUs(15000.0)); i < a; ++i) before += comp(tr, i, d);
            int64_t lastT = 0;
            for (size_t i = a; i < z; ++i) {
                if (const int32_t v = comp(tr, i, d)) {
                    lastT = tr.t[i];
                    leak += v;
                }
            }
            char cell[24];
            if (before < 2) std::snprintf(cell, sizeof(cell), "%7s", "-");
            else std::snprintf(cell, sizeof(cell), "%7.2f", lastT ? Q::toUs(lastT - c.t0) / 1000.0 : 0.0);
            std::strncat(dirs, cell, sizeof(dirs) - std::strlen(dirs) - 1);
        }
        int64_t lastAny = 0;
        for (size_t i = a; i < z; ++i) {
            if (tr.dx[i] || tr.dy[i]) lastAny = tr.t[i];
        }
        const double anyMs = lastAny ? Q::toUs(lastAny - c.t0) / 1000.0 : 0.0;
        anyLat[c.style].push_back(anyMs);
        std::printf("%4zu %s | %s | %6.2f | %5lld | %6.2f | %llu\n", ci, styleName[c.style], dirs, anyMs, static_cast<long long>(leak),
            c.tBoard ? Q::toUs(c.tBoard - c.t0) / 1000.0 : -1.0, static_cast<unsigned long long>(c.repairs));
    }
    for (int s = 0; s < 2; ++s) {
        std::vector<double>& v = anyLat[s];
        if (v.empty()) continue;
        std::sort(v.begin(), v.end());
        std::printf("итог %s: последнее физическое движение после вызова: медиана %.2f мс, max %.2f мс (n=%zu)\n",
            styleName[s], v[v.size() / 2], v.back(), v.size());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::string host = "192.168.50.175";
    uint16_t port = 8080;
    double accuracyS = 0.0;
    int lockN = 0, injectN = 0, refLockN = 0;
    double lossS = 0.0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
        else if (std::strcmp(argv[i], "--accuracy") == 0 && i + 1 < argc) accuracyS = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--lock") == 0 && i + 1 < argc) lockN = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--inject") == 0 && i + 1 < argc) injectN = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--lockloss") == 0 && i + 1 < argc) lossS = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--prepoll") == 0 && i + 1 < argc) g_prePollMs = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--reflock") == 0 && i + 1 < argc) refLockN = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--screen") == 0 && i + 2 < argc) {
            g_scrW = std::atoi(argv[++i]);
            g_scrH = std::atoi(argv[++i]);
        }
        else {
            std::printf("track_test.exe [--host IP] --accuracy S | --lock N | --lockloss S | --inject N | --reflock N\n");
            return 2;
        }
    }
    if (accuracyS <= 0.0 && lockN <= 0 && injectN <= 0 && lossS <= 0.0 && refLockN <= 0) {
        std::printf("track_test.exe [--host IP] --accuracy S | --lock N | --lockloss S | --inject N | --reflock N\n");
        return 2;
    }
    SetConsoleCtrlHandler(onCtrl, TRUE);
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    std::thread ui(uiThread);
    while (!g_ui.ready.load()) Sleep(5);
    say(L"КЛИКНИТЕ В ЭТО ОКНО", L"тест начнётся, когда окно будет в фокусе");
    if (!waitForeground(120.0)) {
        std::printf("[-] окно теста так и не получило фокус\n");
        g_stop = true;
        ui.join();
        return 1;
    }
    int rc = 0;
    if (injectN > 0) {
        rc = runInject(host, port, injectN);
    } else if (refLockN > 0) {
        rc = runRefLock(host, port, refLockN);
    } else {
        Board b;
        std::string err;
        if (!b.open(host, port, err)) {
            std::printf("[-] %s\n", err.c_str());
            rc = 1;
        } else if (auto fw = b.ask(0x04); !fw || fw->size() < 4) {
            std::printf("[-] плата не отвечает (%s:%u)\n", host.c_str(), port);
            rc = 1;
        } else {
            std::printf("[+] %s:%u, прошивка %u\n", host.c_str(), port,
                static_cast<unsigned>((*fw)[0] | (*fw)[1] << 8 | (*fw)[2] << 16 | (*fw)[3] << 24));
            rc = accuracyS > 0.0 ? runAccuracy(b, accuracyS) : lossS > 0.0 ? runLockLoss(b, lossS) : runLock(b, lockN);
        }
    }
    g_cleanDone = true;
    Sleep(800);
    g_stop = true;
    ui.join();
    timeEndPeriod(1);
    return rc;
}
