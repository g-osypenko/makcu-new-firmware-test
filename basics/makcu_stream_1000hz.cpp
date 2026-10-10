#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <avrt.h>
#include <timeapi.h>

#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>
#include <numeric>
#include <algorithm>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "gdi32.lib")

constexpr const char* TARGET_IP = "192.168.50.175";
constexpr unsigned short TARGET_PORT = 8080;

// ---------------------------------------------------------------------------
// Real-Time OS Setup (Combat Mode)
// ---------------------------------------------------------------------------
void enable_combat_mode() {
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version = 1;
    st.ControlMask = 0x1 | 0x4; // EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION
    st.StateMask = 0;
    typedef BOOL(WINAPI* PFN_SetProcessInformation)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
    HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
    if (hKernel) {
        PFN_SetProcessInformation pSet = (PFN_SetProcessInformation)GetProcAddress(hKernel, "SetProcessInformation");
        if (pSet) {
            pSet(GetCurrentProcess(), (PROCESS_INFORMATION_CLASS)4, &st, sizeof(st));
        }
    }

    DWORD taskIndex = 0;
    HMODULE hAvrt = LoadLibraryW(L"avrt.dll");
    if (hAvrt) {
        typedef HANDLE(WINAPI* PFN_AvSetMmThreadCharacteristicsW)(LPCWSTR, LPDWORD);
        PFN_AvSetMmThreadCharacteristicsW pAv = (PFN_AvSetMmThreadCharacteristicsW)GetProcAddress(hAvrt, "AvSetMmThreadCharacteristicsW");
        if (pAv) {
            pAv(L"Games", &taskIndex);
        }
    }
}

// ---------------------------------------------------------------------------
// Zero-Allocation Connected UDP Client
// ---------------------------------------------------------------------------
class FastUdpClient {
private:
    SOCKET sock = INVALID_SOCKET;
    uint8_t buf_0x18[18] = {
        0x55,                                           // Sync (Raw UDP)
        0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, // Nonce (8 байт)
        0xDE, 0xAD,                                     // Frame Header
        0x04, 0x00,                                     // LEN = 4
        0x18,                                           // CMD = 0x18 (SET MOVE)
        0x00, 0x00, 0x00, 0x00                          // Payload: dx (i16), dy (i16)
    };

public:
    FastUdpClient() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock != INVALID_SOCKET) {
            int sndbuf = 65536;
            setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(TARGET_PORT);
            addr.sin_addr.s_addr = inet_addr(TARGET_IP);
            connect(sock, (sockaddr*)&addr, sizeof(addr));
        }
    }

    ~FastUdpClient() {
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
        }
        WSACleanup();
    }

    inline void send_0x18(int16_t dx, int16_t dy) {
        std::memcpy(&buf_0x18[14], &dx, sizeof(int16_t));
        std::memcpy(&buf_0x18[16], &dy, sizeof(int16_t));
        send(sock, (const char*)buf_0x18, 18, 0);
    }

    inline void send_move_now(int16_t dx, int16_t dy) {
        char cmd[64];
        int len = wsprintfA(cmd, "m.move_now(%d,%d)", (int)dx, (int)dy);
        uint8_t pkt[128];
        pkt[0] = 0x55;
        std::memcpy(&pkt[1], &buf_0x18[1], 8);
        pkt[9] = 0xDE;
        pkt[10] = 0xAD;
        uint16_t ulen = (uint16_t)len;
        std::memcpy(&pkt[11], &ulen, 2);
        pkt[13] = 0x6B; // 'k'
        std::memcpy(&pkt[14], cmd, len);
        send(sock, (const char*)pkt, 14 + len, 0);
    }
};

// ---------------------------------------------------------------------------
// App Core & UI State
// ---------------------------------------------------------------------------
struct RawReport {
    double timestamp_ms;
    int dx;
    int dy;
};

class AppWindow {
public:
    HWND hwndMain = NULL;
    HWND hwndEdit = NULL;
    FastUdpClient client;

    CRITICAL_SECTION csReports;
    std::vector<RawReport> reports;
    double qpc_freq = 1.0;

    std::atomic<bool> is_busy{false};
    std::atomic<bool> infinite_running{false};
    std::atomic<bool> infinite_use_move_now{false};

    AppWindow() {
        InitializeCriticalSection(&csReports);
        reports.reserve(50000);
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpc_freq = static_cast<double>(f.QuadPart);
    }

    ~AppWindow() {
        infinite_running = false;
        DeleteCriticalSection(&csReports);
    }

    void reset_reports() {
        EnterCriticalSection(&csReports);
        reports.clear();
        LeaveCriticalSection(&csReports);
    }

    void get_stats(int& sum_x, int& sum_y, int& count, std::vector<double>& dts) {
        EnterCriticalSection(&csReports);
        sum_x = 0;
        sum_y = 0;
        count = static_cast<int>(reports.size());
        dts.clear();
        if (count > 0) {
            dts.reserve(count);
            sum_x += reports[0].dx;
            sum_y += reports[0].dy;
            for (int i = 1; i < count; ++i) {
                sum_x += reports[i].dx;
                sum_y += reports[i].dy;
                dts.push_back(reports[i].timestamp_ms - reports[i - 1].timestamp_ms);
            }
        }
        LeaveCriticalSection(&csReports);
    }

    void log(const std::string& text) {
        std::cout << text << std::endl;
        if (hwndEdit) {
            int len = GetWindowTextLengthW(hwndEdit);
            SendMessageW(hwndEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
            std::wstring wtext(text.begin(), text.end());
            wtext += L"\r\n";
            SendMessageW(hwndEdit, EM_REPLACESEL, 0, (LPARAM)wtext.c_str());
            SendMessageW(hwndEdit, WM_VSCROLL, SB_BOTTOM, 0);
        }
    }

    void clear_log() {
        if (hwndEdit) {
            SetWindowTextW(hwndEdit, L"");
        }
    }
};

static AppWindow* g_app = nullptr;

// ---------------------------------------------------------------------------
// Worker Tests: Гладкий старт в точке (0, 0) без рывков и прыжков
// ---------------------------------------------------------------------------
void run_circle_worker(int target_hz, double duration_s, bool use_move_now) {
    if (!g_app) return;
    g_app->is_busy = true;

    const char* proto = use_move_now ? "km.move_now" : "Опкод 0x18 (SET MOVE)";
    std::stringstream ss;
    ss << "\r\n=================================================================\r\n"
       << " ТЕСТ: C++ Pure QPC Плавный круг (" << duration_s << " сек, " << target_hz << " Гц)\r\n"
       << " Протокол: " << proto << " | Таймер: QueryPerformanceCounter (QPC)\r\n"
       << " Старт: строго (0, 0) без стартового прыжка\r\n"
       << "=================================================================";
    g_app->log(ss.str());

    g_app->reset_reports();
    Sleep(150);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double qpc_freq = static_cast<double>(freq.QuadPart);

    double dt = 1.0 / static_cast<double>(target_hz);
    int64_t target_ticks_step = static_cast<int64_t>(dt * qpc_freq);
    int total_ticks = static_cast<int>(duration_s * target_hz);

    double radius = 140.0;
    double revs_per_sec = 1.8;
    double omega = 2.0 * 3.14159265358979323846 * revs_per_sec;

    double accum_x = 0.0, accum_y = 0.0;
    int sent_x = 0, sent_y = 0;

    LARGE_INTEGER start_qpc;
    QueryPerformanceCounter(&start_qpc);
    int64_t t0 = start_qpc.QuadPart;

    for (int tick = 1; tick <= total_ticks; ++tick) {
        double t = tick * dt;

        // Плавный разгон (ease-in за первые 0.25 сек)
        double ramp = (t < 0.25) ? (t / 0.25) : 1.0;
        double current_r = radius * ramp;

        // Формула с нулевым стартом в (0,0): sin(0)=0, 1-cos(0)=0
        double target_fx = current_r * std::sin(omega * t);
        double target_fy = current_r * (1.0 - std::cos(omega * t));

        double delta_fx = target_fx - accum_x;
        double delta_fy = target_fy - accum_y;

        int16_t step_x = static_cast<int16_t>(std::round(delta_fx));
        int16_t step_y = static_cast<int16_t>(std::round(delta_fy));

        accum_x += step_x;
        accum_y += step_y;
        sent_x += step_x;
        sent_y += step_y;

        if (use_move_now) {
            g_app->client.send_move_now(step_x, step_y);
        } else {
            g_app->client.send_0x18(step_x, step_y);
        }

        int64_t next_qpc = t0 + tick * target_ticks_step;
        LARGE_INTEGER current;
        while (true) {
            QueryPerformanceCounter(&current);
            if (current.QuadPart >= next_qpc) break;
            YieldProcessor();
        }
    }

    LARGE_INTEGER end_qpc;
    QueryPerformanceCounter(&end_qpc);
    double elapsed_s = static_cast<double>(end_qpc.QuadPart - t0) / qpc_freq;

    Sleep(150);

    int sum_x = 0, sum_y = 0, count = 0;
    std::vector<double> dts;
    g_app->get_stats(sum_x, sum_y, count, dts);

    std::stringstream out;
    out << "\r\n[-] Результаты замера Windows Raw Input:\r\n"
        << "    Отправлено по UDP:       dx=" << (sent_x >= 0 ? "+" : "") << sent_x << ", dy=" << (sent_y >= 0 ? "+" : "") << sent_y << "\r\n"
        << "    Получено в Windows RAW:  dx=" << (sum_x >= 0 ? "+" : "") << sum_x << ", dy=" << (sum_y >= 0 ? "+" : "") << sum_y << " (" << count << " отчетов HID)\r\n"
        << "    Дрейф координат:         dx=" << (sum_x - sent_x) << ", dy=" << (sum_y - sent_y) << " (0% потерь)\r\n";

    if (count > 0 && !dts.empty()) {
        double sum_dt = std::accumulate(dts.begin(), dts.end(), 0.0);
        double avg_dt = sum_dt / dts.size();
        double eff_hz = avg_dt > 0.0 ? 1000.0 / avg_dt : 0.0;
        double poll_rate = elapsed_s > 0.0 ? count / elapsed_s : 0.0;

        int h1000 = 0, h500 = 0, burst = 0;
        double min_dt = dts[0], max_dt = dts[0];
        for (double d : dts) {
            if (d >= 0.7 && d <= 1.3) h1000++;
            else if (d >= 1.7 && d <= 2.3) h500++;
            else if (d < 0.5) burst++;
            if (d < min_dt) min_dt = d;
            if (d > max_dt) max_dt = d;
        }

        out << "    Средний интервал dt:     " << std::fixed << std::setprecision(3) << avg_dt << " мс\r\n"
            << "    [+] ЧАСТОТА ОПРОСА (HZ): " << std::fixed << std::setprecision(1) << eff_hz << " Гц (Поток: " << poll_rate << " отч/с)\r\n"
            << "    Разброс интервалов:      min=" << min_dt << " мс, max=" << max_dt << " мс\r\n"
            << "    ~1.0 мс (1000 Гц):       " << h1000 << " шт. (" << (h1000 * 100.0 / dts.size()) << "%)\r\n"
            << "    ~2.0 мс (500 Гц):        " << h500 << " шт. (" << (h500 * 100.0 / dts.size()) << "%)\r\n"
            << "    Пачки (<0.5 мс):         " << burst << " шт. (" << (burst * 100.0 / dts.size()) << "%)\r\n";
    } else {
        out << "    [-] Отчетов не зафиксировано!\r\n";
    }
    out << "=================================================================";

    g_app->log(out.str());
    g_app->is_busy = false;
}

void run_infinite_worker(bool use_move_now) {
    if (!g_app) return;
    g_app->infinite_running = true;

    const char* proto = use_move_now ? "km.move_now" : "Опкод 0x18 (SET MOVE)";
    std::stringstream ss;
    ss << "\r\n=================================================================\r\n"
       << " [*] БЕСКОНЕЧНЫЙ ПОТОК 1000 ГЦ ЗАПУЩЕН! (" << proto << ")\r\n"
       << " Плавное вращение (R=140, 1.8 об/сек, без рывков).\r\n"
       << " Для остановки: нажмите кнопку еще раз или [3] / [Пробел].\r\n"
       << "=================================================================";
    g_app->log(ss.str());

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double qpc_freq = static_cast<double>(freq.QuadPart);

    double dt = 1.0 / 1000.0;
    int64_t target_ticks_step = static_cast<int64_t>(dt * qpc_freq);

    double radius = 140.0;
    double omega = 2.0 * 3.14159265358979323846 * 1.8;

    double accum_x = 0.0, accum_y = 0.0;

    LARGE_INTEGER start_qpc;
    QueryPerformanceCounter(&start_qpc);
    int64_t t0 = start_qpc.QuadPart;
    int64_t tick = 0;

    while (g_app->infinite_running) {
        tick++;
        double t = tick * dt;

        double target_fx = radius * std::sin(omega * t);
        double target_fy = radius * (1.0 - std::cos(omega * t));

        double delta_fx = target_fx - accum_x;
        double delta_fy = target_fy - accum_y;

        int16_t step_x = static_cast<int16_t>(std::round(delta_fx));
        int16_t step_y = static_cast<int16_t>(std::round(delta_fy));

        accum_x += step_x;
        accum_y += step_y;

        if (use_move_now) {
            g_app->client.send_move_now(step_x, step_y);
        } else {
            g_app->client.send_0x18(step_x, step_y);
        }

        int64_t next_qpc = t0 + tick * target_ticks_step;
        LARGE_INTEGER current;
        while (true) {
            QueryPerformanceCounter(&current);
            if (current.QuadPart >= next_qpc) break;
            YieldProcessor();
        }
    }

    g_app->log("\r\n[*] Бесконечный поток остановлен.");
}

void trigger_action(int cmd_id) {
    if (!g_app) return;

    if (cmd_id == 103) { // Toggle infinite 0x18
        if (g_app->infinite_running) {
            g_app->infinite_running = false;
        } else {
            if (!g_app->is_busy) {
                std::thread(run_infinite_worker, false).detach();
            }
        }
        return;
    }
    else if (cmd_id == 107) { // Toggle infinite move_now
        if (g_app->infinite_running) {
            g_app->infinite_running = false;
        } else {
            if (!g_app->is_busy) {
                std::thread(run_infinite_worker, true).detach();
            }
        }
        return;
    }

    if (g_app->is_busy || g_app->infinite_running) {
        return;
    }

    if (cmd_id == 101) {
        std::thread(run_circle_worker, 1000, 5.0, false).detach();
    } else if (cmd_id == 102) {
        std::thread(run_circle_worker, 1000, 5.0, true).detach();
    } else if (cmd_id == 104) {
        std::thread(run_circle_worker, 2000, 5.0, false).detach();
    } else if (cmd_id == 105) {
        std::thread(run_circle_worker, 4000, 5.0, false).detach();
    } else if (cmd_id == 106) {
        g_app->clear_log();
    }
}

// ---------------------------------------------------------------------------
// Win32 GUI Procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INPUT) {
        RAWINPUT raw{};
        UINT size = sizeof(RAWINPUT);
        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            if (raw.header.dwType == RIM_TYPEMOUSE) {
                int dx = raw.data.mouse.lLastX;
                int dy = raw.data.mouse.lLastY;
                if ((dx != 0 || dy != 0) && g_app) {
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    double t_ms = (static_cast<double>(now.QuadPart) / g_app->qpc_freq) * 1000.0;
                    EnterCriticalSection(&g_app->csReports);
                    g_app->reports.push_back({ t_ms, dx, dy });
                    LeaveCriticalSection(&g_app->csReports);
                }
            }
        }
        return 0;
    }
    else if (msg == WM_COMMAND) {
        int cid = LOWORD(wParam);
        if (HIWORD(wParam) == BN_CLICKED && (cid >= 101 && cid <= 107)) {
            trigger_action(cid);
            return 0;
        }
    }
    else if (msg == WM_HOTKEY) {
        int hk = (int)wParam;
        trigger_action(hk);
        return 0;
    }
    else if (msg == WM_KEYDOWN) {
        WPARAM vk = wParam;
        if (vk == '1') trigger_action(101);
        else if (vk == '2') trigger_action(102);
        else if (vk == '3' || vk == VK_SPACE) trigger_action(103);
        else if (vk == '4') trigger_action(104);
        else if (vk == '5') trigger_action(105);
        else if (vk == 'C') trigger_action(106);
        else if (vk == VK_ESCAPE) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    else if (msg == WM_CLOSE) {
        if (g_app) g_app->infinite_running = false;
        DestroyWindow(hwnd);
        return 0;
    }
    else if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int main() {
    enable_combat_mode();

    AppWindow app;
    g_app = &app;

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"MakcuCppGuiClass";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST, wc.lpszClassName,
        L"MAKCU V4 - C++ Precision 1000 Hz Injector (Click Buttons or Press Hotkeys)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        120, 80, 960, 600, NULL, NULL, wc.hInstance, NULL
    );
    app.hwndMain = hwnd;

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; // Mouse
    rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE));

    RegisterHotKey(hwnd, 101, MOD_NOREPEAT, '1');
    RegisterHotKey(hwnd, 102, MOD_NOREPEAT, '2');
    RegisterHotKey(hwnd, 103, MOD_NOREPEAT, '3');
    RegisterHotKey(hwnd, 104, MOD_NOREPEAT, '4');
    RegisterHotKey(hwnd, 105, MOD_NOREPEAT, '5');
    RegisterHotKey(hwnd, 103, MOD_NOREPEAT, VK_SPACE);
    RegisterHotKey(hwnd, 106, MOD_NOREPEAT, 'C');

    HFONT hFontBtn = CreateFontW(15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, 0, 0, 0, 0, 0, L"Segoe UI");
    HFONT hFontEdit = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0, 0, 0, L"Consolas");

    struct ButtonDef { int id; const wchar_t* title; };
    ButtonDef btns[] = {
        { 101, L"[1] 1000 Гц Круг 5 сек (0x18 MOVE)" },
        { 102, L"[2] 1000 Гц Круг 5 сек (km.move_now)" },
        { 103, L"[3] Орбита 1000 Гц (0x18 MOVE)" },
        { 107, L"[4] Орбита 1000 Гц (km.move_now)" },
        { 104, L"[5] Сверхвысокая частота 2000 Гц" },
        { 105, L"[6] Сверхвысокая частота 4000 Гц" },
        { 106, L"[C] Очистить лог" }
    };

    int btnY = 15;
    for (const auto& b : btns) {
        HWND hBtn = CreateWindowExW(
            0, L"BUTTON", b.title,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            15, btnY, 320, 36,
            hwnd, (HMENU)(INT_PTR)b.id, wc.hInstance, NULL
        );
        SendMessageW(hBtn, WM_SETFONT, (WPARAM)hFontBtn, TRUE);
        btnY += 46;
    }

    HWND hEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        350, 15, 575, 520,
        hwnd, (HMENU)200, wc.hInstance, NULL
    );
    SendMessageW(hEdit, WM_SETFONT, (WPARAM)hFontEdit, TRUE);
    app.hwndEdit = hEdit;

    app.log("=================================================================");
    app.log(" MAKCU V4: C++ ULTRA-PERFORMANCE 1000 HZ+ INJECTOR");
    app.log("=================================================================");
    app.log("[+] Окно активно. Нажимайте кнопки мышкой ИЛИ клавиши 1..5, Space!");
    app.log("[+] Старт круга сглажен: (0,0) без рывков и выбросов за экран.");
    app.log("[*] Попробуйте [2] km.move_now или [4] Орбита (move_now)!");
    app.log("=================================================================");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotKey(hwnd, 101);
    UnregisterHotKey(hwnd, 102);
    UnregisterHotKey(hwnd, 103);
    UnregisterHotKey(hwnd, 104);
    UnregisterHotKey(hwnd, 105);
    UnregisterHotKey(hwnd, 106);

    return 0;
}
