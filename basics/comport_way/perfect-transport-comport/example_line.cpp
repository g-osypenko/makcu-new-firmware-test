// Пример и самопроверка makcu_raw_transport.h.
//
//   example_line.exe [--port COM5] [--check] [--buttons S]
//     --check      открыть порт, показать сессию Raw и закрыть (курсор не двигается)
//     --buttons S  S секунд показывать кнопки мыши (нажимайте), курсор не двигается
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
    double buttonsS = 0.0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = argv[++i];
        else if (std::strcmp(argv[i], "--check") == 0) check = true;
        else if (std::strcmp(argv[i], "--buttons") == 0 && i + 1 < argc) buttonsS = std::atof(argv[++i]);
        else {
            std::printf("example_line.exe [--port COM5] [--check] [--buttons S]\n");
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
    if (buttonsS > 0.0) {
        // Кнопки: маску обновляет поток чтения по событиям 0x53. service() в цикле нужен
        // только для пересинхронизации после переполнения потока (кадр уходит из этого потока).
        using makcu::MouseButton;
        const MouseButton all[] = {MouseButton::Left, MouseButton::Right, MouseButton::Middle, MouseButton::Side1,
            MouseButton::Side2};
        std::printf(">>> %.0f с: нажимайте кнопки мыши\n", buttonsS);
        const int64_t t0 = makcu::detail::Qpc::now();
        uint8_t last = 0xFF;
        while (makcu::detail::Qpc::toUs(makcu::detail::Qpc::now() - t0) < buttonsS * 1e6) {
            tx.service();
            const uint8_t m = tx.buttons();
            if (m != last) {
                std::printf("  +%6.3f с  маска %02X ", makcu::detail::Qpc::toUs(makcu::detail::Qpc::now() - t0) / 1e6, m);
                for (MouseButton b : all) {
                    if (tx.buttonDown(b)) std::printf(" %s", makcu::buttonName(b));
                }
                std::printf("\n");
                last = m;
            }
            Sleep(1);
        }
        std::printf("нажатий:");
        for (MouseButton b : all) std::printf(" %s %u", makcu::buttonName(b), tx.pressCount(b));
        std::printf("\n%s\n", tx.statsLine().c_str());
        tx.close();
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
    std::printf("принято move(): %llu из 2000 тиков | записей %llu, склеено %llu, из переноса %llu | путь отправлен %lld (+ не принято %lld)\n",
        static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(s.submitted),
        static_cast<unsigned long long>(s.merged), static_cast<unsigned long long>(s.carried),
        static_cast<long long>(sentX), static_cast<long long>(pendX));
    std::printf("%s\n", tx.statsLine().c_str());
    // Склейка меняет счёт: принятый move() - это запись или прибавка к следующей записи.
    const bool ok = closed && s.submitted == s.endpointOk && accepted + s.carried == s.submitted + s.merged &&
                    s.rejected == 0 && s.immediate == 0 && s.errors == 0 && s.carryX == 0 && s.carryY == 0 && pendX == 0;
    std::printf(ok ? "[+] OK: каждая запись ушла одним HID-отчётом, склейка сошлась, путь цел\n"
                   : "[-] ЕСТЬ ПОТЕРИ/ОТКАЗЫ - см. строку raw 0x69 выше\n");
    tx.close();
    return ok ? 0 : 1;
}
