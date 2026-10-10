#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <iostream>
#include <cmath>
#include <cstdint>

// Линковка winmm для timeBeginPeriod(1)
#pragma comment(lib, "winmm.lib")

constexpr const char* PORT_NAME = "\\\\.\\COM5";
constexpr DWORD MAKCU_BAUD = 4000000; // 4 MBaud UART (чип CH343)
constexpr double PI = 3.14159265358979323846;

// -----------------------------------------------------------------------------
// 1. Быстрое открытие COM5 без задержек и без случайного ресета ESP32
// -----------------------------------------------------------------------------
HANDLE open_com5() {
    HANDLE hComm = CreateFileA(
        PORT_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (hComm == INVALID_HANDLE_VALUE) {
        std::cerr << "[-] Ошибка: Не удалось открыть " << PORT_NAME << "! Проверьте порт.\n";
        return INVALID_HANDLE_VALUE;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(hComm, &dcb)) {
        CloseHandle(hComm);
        return INVALID_HANDLE_VALUE;
    }

    dcb.BaudRate = MAKCU_BAUD;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    // КРИТИЧНО для CH343/ESP32: отключение DTR/RTS предотвращает сброс микроконтроллера
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;

    if (!SetCommState(hComm, &dcb)) {
        std::cerr << "[-] Ошибка конфигурации DCB на 4,000,000 бод!\n";
        CloseHandle(hComm);
        return INVALID_HANDLE_VALUE;
    }

    // Мгновенные неблокирующие таймауты
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(hComm, &timeouts);

    // Буфер драйвера 64 КБ
    SetupComm(hComm, 65536, 65536);
    return hComm;
}

// -----------------------------------------------------------------------------
// 2. Отправка пакета перемещения (Опкод 0x18 MAK_API: 9 байт)
// -----------------------------------------------------------------------------
inline void send_move(HANDLE hComm, int16_t dx, int16_t dy) {
    uint8_t pkt[9] = {
        0xDE, 0xAD,             // Заголовок фрейма MAK_API
        0x04, 0x00,             // Длина полезной нагрузки = 4 байта (uint16_le)
        0x18,                   // Опкод 0x18: MOVE
        static_cast<uint8_t>(dx & 0xFF),
        static_cast<uint8_t>((dx >> 8) & 0xFF),
        static_cast<uint8_t>(dy & 0xFF),
        static_cast<uint8_t>((dy >> 8) & 0xFF)
    };
    DWORD written = 0;
    WriteFile(hComm, pkt, sizeof(pkt), &written, NULL);
}

// -----------------------------------------------------------------------------
// 3. Непрерывный стрим 1000 Гц с наносекундным QPC-таймингом
//    Использует орбитальное круговое движение: ни одного нулевого тика!
// -----------------------------------------------------------------------------
void stream_1000hz(HANDLE hComm, double duration_sec = 5.0, double radius = 260.0, double revs_per_sec = 2.0) {
    LARGE_INTEGER freq, cur, t0;
    QueryPerformanceFrequency(&freq);
    const double qpc_freq = static_cast<double>(freq.QuadPart);

    // Интервал для 1000 Гц = 1.000 мс (1/1000 сек)
    const int64_t step_ticks = static_cast<int64_t>(qpc_freq * 0.0010);
    const int total_ticks = static_cast<int>(std::round(duration_sec * 1000.0));
    const double d_theta = (2.0 * PI * revs_per_sec) / 1000.0;

    std::cout << "[>>>] СТАРТ СТРИМА: 1000 Гц (dt = 1.00 мс) на " << duration_sec << " сек...\n";
    std::cout << "      Смотрите график в Mouse Polling Rate Monitor прямо сейчас!\n";
    MessageBeep(MB_OK);

    double theta = 0.0;
    double prev_x = radius;
    double prev_y = 0.0;
    double acc_x = 0.0;
    double acc_y = 0.0;

    QueryPerformanceCounter(&t0);
    int64_t next_tick = t0.QuadPart;

    int emitted_reports = 0;

    for (int i = 0; i < total_ticks; ++i) {
        // Досрочный выход по клавише ESC
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            std::cout << "[!] Прервано по нажатию ESC.\n";
            break;
        }

        // Расчет плавной траектории
        theta += d_theta;
        double cur_x = radius * std::cos(theta);
        double cur_y = radius * std::sin(theta);

        acc_x += (cur_x - prev_x);
        acc_y += (cur_y - prev_y);
        prev_x = cur_x;
        prev_y = cur_y;

        int16_t dx = static_cast<int16_t>(std::round(acc_x));
        int16_t dy = static_cast<int16_t>(std::round(acc_y));
        acc_x -= dx;
        acc_y -= dy;

        // Отправка в COM5
        send_move(hComm, dx, dy);
        emitted_reports++;

        // Бездрейфовый QPC Spin-Wait
        next_tick += step_ticks;
        while (true) {
            QueryPerformanceCounter(&cur);
            if (cur.QuadPart >= next_tick) break;
            YieldProcessor();
        }
    }

    MessageBeep(MB_ICONASTERISK);
    std::cout << "[+] Стрим завершен! Отправлено " << emitted_reports << " отчетов @ 1000 Гц.\n\n";
}

int main() {
    // Настройка приоритетов реального времени ОС
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    std::cout << "=================================================================\n";
    std::cout << "   MAKCU V4 MINIMAL 1000 HZ DRIVER (COM5 / 4M BAUD / OP 0x18)\n";
    std::cout << "=================================================================\n";

    HANDLE hComm = open_com5();
    if (hComm == INVALID_HANDLE_VALUE) {
        timeEndPeriod(1);
        return 1;
    }
    std::cout << "[+] Порт COM5 успешно открыт @ 4,000,000 бод (Zero-Latency Win32).\n";
    std::cout << "[*] Управление (глобальные горячие клавиши, фокус окна не требуется):\n";
    std::cout << "      [F1] или [Пробел] -> Запустить непрерывный стрим 1000 Гц на 5 сек\n";
    std::cout << "      [ESC]             -> Выход из программы\n";
    std::cout << "=================================================================\n";
    std::cout << ">>> Переключитесь на окно 'Mouse Polling Rate Monitor v1.0' и нажмите F1 или Пробел! <<<\n\n";

    while (true) {
        if (GetAsyncKeyState(VK_F1) & 0x8000 || GetAsyncKeyState(VK_SPACE) & 0x8000) {
            Sleep(200); // Подавление дребезга клавиши
            stream_1000hz(hComm, 5.0, 260.0, 2.0);
        }
        else if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            break;
        }

        Sleep(15);
    }

    CloseHandle(hComm);
    timeEndPeriod(1);
    std::cout << "[*] COM5 закрыт. Завершение работы.\n";
    return 0;
}

