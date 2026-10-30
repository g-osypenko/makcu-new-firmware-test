// Пример и самопроверка makcu_udp_batch_transport.h.
//
//   example_line.exe [--host 192.168.50.175] [--port 8080] [--check] [--buttons S]
//     --check      связь + прошивка + Raw-метаданные, курсор не двигается
//     --buttons S  S секунд показывать кнопки мыши (нажимайте), курсор не двигается
//     без них 2 с по X: 1 с вправо, 1 с влево по 2 отсчёта каждую 1 мс
//              (итоговый путь 0, курсор вернётся на место), мышь не трогать
//
// Здесь же минимальный образец темпа: абсолютная сетка QPC 1 мс, spin-ожидание,
// дельта при move() == false остаётся у вызывающего и уходит следующим тиком.
// Частоту HID пример считает низкоуровневым хуком мыши (WH_MOUSE_LL): каждый
// HID-отчёт MAKCU, который увидел Windows, независимо от фокуса окна.
#include "makcu_udp_batch_transport.h"

#include <timeapi.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// Счётчик HID-отчётов мыши: число вызовов хука и доля интервалов ~1 мс.
std::atomic<uint64_t> g_events{0};
std::atomic<uint64_t> g_oneMs{0};
std::atomic<int64_t> g_last{0};
std::atomic<bool> g_count{false};
DWORD g_hookThread = 0;

LRESULT CALLBACK hookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_MOUSEMOVE && g_count.load(std::memory_order_relaxed)) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (!(info->flags & LLMHF_INJECTED)) {  // MAKCU - железо, SendInput не считаем
            const int64_t now = makcu::detail::Qpc::now();
            const int64_t prev = g_last.exchange(now, std::memory_order_relaxed);
            g_events.fetch_add(1, std::memory_order_relaxed);
            if (prev) {
                const double us = makcu::detail::Qpc::toUs(now - prev);
                if (us >= 750.0 && us < 1250.0) g_oneMs.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void hookMain(std::atomic<bool>* ready) {
    g_hookThread = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, &hookProc, GetModuleHandleW(nullptr), 0);
    ready->store(true);
    if (!hook) return;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    UnhookWindowsHookEx(hook);
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::string host = "192.168.50.175";
    uint16_t port = makcu::UdpBatchTransport::kDefaultPort;
    bool check = false;
    double buttonsS = 0.0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
        else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--check") == 0) check = true;
        else if (std::strcmp(argv[i], "--buttons") == 0 && i + 1 < argc) buttonsS = std::atof(argv[++i]);
        else {
            std::printf("example_line.exe [--host 192.168.50.175] [--port 8080] [--check] [--buttons S]\n");
            return 2;
        }
    }

    makcu::UdpBatchTransport tx;
    std::string err;
    if (!tx.open(host, port, err)) {
        std::printf("[-] %s\n", err.c_str());
        return 1;
    }
    std::printf("[+] %s | %s\n", tx.endpoint().c_str(), tx.info().c_str());
    if (check) {
        tx.close();
        std::printf("[+] связь, прошивка и Raw API в порядке\n");
        return 0;
    }
    if (buttonsS > 0.0) {
        // Кнопки: читать маску можно из любого потока, транспорт обновляет её сам.
        using makcu::MouseButton;
        const MouseButton all[] = {MouseButton::Left, MouseButton::Right, MouseButton::Middle, MouseButton::Side1,
            MouseButton::Side2};
        std::printf(">>> %.0f с: нажимайте кнопки мыши\n", buttonsS);
        const int64_t t0 = makcu::detail::Qpc::now();
        uint8_t last = 0xFF;
        while (makcu::detail::Qpc::toUs(makcu::detail::Qpc::now() - t0) < buttonsS * 1e6) {
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

    std::atomic<bool> hookReady{false};
    std::thread hookThread(hookMain, &hookReady);
    while (!hookReady.load()) Sleep(1);
    Sleep(100);

    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    using Q = makcu::detail::Qpc;
    const int64_t period = Q::fromUs(1000.0);
    int64_t due = Q::now() + Q::fromUs(2000.0);
    int64_t pendX = 0;  // дельта, которую транспорт пока не принял
    int64_t sentX = 0;
    uint64_t accepted = 0;
    g_count = true;
    const int64_t t0 = Q::now();
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
    const double runS = Q::toUs(Q::now() - t0) / 1e6;
    g_count = false;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    timeEndPeriod(1);
    PostThreadMessageW(g_hookThread, WM_QUIT, 0, 0);
    hookThread.join();

    const auto s = tx.stats();
    const uint64_t hid = g_events.load();
    std::printf("принято move(): %llu из 2000 тиков | путь отправлен %lld (+ не принято %lld)\n",
        static_cast<unsigned long long>(accepted), static_cast<long long>(sentX), static_cast<long long>(pendX));
    std::printf("HID (LL-хук): %llu отчётов, ~%.0f Гц, интервалов ~1 мс %.1f %%\n", static_cast<unsigned long long>(hid),
        static_cast<double>(hid) / runS, hid > 1 ? 100.0 * static_cast<double>(g_oneMs.load()) / static_cast<double>(hid - 1) : 0.0);
    std::printf("%s\n", tx.statsLine().c_str());
    std::printf("склеено move() с последней неотправленной записью (очередь >= maxLag %u): %llu\n", tx.maxLag(),
        static_cast<unsigned long long>(s.merged));
    const bool ok = closed && s.records + s.merged == accepted && s.admitted == s.records && s.termRejected == 0 &&
                    s.envelope == 0 && s.badReplies == 0 && s.pending == 0 && !s.inFlight && pendX == 0;
    std::printf(ok ? "[+] OK: каждая запись принята платой и ушла одним HID-отчётом, плата чистая\n"
                   : "[-] ЕСТЬ ПОТЕРИ/ОТКАЗЫ - см. строку batch 0x6C выше\n");
    tx.close();
    return ok ? 0 : 1;
}
