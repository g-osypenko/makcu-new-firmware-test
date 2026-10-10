// Пример и самопроверка makcu_raw_transport.h.
//
//   example_line.exe [--port COM5] [--check]
//     --check  открыть порт, показать сессию Raw и закрыть (курсор не двигается)
//     без него 2 с по X: 1 с вправо, 1 с влево по 2 отсчёта каждую 1 мс
//              (итоговый путь 0, курсор вернётся на место), мышь не трогать
//
// Здесь же минимальный образец темпа: абсолютная сетка QPC 1 мс, spin-ожидание,
// дельта при move() == false остаётся у вызывающего и уходит следующим тиком.
#include "makcu_raw_transport.h"

#include <timeapi.h>

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::string port;
    bool check = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = argv[++i];
        else if (std::strcmp(argv[i], "--check") == 0) check = true;
        else {
            std::printf("example_line.exe [--port COM5] [--check]\n");
            return 2;
        }
    }

    makcu::RawTransport tx;
    std::string err;
    if (!tx.open(port, err)) {
        std::printf("[-] %s\n", err.c_str());
        return 1;
    }
    std::printf("[+] %s, %u бод | %s\n", tx.port().c_str(), tx.baud(), tx.info().c_str());
    if (check) {
        tx.close();
        std::printf("[+] связь и Raw API в порядке\n");
        return 0;
    }

    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    using Q = makcu::detail::Qpc;
    const int64_t period = Q::fromUs(1000.0);
    int64_t due = Q::now() + Q::fromUs(2000.0);
    int64_t pendX = 0;  // дельта, которую транспорт пока не принял
    int64_t sentX = 0;
    uint64_t accepted = 0;
    for (int k = 0; k < 2000; ++k) {
        while (Q::now() < due) YieldProcessor();
        due += period;
        pendX += k < 1000 ? 2 : -2;
        if (pendX != 0 && tx.move(static_cast<int16_t>(pendX), 0)) {
            sentX += pendX;
            pendX = 0;
            ++accepted;
        }
    }
    const bool closed = tx.finish();

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    timeEndPeriod(1);

    const auto s = tx.stats();
    std::printf("принято move(): %llu из 2000 тиков | путь отправлен %lld (+ не принято %lld)\n",
        static_cast<unsigned long long>(accepted), static_cast<long long>(sentX), static_cast<long long>(pendX));
    std::printf("%s\n", tx.statsLine().c_str());
    const bool ok = closed && s.submitted == s.endpointOk && s.rejected == 0 && s.immediate == 0 && s.errors == 0 &&
                    s.carryX == 0 && s.carryY == 0 && pendX == 0;
    std::printf(ok ? "[+] OK: каждая команда ушла ровно одним HID-отчётом, путь цел\n"
                   : "[-] ЕСТЬ ПОТЕРИ/ОТКАЗЫ - см. строку raw 0x69 выше\n");
    tx.close();
    return ok ? 0 : 1;
}
