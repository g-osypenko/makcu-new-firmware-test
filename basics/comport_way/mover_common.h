// Общие утилиты: точное время (QPC), ожидание до дедлайна, настройка
// приоритетов/энергосбережения Windows, временное отключение акселерации.
#pragma once

// Windows 10+ API (SetProcessInformation, power throttling, high-res timers).
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace mover {

// ---------------------------------------------------------------------------
// QPC-часы. Все расписания строятся в тиках QPC, без накопления ошибки.
// ---------------------------------------------------------------------------
struct Clock {
    static int64_t freq() {
        static const int64_t f = [] {
            LARGE_INTEGER v;
            QueryPerformanceFrequency(&v);
            return static_cast<int64_t>(v.QuadPart);
        }();
        return f;
    }
    static int64_t now() {
        LARGE_INTEGER v;
        QueryPerformanceCounter(&v);
        return v.QuadPart;
    }
    static int64_t fromUs(double us) {
        return static_cast<int64_t>(std::llround(us * static_cast<double>(freq()) / 1e6));
    }
    static double toUs(int64_t ticks) {
        return static_cast<double>(ticks) * 1e6 / static_cast<double>(freq());
    }
};

// ---------------------------------------------------------------------------
// Ожидание до абсолютного дедлайна.
//  spin   - чистый busy-wait по QPC (минимальный джиттер, 1 ядро на 100%).
//  hybrid - high-resolution waitable timer до (дедлайн - запас), затем spin.
// ---------------------------------------------------------------------------
class PreciseWaiter {
public:
    explicit PreciseWaiter(bool hybrid) : hybrid_(hybrid) {
        if (hybrid_) {
            timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!timer_) {
                hybrid_ = false;
            }
        }
        spinMargin_ = Clock::fromUs(350.0);
    }
    ~PreciseWaiter() {
        if (timer_) {
            CloseHandle(timer_);
        }
    }
    PreciseWaiter(const PreciseWaiter&) = delete;
    PreciseWaiter& operator=(const PreciseWaiter&) = delete;

    void waitUntil(int64_t deadline) {
        if (hybrid_) {
            const int64_t remain = deadline - Clock::now();
            if (remain > spinMargin_) {
                LARGE_INTEGER due;
                // Относительное время в 100-нс единицах (отрицательное).
                due.QuadPart = -static_cast<LONGLONG>(Clock::toUs(remain - spinMargin_) * 10.0);
                if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(timer_, INFINITE);
                }
            }
        }
        while (Clock::now() < deadline) {
            YieldProcessor();
        }
    }

    [[nodiscard]] bool hybrid() const { return hybrid_; }

private:
    bool hybrid_;
    HANDLE timer_ = nullptr;
    int64_t spinMargin_;
};

// ---------------------------------------------------------------------------
// Приоритеты и энергосбережение.
// В Windows 11 фоновый процесс (консоль не в фокусе, а в фокусе тестер
// поллинга) может получить EcoQoS и игнорирование timeBeginPeriod. Явно
// отключаем оба механизма, иначе тайминг "плывёт" при смене фокуса.
// ---------------------------------------------------------------------------
inline void tuneProcessForTiming() {
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

#ifdef PROCESS_POWER_THROTTLING_CURRENT_VERSION
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
#ifdef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
    state.ControlMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
#else
    state.ControlMask |= 0x4u;
#endif
    state.StateMask = 0;  // 0 = механизм выключен для процесса
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
#endif
}

inline void restoreProcessTiming() {
    timeEndPeriod(1);
}

inline void tuneTimingThread(int core) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#ifdef THREAD_POWER_THROTTLING_CURRENT_VERSION
    THREAD_POWER_THROTTLING_STATE state{};
    state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
#endif
    if (core >= 0 && core < 64) {
        SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << core);
    }
}

// ---------------------------------------------------------------------------
// "Повышенная точность указателя" (Enhance Pointer Precision).
// С ней скорость курсора зависит от величины дельты в КАЖДОМ HID-отчёте.
// Когда реальная мышь двигается, её дельты складываются с инжектом в одних
// отчётах -> нелинейная кривая Windows ускоряет/замедляет инжект. Отключаем
// только для текущей сессии (без записи в реестр) и возвращаем при выходе.
// На Raw Input (игры, тестеры поллинга) эта настройка не влияет.
// ---------------------------------------------------------------------------
class PointerAccelGuard {
public:
    bool disable() {
        int params[3]{};
        if (!SystemParametersInfoW(SPI_GETMOUSE, 0, params, 0)) {
            return false;
        }
        original_[0] = params[0];
        original_[1] = params[1];
        original_[2] = params[2];
        wasEnabled_ = params[2] != 0;
        if (!wasEnabled_) {
            return true;
        }
        int off[3]{0, 0, 0};
        if (!SystemParametersInfoW(SPI_SETMOUSE, 0, off, 0)) {
            return false;
        }
        changed_.store(true);
        return true;
    }
    void restore() {
        bool expected = true;
        if (changed_.compare_exchange_strong(expected, false)) {
            SystemParametersInfoW(SPI_SETMOUSE, 0, original_, 0);
        }
    }
    [[nodiscard]] bool wasEnabled() const { return wasEnabled_; }

private:
    int original_[3]{};
    bool wasEnabled_ = false;
    std::atomic<bool> changed_{false};
};

}  // namespace mover
