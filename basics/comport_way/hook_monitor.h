// Счётчик HID-событий мыши через низкоуровневый хук WH_MOUSE_LL.
//
// Raw Input для фонового окна Windows 11 прореживает (~125 Гц), а консоль во
// время теста всегда в фоне. LL-хук вызывается системой на КАЖДОЕ событие
// ввода мыши независимо от фокуса, поэтому число вызовов = число отчётов,
// которые увидел Windows (как у внешнего тестера поллинга).
// Колбэк минимальный: QPC + счётчик + корзина интервала, без блокировок.
#pragma once

#include "mover_common.h"

#include <array>
#include <atomic>
#include <thread>
#include <vector>

namespace mover {

class HookMonitor {
public:
    // Границы корзин интервалов, мкс.
    static constexpr std::array<double, 5> kEdgesUs{750, 1250, 1750, 2500, 5000};
    static constexpr std::array<const char*, 6> kBucketNames{
        "< 0.75 мс", "0.75-1.25 (1 кадр)", "1.25-1.75", "1.75-2.5 (2 кадра)", "2.5-5", "> 5 мс"};

    // Событие для журнала: время QPC и позиция курсора (pt хука, экранные
    // координаты). При выключенной "Повышенной точности" и скорости указателя
    // 10/20 разность pt соседних событий = сырые отсчёты (пока курсор не у края).
    struct Event {
        int64_t t;
        int32_t x;
        int32_t y;
    };
    static constexpr size_t kLogCapacity = 1u << 19;  // ~8 мин при 1000 Гц, 8 МБ

    ~HookMonitor() { stop(); }

    bool start() {
        if (thread_.joinable()) return true;
        // Память журнала - один раз, до появления потока хука (дальше не перевыделяется).
        if (log_.empty()) log_.resize(kLogCapacity);
        instance_ = this;
        ready_ = false;
        thread_ = std::thread([this] { threadMain(); });
        for (int i = 0; i < 200 && !ready_; ++i) Sleep(5);
        return ok_;
    }

    void stop() {
        if (!thread_.joinable()) return;
        PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
        thread_.join();
        instance_ = nullptr;
    }

    void reset() {
        count_.store(0);
        last_.store(0);
        for (auto& b : hist_) b.store(0);
    }

    // Журнал событий: начать с нуля / остановить и забрать копию.
    void startLog() {
        logging_.store(false, std::memory_order_release);
        Sleep(5);  // хук мог быть внутри onEvent с прошлого журнала
        logN_.store(0, std::memory_order_release);
        logging_.store(true, std::memory_order_release);
    }
    std::vector<Event> stopLog() {
        logging_.store(false, std::memory_order_release);
        Sleep(5);  // дать хуку дописать событие, если он был внутри onEvent
        const size_t n = logN_.load(std::memory_order_acquire);
        return std::vector<Event>(log_.begin(), log_.begin() + static_cast<std::ptrdiff_t>(n));
    }

    [[nodiscard]] uint64_t count() const { return count_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::array<uint64_t, 6> histogram() const {
        std::array<uint64_t, 6> h{};
        for (size_t i = 0; i < h.size(); ++i) h[i] = hist_[i].load(std::memory_order_relaxed);
        return h;
    }

private:
    static LRESULT CALLBACK hookProc(int code, WPARAM wp, LPARAM lp) {
        if (code == HC_ACTION && wp == WM_MOUSEMOVE && instance_) {
            const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
            if (!(info->flags & LLMHF_INJECTED)) {  // SendInput не считаем, MAKCU - железо
                instance_->onEvent(info->pt);
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    void onEvent(POINT pt) {
        const int64_t now = Clock::now();
        if (logging_.load(std::memory_order_acquire)) {
            const size_t n = logN_.load(std::memory_order_relaxed);
            if (n < log_.size()) {
                log_[n] = {now, pt.x, pt.y};
                logN_.store(n + 1, std::memory_order_release);
            }
        }
        const int64_t prev = last_.exchange(now, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
        if (prev != 0) {
            const double us = Clock::toUs(now - prev);
            size_t b = 0;
            while (b < kEdgesUs.size() && us >= kEdgesUs[b]) ++b;
            hist_[b].fetch_add(1, std::memory_order_relaxed);
        }
    }

    void threadMain() {
        threadId_ = GetCurrentThreadId();
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // создать очередь сообщений
        HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, &HookMonitor::hookProc, GetModuleHandleW(nullptr), 0);
        ok_ = hook != nullptr;
        ready_ = true;
        if (!hook) return;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            DispatchMessageW(&msg);
        }
        UnhookWindowsHookEx(hook);
    }

    static inline HookMonitor* instance_ = nullptr;
    std::thread thread_;
    DWORD threadId_ = 0;
    std::atomic<bool> ready_{false};
    std::atomic<bool> ok_{false};
    std::atomic<uint64_t> count_{0};
    std::atomic<int64_t> last_{0};
    std::array<std::atomic<uint64_t>, 6> hist_{};
    std::vector<Event> log_;
    std::atomic<size_t> logN_{0};
    std::atomic<bool> logging_{false};
};

}  // namespace mover
