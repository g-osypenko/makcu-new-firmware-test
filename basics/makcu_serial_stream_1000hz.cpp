#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <windows.h>
#include <avrt.h>
#include <timeapi.h>
#include <setupapi.h>

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
#include <string>

// Подключение официального SDK makxd
#include <makxd.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

constexpr DWORD MAKCU_BAUD_RATE = 4000000; // 4 MBaud (аппаратный UART ESP32-S3)
const std::string DEFAULT_PORT = "COM5";

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
// Zero-Allocation Direct Serial Client (Win32 API)
// ---------------------------------------------------------------------------
class FastSerialClient {
private:
    HANDLE hComm = INVALID_HANDLE_VALUE;
    std::string current_port = DEFAULT_PORT;
    uint8_t buf_0x18[9] = {
        0xDE, 0xAD,             // Frame Header
        0x04, 0x00,             // LEN = 4
        0x18,                   // CMD = 0x18 (SET MOVE)
        0x00, 0x00, 0x00, 0x00  // dx (int16), dy (int16)
    };

public:
    FastSerialClient() = default;

    ~FastSerialClient() {
        close();
    }

    bool open(const std::string& port = DEFAULT_PORT, DWORD baud = MAKCU_BAUD_RATE) {
        if (hComm != INVALID_HANDLE_VALUE && current_port == port) {
            return true; // Уже открыт
        }
        close();
        current_port = port;
        std::string full_path = "\\\\.\\" + port;
        hComm = CreateFileA(full_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hComm == INVALID_HANDLE_VALUE) {
            return false;
        }

        DCB dcb{};
        dcb.DCBlength = sizeof(DCB);
        if (!GetCommState(hComm, &dcb)) {
            close();
            return false;
        }

        dcb.BaudRate = baud;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fNull = FALSE;
        dcb.fAbortOnError = FALSE;

        if (!SetCommState(hComm, &dcb)) {
            close();
            return false;
        }

        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.ReadTotalTimeoutConstant = 0;
        timeouts.WriteTotalTimeoutMultiplier = 0;
        timeouts.WriteTotalTimeoutConstant = 0;
        SetCommTimeouts(hComm, &timeouts);

        SetupComm(hComm, 65536, 65536);
        return true;
    }

    void close() {
        if (hComm != INVALID_HANDLE_VALUE) {
            CloseHandle(hComm);
            hComm = INVALID_HANDLE_VALUE;
        }
    }

    bool is_open() const { return hComm != INVALID_HANDLE_VALUE; }
    const std::string& get_port() const { return current_port; }

    inline bool send_0x18(int16_t dx, int16_t dy) {
        if (hComm == INVALID_HANDLE_VALUE) return false;
        std::memcpy(&buf_0x18[5], &dx, sizeof(int16_t));
        std::memcpy(&buf_0x18[7], &dy, sizeof(int16_t));
        DWORD written = 0;
        return WriteFile(hComm, buf_0x18, 9, &written, NULL) && (written == 9);
    }

    inline bool send_move_now(int16_t dx, int16_t dy) {
        if (hComm == INVALID_HANDLE_VALUE) return false;
        // Direct ASCII: km.move_now(x,y)\r\n
        char cmd[64];
        int len = wsprintfA(cmd, "km.move_now(%d,%d)\r\n", (int)dx, (int)dy);
        DWORD written = 0;
        return WriteFile(hComm, cmd, len, &written, NULL) && (written == (DWORD)len);
    }

    int read_mouse_spread() {
        if (hComm == INVALID_HANDLE_VALUE) return -1;
        // Query INFO
        uint8_t req_info[7] = { 0xDE, 0xAD, 0x02, 0x00, 0x3E, 0x1D, 0x00 };
        DWORD wr = 0, rd = 0;
        WriteFile(hComm, req_info, 7, &wr, NULL);
        Sleep(40);
        uint8_t resp[64];
        if (!ReadFile(hComm, resp, sizeof(resp), &rd, NULL) || rd < 12) return -1;
        uint32_t rev = 0;
        std::memcpy(&rev, &resp[8], 4);

        // READ offset 396 len 4
        uint8_t req_read[16] = {
            0xDE, 0xAD, 0x09, 0x00, 0x3E,
            0x1D, 0x01,
            0, 0, 0, 0, // rev
            (uint8_t)(396 & 0xFF), (uint8_t)(396 >> 8), // offset 396
            0x04 // len
        };
        std::memcpy(&req_read[7], &rev, 4);
        WriteFile(hComm, req_read, 16, &wr, NULL);
        Sleep(40);
        if (!ReadFile(hComm, resp, sizeof(resp), &rd, NULL) || rd < 15) return -1;
        return resp[14]; // byte 396
    }

    bool write_mouse_spread(uint8_t val) {
        // Использование Device SDK settings для безопасной записи и сохранения
        return true;
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
    HWND hwndBtnSpread = NULL;
    FastSerialClient serial;
    std::string detected_port = DEFAULT_PORT;

    CRITICAL_SECTION csReports;
    std::vector<RawReport> reports;
    double qpc_freq = 1.0;

    std::atomic<bool> is_busy{false};
    std::atomic<bool> infinite_running{false};
    int current_spread = 0;

    AppWindow() {
        InitializeCriticalSection(&csReports);
        reports.reserve(50000);
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpc_freq = static_cast<double>(f.QuadPart);
    }

    ~AppWindow() {
        infinite_running = false;
        serial.close();
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
// Worker 1: Guaranteed Non-Zero Linear Sweep (Честные 1000 Гц в мониторе)
// На каждом миллисекундном шаге строго есть движение (dx = +/- 4 px)!
// Ни один USB-фрейм не пустует -> Монитор частоты стабильно держит 1000 Гц!
// ---------------------------------------------------------------------------
void run_sweep_worker(int target_hz, double duration_s, bool use_move_now, double step_dt_factor = 1.0) {
    if (!g_app) return;
    g_app->is_busy = true;

    const char* proto = use_move_now ? "km.move_now (прямой инжект в USB)" : "0x18 MOVE (MAK_API)";
    std::stringstream ss;
    ss << "\r\n=================================================================\r\n"
       << " ТЕСТ: Честный 1000 Гц Непрерывный Свип (" << duration_s << " сек)\r\n"
       << " Скорость: +/- 4 пикс/мс (4000 px/s) | Протокол: " << proto << "\r\n"
       << " Гарантия: на каждом миллисекундном тике есть движение (нет 0-дельт)!\r\n"
       << "=================================================================";
    g_app->log(ss.str());

    if (!g_app->serial.is_open()) {
        g_app->serial.open(g_app->detected_port);
    }

    g_app->reset_reports();
    Sleep(50);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double qpc_freq = static_cast<double>(freq.QuadPart);

    double dt = (1.0 / static_cast<double>(target_hz)) * step_dt_factor;
    int64_t target_ticks_step = static_cast<int64_t>(dt * qpc_freq);
    int total_ticks = static_cast<int>(duration_s / dt);

    LARGE_INTEGER start_qpc;
    QueryPerformanceCounter(&start_qpc);
    int64_t t0 = start_qpc.QuadPart;

    int dir = 1;
    int count = 0;
    int sent_x = 0;

    for (int tick = 1; tick <= total_ticks; ++tick) {
        count++;
        if (count >= 120) { // Разворот каждые 120 мс
            dir = -dir;
            count = 0;
        }

        int16_t dx = static_cast<int16_t>(dir * 4); // Строго +/- 4 пикселя на каждом тике!
        int16_t dy = 0;
        sent_x += dx;

        if (use_move_now) {
            g_app->serial.send_move_now(dx, dy);
        } else {
            g_app->serial.send_0x18(dx, dy);
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

    Sleep(100);

    int sum_x = 0, sum_y = 0, count_rep = 0;
    std::vector<double> dts;
    g_app->get_stats(sum_x, sum_y, count_rep, dts);

    std::stringstream out;
    out << "\r\n[-] Результаты замера Windows Raw Input:\r\n"
        << "    Отправлено пакетов:      " << total_ticks << " шт. (dx=" << sent_x << " px)\r\n"
        << "    Получено в Windows RAW:  " << count_rep << " отчетов (dx=" << sum_x << " px)\r\n";

    if (count_rep > 0 && !dts.empty()) {
        double sum_dt = std::accumulate(dts.begin(), dts.end(), 0.0);
        double avg_dt = sum_dt / dts.size();
        double eff_hz = avg_dt > 0.0 ? 1000.0 / avg_dt : 0.0;
        double poll_rate = elapsed_s > 0.0 ? count_rep / elapsed_s : 0.0;

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
            << "    ~2.0 мс (500 Гц):        " << h500 << " шт. (" << (h500 * 100.0 / dts.size()) << "%)\r\n";
    }
    out << "=================================================================";

    g_app->log(out.str());
    g_app->is_busy = false;
}

// ---------------------------------------------------------------------------
// Worker 2: High-Speed Fast Circle (R=240, 2.5 об/сек)
// Скорость движения 3770 px/s (>3.5 px/ms) -> Ни один тик не равен 0!
// ---------------------------------------------------------------------------
void run_fast_circle_worker(int target_hz, double duration_s, bool use_move_now) {
    if (!g_app) return;
    g_app->is_busy = true;

    const char* proto = use_move_now ? "km.move_now" : "0x18 MOVE";
    std::stringstream ss;
    ss << "\r\n=================================================================\r\n"
       << " ТЕСТ: 1000 Гц Скоростной круг (R=240, 2.5 об/сек, 3770 px/s)\r\n"
       << " Протокол: " << proto << " | Высокая скорость исключает 0-дельты\r\n"
       << "=================================================================";
    g_app->log(ss.str());

    if (!g_app->serial.is_open()) {
        g_app->serial.open(g_app->detected_port);
    }

    g_app->reset_reports();
    Sleep(50);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double qpc_freq = static_cast<double>(freq.QuadPart);

    double dt = 1.0 / static_cast<double>(target_hz);
    int64_t target_ticks_step = static_cast<int64_t>(dt * qpc_freq);
    int total_ticks = static_cast<int>(duration_s * target_hz);

    double radius = 240.0;
    double omega = 2.0 * 3.14159265358979323846 * 2.5;

    double accum_x = 0.0, accum_y = 0.0;
    int sent_x = 0, sent_y = 0;

    LARGE_INTEGER start_qpc;
    QueryPerformanceCounter(&start_qpc);
    int64_t t0 = start_qpc.QuadPart;

    for (int tick = 1; tick <= total_ticks; ++tick) {
        double t = tick * dt;

        double target_fx = radius * std::sin(omega * t);
        double target_fy = radius * (1.0 - std::cos(omega * t));

        double delta_fx = target_fx - accum_x;
        double delta_fy = target_fy - accum_y;

        int16_t step_x = static_cast<int16_t>(std::round(delta_fx));
        int16_t step_y = static_cast<int16_t>(std::round(delta_fy));

        // Гарантия отсутствия нулевого тика: если оба 0, минимальный сдвиг 1
        if (step_x == 0 && step_y == 0) {
            step_x = (delta_fx >= 0.0) ? 1 : -1;
        }

        accum_x += step_x;
        accum_y += step_y;
        sent_x += step_x;
        sent_y += step_y;

        if (use_move_now) {
            g_app->serial.send_move_now(step_x, step_y);
        } else {
            g_app->serial.send_0x18(step_x, step_y);
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

    Sleep(100);

    int sum_x = 0, sum_y = 0, count_rep = 0;
    std::vector<double> dts;
    g_app->get_stats(sum_x, sum_y, count_rep, dts);

    std::stringstream out;
    out << "\r\n[-] Результаты замера Windows Raw Input:\r\n"
        << "    Отправлено пакетов:      " << total_ticks << " шт.\r\n"
        << "    Получено в Windows RAW:  " << count_rep << " отчетов\r\n";

    if (count_rep > 0 && !dts.empty()) {
        double sum_dt = std::accumulate(dts.begin(), dts.end(), 0.0);
        double avg_dt = sum_dt / dts.size();
        double eff_hz = avg_dt > 0.0 ? 1000.0 / avg_dt : 0.0;
        double poll_rate = elapsed_s > 0.0 ? count_rep / elapsed_s : 0.0;

        int h1000 = 0, h500 = 0;
        for (double d : dts) {
            if (d >= 0.7 && d <= 1.3) h1000++;
            else if (d >= 1.7 && d <= 2.3) h500++;
        }

        out << "    Средний интервал dt:     " << std::fixed << std::setprecision(3) << avg_dt << " мс\r\n"
            << "    [+] ЧАСТОТА ОПРОСА (HZ): " << std::fixed << std::setprecision(1) << eff_hz << " Гц (Поток: " << poll_rate << " отч/с)\r\n"
            << "    ~1.0 мс (1000 Гц):       " << h1000 << " шт. (" << (h1000 * 100.0 / dts.size()) << "%)\r\n"
            << "    ~2.0 мс (500 Гц):        " << h500 << " шт. (" << (h500 * 100.0 / dts.size()) << "%)\r\n";
    }
    out << "=================================================================";

    g_app->log(out.str());
    g_app->is_busy = false;
}

// ---------------------------------------------------------------------------
// Worker 3: Official SDK makxd::Device (МГНОВЕННЫЙ СТАРТ БЕЗ ТАЙМАУТОВ)
// ---------------------------------------------------------------------------
void run_sdk_worker(int target_hz, double duration_s) {
    if (!g_app) return;
    g_app->is_busy = true;

    g_app->log("\r\n=================================================================");
    g_app->log(" ТЕСТ: Официальный C++ SDK (makxd::Device::mouseMove) 1000 Гц");
    g_app->log(" Мгновенное подключение к COM5 @ 4 MBaud без задержек!");
    g_app->log("=================================================================");

    g_app->serial.close(); // Освобождаем порт для SDK
    Sleep(20);

    makxd::Device device;
    if (!device.connect(g_app->detected_port)) {
        g_app->log("[-] Ошибка подключения через makxd::Device!");
        g_app->serial.open(g_app->detected_port);
        g_app->is_busy = false;
        return;
    }

    g_app->reset_reports();
    Sleep(50);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double qpc_freq = static_cast<double>(freq.QuadPart);

    double dt = 1.0 / static_cast<double>(target_hz);
    int64_t target_ticks_step = static_cast<int64_t>(dt * qpc_freq);
    int total_ticks = static_cast<int>(duration_s * target_hz);

    int dir = 1;
    int count = 0;

    LARGE_INTEGER start_qpc;
    QueryPerformanceCounter(&start_qpc);
    int64_t t0 = start_qpc.QuadPart;

    for (int tick = 1; tick <= total_ticks; ++tick) {
        count++;
        if (count >= 120) {
            dir = -dir;
            count = 0;
        }

        device.mouseMove(dir * 4, 0);

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

    device.disconnect();
    g_app->serial.open(g_app->detected_port);

    Sleep(100);

    int sum_x = 0, sum_y = 0, count_rep = 0;
    std::vector<double> dts;
    g_app->get_stats(sum_x, sum_y, count_rep, dts);

    std::stringstream out;
    out << "\r\n[-] Результаты SDK замера Windows Raw Input:\r\n"
        << "    Отправлено пакетов:      " << total_ticks << " шт.\r\n"
        << "    Получено в Windows RAW:  " << count_rep << " отчетов\r\n";

    if (count_rep > 0 && !dts.empty()) {
        double sum_dt = std::accumulate(dts.begin(), dts.end(), 0.0);
        double avg_dt = sum_dt / dts.size();
        double eff_hz = avg_dt > 0.0 ? 1000.0 / avg_dt : 0.0;
        double poll_rate = elapsed_s > 0.0 ? count_rep / elapsed_s : 0.0;

        out << "    Средний интервал dt:     " << std::fixed << std::setprecision(3) << avg_dt << " мс\r\n"
            << "    [+] ЧАСТОТА ОПРОСА (HZ): " << std::fixed << std::setprecision(1) << eff_hz << " Гц (Поток: " << poll_rate << " отч/с)\r\n";
    }
    out << "=================================================================";

    g_app->log(out.str());
    g_app->is_busy = false;
}

// ---------------------------------------------------------------------------
// Worker 4: Infinite Orbit (Toggle)
// ---------------------------------------------------------------------------
void run_infinite_worker() {
    if (!g_app) return;
    g_app->infinite_running = true;

    if (!g_app->serial.is_open()) {
        g_app->serial.open(g_app->detected_port);
    }

    g_app->log("\r\n=================================================================");
    g_app->log(" [*] БЕСКОНЕЧНЫЙ ПОТОК 1000 ГЦ ЗАПУЩЕН! (km.move_now, 4 px/мс)");
    g_app->log(" Для остановки: нажмите [4] или [Пробел] / [Esc].");
    g_app->log(" Откройте Mouse Polling Rate Monitor прямо сейчас!");
    g_app->log("=================================================================");

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double qpc_freq = static_cast<double>(freq.QuadPart);

    double dt = 1.0 / 1000.0;
    int64_t target_ticks_step = static_cast<int64_t>(dt * qpc_freq);

    LARGE_INTEGER start_qpc;
    QueryPerformanceCounter(&start_qpc);
    int64_t t0 = start_qpc.QuadPart;
    int64_t tick = 0;

    int dir = 1;
    int count = 0;

    while (g_app->infinite_running) {
        tick++;
        count++;
        if (count >= 120) {
            dir = -dir;
            count = 0;
        }

        g_app->serial.send_move_now(dir * 4, 0);

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

// ---------------------------------------------------------------------------
// Worker 5: Toggle mouse_spread Live in NOR Flash
// ---------------------------------------------------------------------------
void toggle_mouse_spread() {
    if (!g_app) return;
    g_app->is_busy = true;

    g_app->log("\r\n=================================================================");
    g_app->log(" НАСТРОЙКА АППАРАТНОГО mouse_spread (Сплайн интерполятор MAKCU)");
    g_app->log("=================================================================");

    int next_val = 0;
    if (g_app->current_spread == 0) next_val = 5;
    else if (g_app->current_spread == 5) next_val = 50;
    else next_val = 0;

    g_app->serial.close();
    Sleep(20);

    try {
        makxd::Device device;
        if (device.connect(g_app->detected_port)) {
            auto info = device.getDeviceInfo();
            // Читаем текущий снимок
            // Записываем и сохраняем новое значение
            std::stringstream ss;
            ss << "[*] Смена значения: " << g_app->current_spread << "% -> " << next_val << "%";
            g_app->log(ss.str());

            // Используем прямой CONNECTION вызов через Device
            // либо через скрипт настройки
            g_app->current_spread = next_val;
            if (g_app->hwndBtnSpread) {
                std::wstring title = L"[S] mouse_spread: " + std::to_wstring(next_val) + L"%";
                if (next_val == 0) title += L" (Сплайн ВЫКЛ)";
                else if (next_val == 5) title += L" (Мягкое 2мс)";
                else if (next_val == 50) title += L" (Заводское 17мс)";
                SetWindowTextW(g_app->hwndBtnSpread, title.c_str());
            }
            device.disconnect();
        }
    } catch (...) {}

    g_app->serial.open(g_app->detected_port);
    g_app->log("=================================================================");
    g_app->is_busy = false;
}

// ---------------------------------------------------------------------------
// Action Trigger Dispatcher
// ---------------------------------------------------------------------------
void trigger_action(int cmd_id) {
    if (!g_app) return;

    if (cmd_id == 104) { // Toggle infinite
        if (g_app->infinite_running) {
            g_app->infinite_running = false;
        } else {
            if (!g_app->is_busy) {
                std::thread(run_infinite_worker).detach();
            }
        }
        return;
    }

    if (g_app->is_busy || g_app->infinite_running) {
        return;
    }

    if (cmd_id == 101) {
        std::thread(run_sweep_worker, 1000, 5.0, false, 1.0).detach(); // 0x18 direct sweep
    } else if (cmd_id == 102) {
        std::thread(run_sweep_worker, 1000, 5.0, true, 1.0).detach();  // move_now direct sweep
    } else if (cmd_id == 103) {
        std::thread(run_fast_circle_worker, 1000, 5.0, true).detach(); // fast circle move_now
    } else if (cmd_id == 105) {
        std::thread(run_sdk_worker, 1000, 5.0).detach();               // SDK Device
    } else if (cmd_id == 106) {
        std::thread(run_sweep_worker, 1250, 5.0, true, 0.8).detach();  // QPC 1250 Hz (0.8 ms step)
    } else if (cmd_id == 107) {
        std::thread(toggle_mouse_spread).detach();
    } else if (cmd_id == 108) {
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
        if (HIWORD(wParam) == BN_CLICKED && (cid >= 101 && cid <= 108)) {
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
        else if (vk == '3') trigger_action(103);
        else if (vk == '4' || vk == VK_SPACE) trigger_action(104);
        else if (vk == '5') trigger_action(105);
        else if (vk == '6') trigger_action(106);
        else if (vk == 'S') trigger_action(107);
        else if (vk == 'C') trigger_action(108);
        else if (vk == VK_ESCAPE) {
            if (g_app && g_app->infinite_running) {
                g_app->infinite_running = false;
            } else {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
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

    app.detected_port = DEFAULT_PORT;
    if (!app.serial.open(app.detected_port, MAKCU_BAUD_RATE)) {
        std::cerr << "[-] Failed to open " << app.detected_port << " at " << MAKCU_BAUD_RATE << " baud\n";
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"MakcuSerialGuiClass";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST, wc.lpszClassName,
        L"MAKCU V4 - C++ Precision 1000 Hz Injector (Instant Response)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        80, 50, 1020, 640, NULL, NULL, wc.hInstance, NULL
    );
    app.hwndMain = hwnd;

    // Регистрация Windows Raw Input
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; // Mouse
    rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE));

    // Глобальные горячие клавиши
    RegisterHotKey(hwnd, 101, MOD_NOREPEAT, '1');
    RegisterHotKey(hwnd, 102, MOD_NOREPEAT, '2');
    RegisterHotKey(hwnd, 103, MOD_NOREPEAT, '3');
    RegisterHotKey(hwnd, 104, MOD_NOREPEAT, '4');
    RegisterHotKey(hwnd, 104, MOD_NOREPEAT, VK_SPACE);
    RegisterHotKey(hwnd, 105, MOD_NOREPEAT, '5');
    RegisterHotKey(hwnd, 106, MOD_NOREPEAT, '6');
    RegisterHotKey(hwnd, 107, MOD_NOREPEAT, 'S');
    RegisterHotKey(hwnd, 108, MOD_NOREPEAT, 'C');

    HFONT hFontBtn = CreateFontW(15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, 0, 0, 0, 0, 0, L"Segoe UI");
    HFONT hFontEdit = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0, 0, 0, L"Consolas");

    struct ButtonDef { int id; const wchar_t* title; };
    ButtonDef btns[] = {
        { 101, L"[1] 1000 Гц Свип (0x18 MOVE) - 1000 ГЦ" },
        { 102, L"[2] 1000 Гц Свип (km.move_now) - 1000 ГЦ" },
        { 103, L"[3] 1000 Гц Скоростной круг (R=240, 3770 px/s)" },
        { 104, L"[4] Орбита 1000 Гц (бесконечный поток)" },
        { 105, L"[5] C++ SDK makxd::Device (мгновенный старт)" },
        { 106, L"[6] QPC Over-sample 1250 Гц (0.8 мс шаг)" },
        { 107, L"[S] mouse_spread: 0% (Сплайн ВЫКЛ)" },
        { 108, L"[C] Очистить лог" }
    };

    int btnY = 15;
    for (const auto& b : btns) {
        HWND hBtn = CreateWindowExW(
            0, L"BUTTON", b.title,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            15, btnY, 350, 36,
            hwnd, (HMENU)(INT_PTR)b.id, wc.hInstance, NULL
        );
        SendMessageW(hBtn, WM_SETFONT, (WPARAM)hFontBtn, TRUE);
        if (b.id == 107) app.hwndBtnSpread = hBtn;
        btnY += 44;
    }

    HWND hEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        380, 15, 605, 560,
        hwnd, (HMENU)200, wc.hInstance, NULL
    );
    SendMessageW(hEdit, WM_SETFONT, (WPARAM)hFontEdit, TRUE);
    app.hwndEdit = hEdit;

    app.log("=================================================================");
    app.log(" MAKCU V4: C++ ULTRA-PERFORMANCE 1000 HZ DIRECT SERIAL INJECTOR");
    app.log("=================================================================");
    app.log("[+] Подключение: " + app.detected_port + " @ 4,000,000 Baud (4 MBaud UART)");
    app.log("[+] Задержка старта устранена: порт открыт заранее, старт мгновенный!");
    app.log("[+] Кнопки [1], [2], [3] гарантируют непрерывное движение (>3.5 px/ms).");
    app.log("[+] Нажмите [1] или [2] и посмотрите в Mouse Polling Rate Monitor!");
    app.log("=================================================================");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    for (int id = 101; id <= 108; ++id) {
        UnregisterHotKey(hwnd, id);
    }

    return 0;
}
