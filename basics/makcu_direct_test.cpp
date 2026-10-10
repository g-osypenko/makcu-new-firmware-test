#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <iostream>
#include <cstdint>
#include <string>
#include <conio.h>
#include <makxd.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "bcrypt.lib")

// ---------------------------------------------------------------------------
// Конфигурация MAKCU V4
// ---------------------------------------------------------------------------
constexpr DWORD MAKCU_BAUD = 4000000; // 4 MBaud UART
const char* PORT_NAME = "\\\\.\\COM5";

HANDLE open_com_port() {
    HANDLE h = CreateFileA(PORT_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        std::cerr << "[-] Не удалось открыть COM5! Проверьте подключение платы.\n";
        return INVALID_HANDLE_VALUE;
    }

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

// ---------------------------------------------------------------------------
// Прямой стриминг движения без оверинжиниринга
// ---------------------------------------------------------------------------
void run_stream(HANDLE hComm, double dt_sec, int duration_sec, bool use_move_now, int step_px = 4) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double qpc_freq = static_cast<double>(freq.QuadPart);
    int64_t step_ticks = static_cast<int64_t>(dt_sec * qpc_freq);
    int total_packets = static_cast<int>(duration_sec / dt_sec);

    std::cout << "\n>>> СТАРТ СТРИМА (" << duration_sec << " сек, dt = " << (dt_sec * 1000.0) << " мс, шаг = " << step_px << " px) <<<\n";
    std::cout << ">>> Смотрите в Mouse Polling Rate Monitor прямо сейчас! <<<\n";

    uint8_t pkt[9] = { 0xDE, 0xAD, 0x04, 0x00, 0x18, 0, 0, 0, 0 };
    DWORD wr = 0;

    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    int64_t next_tick = t0.QuadPart;

    int dir = 1;
    int count = 0;
    int sweep_limit = 120; // Каждые 120 тиков меняем направление, чтобы мышь не улетала

    char cmd_pos[32], cmd_neg[32];
    int len_pos = wsprintfA(cmd_pos, "km.move_now(%d,0)\r\n", step_px);
    int len_neg = wsprintfA(cmd_neg, "km.move_now(-%d,0)\r\n", step_px);

    for (int i = 0; i < total_packets; ++i) {
        count++;
        if (count >= sweep_limit) {
            dir = -dir;
            count = 0;
        }

        if (use_move_now) {
            if (dir > 0) WriteFile(hComm, cmd_pos, len_pos, &wr, NULL);
            else WriteFile(hComm, cmd_neg, len_neg, &wr, NULL);
        } else {
            int16_t dx = static_cast<int16_t>(dir * step_px);
            pkt[5] = static_cast<uint8_t>(dx & 0xFF);
            pkt[6] = static_cast<uint8_t>((dx >> 8) & 0xFF);
            WriteFile(hComm, pkt, 9, &wr, NULL);
        }

        next_tick += step_ticks;
        LARGE_INTEGER cur;
        while (true) {
            QueryPerformanceCounter(&cur);
            if (cur.QuadPart >= next_tick) break;
            YieldProcessor();
        }
    }

    std::cout << "[+] Стрим завершен. Отправлено пакетов: " << total_packets << "\n";
}

// ---------------------------------------------------------------------------
// Тест через официальный C++ SDK (makxd::Device)
// ---------------------------------------------------------------------------
void run_sdk_stream(double dt_sec, int duration_sec, int step_px = 4) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double qpc_freq = static_cast<double>(freq.QuadPart);
    int64_t step_ticks = static_cast<int64_t>(dt_sec * qpc_freq);
    int total_packets = static_cast<int>(duration_sec / dt_sec);

    std::cout << "\n>>> СТАРТ СТРИМА через makxd::Device::mouseMove (" << duration_sec << " сек) <<<\n";
    makxd::Device device;
    if (!device.connect("COM5")) {
        std::cout << "[-] Ошибка подключения через makxd::Device!\n";
        return;
    }

    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    int64_t next_tick = t0.QuadPart;

    int dir = 1;
    int count = 0;

    for (int i = 0; i < total_packets; ++i) {
        count++;
        if (count >= 120) {
            dir = -dir;
            count = 0;
        }

        device.mouseMove(dir * step_px, 0);

        next_tick += step_ticks;
        LARGE_INTEGER cur;
        while (true) {
            QueryPerformanceCounter(&cur);
            if (cur.QuadPart >= next_tick) break;
            YieldProcessor();
        }
    }

    device.disconnect();
    std::cout << "[+] SDK Стрим завершен. Отправлено: " << total_packets << "\n";
}

int main() {
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    std::cout << "=================================================================\n";
    std::cout << "   MAKCU V4 DIRECT C++ 1000 HZ STREAM TEST (NO BLOAT, NO GUI)\n";
    std::cout << "=================================================================\n";
    std::cout << "[*] COM-порт: COM5 @ 4,000,000 Baud\n";
    std::cout << "[*] Выберите тест (нажмите клавишу):\n\n";
    std::cout << "  [1] 1000 Гц QPC (dt = 1.0 мс)  | Опкод 0x18 | 5 сек свип\n";
    std::cout << "  [2] 1250 Гц QPC (dt = 0.8 мс)  | Опкод 0x18 | nano совет (sub-1ms QPC)\n";
    std::cout << "  [3] 2000 Гц QPC (dt = 0.5 мс)  | Опкод 0x18 | двойной оверсэмплинг\n";
    std::cout << "  [4] 1000 Гц QPC (dt = 1.0 мс)  | km.move_now (прямой инжект в USB)\n";
    std::cout << "  [5] 1250 Гц QPC (dt = 0.8 мс)  | km.move_now (sub-1ms инжект)\n";
    std::cout << "  [6] 1000 Гц через официальный C++ SDK (makxd::Device::mouseMove)\n";
    std::cout << "  [0] Выход\n";
    std::cout << "=================================================================\n";
    std::cout << "Горячие клавиши F1..F6 также работают, когда активно окно монитора!\n\n";

    HANDLE hComm = open_com_port();
    if (hComm == INVALID_HANDLE_VALUE) {
        return 1;
    }

    while (true) {
        int choice = -1;

        if (_kbhit()) {
            char ch = _getch();
            if (ch >= '0' && ch <= '6') choice = ch - '0';
        }
        else if (GetAsyncKeyState(VK_F1) & 0x8000) { choice = 1; Sleep(300); }
        else if (GetAsyncKeyState(VK_F2) & 0x8000) { choice = 2; Sleep(300); }
        else if (GetAsyncKeyState(VK_F3) & 0x8000) { choice = 3; Sleep(300); }
        else if (GetAsyncKeyState(VK_F4) & 0x8000) { choice = 4; Sleep(300); }
        else if (GetAsyncKeyState(VK_F5) & 0x8000) { choice = 5; Sleep(300); }
        else if (GetAsyncKeyState(VK_F6) & 0x8000) { choice = 6; Sleep(300); }

        if (choice == 0) break;

        if (choice == 1) {
            run_stream(hComm, 0.0010, 5, false, 4); // 1000 Hz, 0x18
        }
        else if (choice == 2) {
            run_stream(hComm, 0.0008, 5, false, 4); // 1250 Hz, 0x18
        }
        else if (choice == 3) {
            run_stream(hComm, 0.0005, 5, false, 3); // 2000 Hz, 0x18
        }
        else if (choice == 4) {
            run_stream(hComm, 0.0010, 5, true, 4);  // 1000 Hz, move_now
        }
        else if (choice == 5) {
            run_stream(hComm, 0.0008, 5, true, 4);  // 1250 Hz, move_now
        }
        else if (choice == 6) {
            CloseHandle(hComm);
            run_sdk_stream(0.0010, 5, 4);
            hComm = open_com_port();
        }

        Sleep(10);
    }

    CloseHandle(hComm);
    timeEndPeriod(1);
    return 0;
}

