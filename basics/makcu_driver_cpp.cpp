#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <iostream>
#include <cmath>
#include <cstdint>
#include <conio.h>
#include <makxd.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "bcrypt.lib")

constexpr double PI = 3.14159265358979323846;
const char* PORT_NAME = "\\\\.\\COM5";
constexpr DWORD MAKCU_BAUD = 4000000;

// Режимы транспорта
enum class TransportMode {
    MAKXD_SDK,      // Официальный C++ SDK (makxd::Device::mouseMove)
    DIRECT_SERIAL   // Прямой Win32 WriteFile опкод 0x18 (минимальный оверхед)
};

// Открытие прямого COM-порта
HANDLE open_direct_com() {
    HANDLE h = CreateFileA(PORT_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    GetCommState(h, &dcb);
    dcb.BaudRate = MAKCU_BAUD;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    SetCommState(h, &dcb);

    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    SetCommTimeouts(h, &timeouts);
    SetupComm(h, 65536, 65536);
    return h;
}

// Отправка бинарного опкода 0x18 (MOVE: x:i16, y:i16)
inline void send_raw_0x18(HANDLE hComm, int dx, int dy) {
    uint8_t pkt[9] = { 0xDE, 0xAD, 0x04, 0x00, 0x18, 0, 0, 0, 0 };
    int16_t x = static_cast<int16_t>(dx);
    int16_t y = static_cast<int16_t>(dy);
    pkt[5] = static_cast<uint8_t>(x & 0xFF);
    pkt[6] = static_cast<uint8_t>((x >> 8) & 0xFF);
    pkt[7] = static_cast<uint8_t>(y & 0xFF);
    pkt[8] = static_cast<uint8_t>((y >> 8) & 0xFF);
    DWORD wr = 0;
    WriteFile(hComm, pkt, 9, &wr, NULL);
}

// ---------------------------------------------------------------------------
// Траектория 1: Непрерывный плавный круг (Smooth Circle Orbit)
// Скорость V = 2 * PI * R * revs / 1000 = ~ 5 px/мс.
// Ни одного нулевого отчета!
// ---------------------------------------------------------------------------
void run_circle(TransportMode mode, makxd::Device* pDev, HANDLE hComm, double dt_sec, double duration_s = 5.0, double radius = 320.0, double revs_per_sec = 2.5) {
    LARGE_INTEGER freq, t0, cur;
    QueryPerformanceFrequency(&freq);
    double qpc_freq = static_cast<double>(freq.QuadPart);
    int64_t step_ticks = static_cast<int64_t>(dt_sec * qpc_freq);
    int total_ticks = static_cast<int>(std::round(duration_s / dt_sec));

    int total_revs = static_cast<int>(std::round(revs_per_sec * duration_s));
    double d_theta = (2.0 * PI * total_revs) / static_cast<double>(total_ticks);

    std::cout << "\n[>>>] СТАРТ: Круг R=" << radius << " | dt=" << (dt_sec * 1000.0) << " ms (" 
              << static_cast<int>(std::round(1.0 / dt_sec)) << " Hz) | Транспорт: " 
              << (mode == TransportMode::MAKXD_SDK ? "makxd SDK" : "Direct Win32 0x18") << "\n";
    MessageBeep(MB_OK);

    double theta = 0.0;
    double prev_ideal_x = radius;
    double prev_ideal_y = 0.0;
    double acc_x = 0.0;
    double acc_y = 0.0;

    QueryPerformanceCounter(&t0);
    int64_t next_tick = t0.QuadPart;

    int total_emitted = 0;
    int zero_emitted = 0;

    for (int i = 1; i <= total_ticks; ++i) {
        theta += d_theta;
        double cur_x = radius * std::cos(theta);
        double cur_y = radius * std::sin(theta);

        double step_x = cur_x - prev_ideal_x;
        double step_y = cur_y - prev_ideal_y;
        prev_ideal_x = cur_x;
        prev_ideal_y = cur_y;

        acc_x += step_x;
        acc_y += step_y;
        int out_x = static_cast<int>(std::round(acc_x));
        int out_y = static_cast<int>(std::round(acc_y));
        acc_x -= out_x;
        acc_y -= out_y;

        if (out_x == 0 && out_y == 0) {
            zero_emitted++;
        }

        if (mode == TransportMode::MAKXD_SDK && pDev) {
            (void)pDev->mouseMove(out_x, out_y);
        } else if (hComm != INVALID_HANDLE_VALUE) {
            send_raw_0x18(hComm, out_x, out_y);
        }
        total_emitted++;

        next_tick += step_ticks;
        while (true) {
            QueryPerformanceCounter(&cur);
            if (cur.QuadPart >= next_tick) break;
            YieldProcessor();
        }
    }

    MessageBeep(MB_ICONASTERISK);
    std::cout << "[+] Завершено: отправлено " << total_emitted << " отчетов, нулей: " << zero_emitted << "\n";
}

// ---------------------------------------------------------------------------
// Траектория 2: Хаотичный свайп по всему экрану (Chaotic Fourier Sweep)
// ---------------------------------------------------------------------------
void run_chaotic(TransportMode mode, makxd::Device* pDev, HANDLE hComm, double dt_sec, double duration_s = 5.0, double amp_x = 500.0, double amp_y = 350.0) {
    LARGE_INTEGER freq, t0, cur;
    QueryPerformanceFrequency(&freq);
    double qpc_freq = static_cast<double>(freq.QuadPart);
    int64_t step_ticks = static_cast<int64_t>(dt_sec * qpc_freq);
    int total_ticks = static_cast<int>(std::round(duration_s / dt_sec));

    constexpr double kx1 = 6.0, kx2 = 11.0, kx3 = 17.0;
    constexpr double ky1 = 7.0, ky2 = 13.0, ky3 = 19.0;

    std::cout << "\n[>>>] СТАРТ: Хаос свайп Ax=" << amp_x << ", Ay=" << amp_y << " | dt=" << (dt_sec * 1000.0) << " ms (" 
              << static_cast<int>(std::round(1.0 / dt_sec)) << " Hz) | Транспорт: " 
              << (mode == TransportMode::MAKXD_SDK ? "makxd SDK" : "Direct Win32 0x18") << "\n";
    MessageBeep(MB_OK);

    double prev_ideal_x = 0.0;
    double prev_ideal_y = 0.0;
    double acc_x = 0.0;
    double acc_y = 0.0;

    QueryPerformanceCounter(&t0);
    int64_t next_tick = t0.QuadPart;

    int total_emitted = 0;
    int zero_emitted = 0;

    for (int i = 1; i <= total_ticks; ++i) {
        double tau = static_cast<double>(i) / static_cast<double>(total_ticks);
        double cur_x = amp_x * (0.60 * std::sin(2.0 * PI * kx1 * tau) +
                                0.30 * std::sin(2.0 * PI * kx2 * tau) +
                                0.10 * std::sin(2.0 * PI * kx3 * tau));
        double cur_y = amp_y * (0.60 * std::sin(2.0 * PI * ky1 * tau) +
                                0.30 * std::sin(2.0 * PI * ky2 * tau) +
                                0.10 * std::sin(2.0 * PI * ky3 * tau));

        double step_x = cur_x - prev_ideal_x;
        double step_y = cur_y - prev_ideal_y;
        prev_ideal_x = cur_x;
        prev_ideal_y = cur_y;

        acc_x += step_x;
        acc_y += step_y;
        int out_x = static_cast<int>(std::round(acc_x));
        int out_y = static_cast<int>(std::round(acc_y));
        acc_x -= out_x;
        acc_y -= out_y;

        if (out_x == 0 && out_y == 0) {
            zero_emitted++;
        }

        if (mode == TransportMode::MAKXD_SDK && pDev) {
            (void)pDev->mouseMove(out_x, out_y);
        } else if (hComm != INVALID_HANDLE_VALUE) {
            send_raw_0x18(hComm, out_x, out_y);
        }
        total_emitted++;

        next_tick += step_ticks;
        while (true) {
            QueryPerformanceCounter(&cur);
            if (cur.QuadPart >= next_tick) break;
            YieldProcessor();
        }
    }

    MessageBeep(MB_ICONASTERISK);
    std::cout << "[+] Завершено: отправлено " << total_emitted << " отчетов, нулей: " << zero_emitted << "\n";
}

int main() {
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    std::cout << "=================================================================\n";
    std::cout << "   MAKCU V4 PRECISION C++ DRIVER (1000 HZ / COM5 / RAW INPUT)\n";
    std::cout << "=================================================================\n";

    TransportMode currentMode = TransportMode::MAKXD_SDK;
    makxd::Device device;
    HANDLE hDirect = INVALID_HANDLE_VALUE;

    std::cout << "[*] Подключение через официальный C++ SDK (makxd)... ";
    if (device.connect("COM5")) {
        std::cout << "OK!\n";
        auto ver = device.firmwareVersion();
        if (ver) {
            std::cout << "[*] Прошивка MAKCU: V" << (*ver / 1000.0) << " (код " << *ver << ")\n";
        }
    } else {
        std::cout << "[-] Ошибка подключения через SDK! Переключение на Direct Win32 Serial...\n";
        currentMode = TransportMode::DIRECT_SERIAL;
        hDirect = open_direct_com();
        if (hDirect == INVALID_HANDLE_VALUE) {
            std::cout << "[-] Не удалось открыть COM5!\n";
            return 1;
        }
        std::cout << "[+] Direct Win32 COM5 открыт успешно @ 4,000,000 Baud!\n";
    }

    std::cout << "\n[*] Горячие клавиши (работают глобально при активном Mouse Monitor):\n";
    std::cout << "  [F1] Круг 1000 Гц QPC (dt = 1.00 мс)  | Референс 1000 Гц\n";
    std::cout << "  [F2] Круг 1250 Гц QPC (dt = 0.80 мс)  | Nano sub-1ms (устраняет фазовый сдвиг USB)\n";
    std::cout << "  [F3] Круг 1500 Гц QPC (dt = 0.66 мс)  | Nano sub-1ms оверсэмплинг\n";
    std::cout << "  [F4] Хаос свайп 1000 Гц (dt = 1.00 мс)| Полный экран 500x350 px\n";
    std::cout << "  [F5] Хаос свайп 1250 Гц (dt = 0.80 мс)| Nano sub-1ms\n";
    std::cout << "  [Пробел] Быстрый тест 3 сек           | Круг 1000 Гц\n";
    std::cout << "  [T] Сменить транспорт                 | makxd SDK <-> Direct Win32\n";
    std::cout << "  [Esc / 0] Выход\n";
    std::cout << "=================================================================\n";
    std::cout << "Текущий транспорт: " << (currentMode == TransportMode::MAKXD_SDK ? "[makxd SDK]" : "[Direct Win32 0x18]") << "\n";
    std::cout << ">>> Откройте окно 'Mouse Polling Rate Monitor v1.0' и нажмите F1..F5! <<<\n\n";

    while (true) {
        int act = 0;
        if (_kbhit()) {
            char ch = _getch();
            if (ch == '1') act = 1;
            else if (ch == '2') act = 2;
            else if (ch == '3') act = 3;
            else if (ch == '4') act = 4;
            else if (ch == '5') act = 5;
            else if (ch == ' ') act = 10;
            else if (ch == 't' || ch == 'T') act = 99;
            else if (ch == 27 || ch == '0' || ch == 'q') break;
        }
        else if (GetAsyncKeyState(VK_F1) & 0x8000) { act = 1; Sleep(250); }
        else if (GetAsyncKeyState(VK_F2) & 0x8000) { act = 2; Sleep(250); }
        else if (GetAsyncKeyState(VK_F3) & 0x8000) { act = 3; Sleep(250); }
        else if (GetAsyncKeyState(VK_F4) & 0x8000) { act = 4; Sleep(250); }
        else if (GetAsyncKeyState(VK_F5) & 0x8000) { act = 5; Sleep(250); }
        else if (GetAsyncKeyState(VK_SPACE) & 0x8000) { act = 10; Sleep(250); }
        else if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { break; }

        if (act == 99) {
            if (currentMode == TransportMode::MAKXD_SDK) {
                device.disconnect();
                hDirect = open_direct_com();
                currentMode = TransportMode::DIRECT_SERIAL;
                std::cout << "[*] Переключено на: Direct Win32 Serial (0x18)\n";
            } else {
                if (hDirect != INVALID_HANDLE_VALUE) {
                    CloseHandle(hDirect);
                    hDirect = INVALID_HANDLE_VALUE;
                }
                (void)device.connect("COM5");
                currentMode = TransportMode::MAKXD_SDK;
                std::cout << "[*] Переключено на: makxd C++ SDK\n";
            }
        }
        else if (act == 1) {
            run_circle(currentMode, &device, hDirect, 0.0010, 5.0, 320.0, 2.5); // 1000 Hz
        }
        else if (act == 2) {
            run_circle(currentMode, &device, hDirect, 0.0008, 5.0, 320.0, 2.5); // 1250 Hz (Nano sub-1ms)
        }
        else if (act == 3) {
            run_circle(currentMode, &device, hDirect, 0.000666, 5.0, 320.0, 2.5); // 1500 Hz
        }
        else if (act == 4) {
            run_chaotic(currentMode, &device, hDirect, 0.0010, 5.0, 500.0, 350.0); // 1000 Hz
        }
        else if (act == 5) {
            run_chaotic(currentMode, &device, hDirect, 0.0008, 5.0, 500.0, 350.0); // 1250 Hz
        }
        else if (act == 10) {
            run_circle(currentMode, &device, hDirect, 0.0010, 3.0, 260.0, 2.0); // 3 сек
        }

        Sleep(10);
    }

    if (currentMode == TransportMode::MAKXD_SDK) {
        device.disconnect();
    } else if (hDirect != INVALID_HANDLE_VALUE) {
        CloseHandle(hDirect);
    }

    timeEndPeriod(1);
    std::cout << "[*] Завершение работы.\n";
    return 0;
}
