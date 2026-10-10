// Генератор движения с жёстким расписанием.
//
// Принципы стабильности (скорость не зависит ни от реальной мыши, ни от
// подтормаживаний ОС):
//  1. Положение - функция времени: p(tau), tau = k * period. Дельта тика =
//     round(p(tau_k)) - уже_отправлено. Ошибка округления не копится, путь
//     за любой отрезок времени точен до 1 пикселя.
//  2. Расписание - абсолютная сетка QPC t0 + k*period (без дрейфа от sleep).
//  3. Опоздание < 1 периода: дельта считается по номинальному времени тика,
//     поэтому шаги одинаковые, а джиттер ОС не превращается в джиттер скорости.
//     Пропущенные тики (опоздание >= периода) догоняются одной дельтой.
//     Длинный фриз (> catchupLimit) - сетка сдвигается, траектория встаёт на
//     паузу вместо прыжка.
//  4. Если транспорт занят - дельта не теряется и не ставится в очередь
//     (никаких пачек), а уходит со следующим тиком.
//
// Эталонный конвейер (fw 4094, 2026-10-10, см. SOLUTION_1000HZ_RAW_MOVE.md):
//   траектория p(t) -> [ABCurves Renderer: гладкое намерение за 1 мс ->
//   целочисленный отчёт] -> Transport::move() раз в 1 мс -> RawComTransport
//   (Format::RawMove, 0x69) -> ровно один HID-отчёт на команду, ~1000 Гц.
// С Raw частота HID = частота НЕНУЛЕВЫХ команд: пустой тик = нет отчёта.
// Старый путь MOVE 0x18 / km.move идёт через интерполятор прошивки
// (~640-690 Гц, команда режется на несколько отчётов) - только для сравнения.
// Траектории здесь захардкожены (Pattern) для тестов; в модуле их заменит
// внешний источник движения.
#pragma once

#include "abc_renderer.h"
#include "makcu_transport.h"
#include "mover_common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace mover {

enum class Pattern { Circle, Eight, Line, Ramp, Wobble, Flicks };

inline const char* patternName(Pattern p) {
    switch (p) {
    case Pattern::Circle: return "circle";
    case Pattern::Eight: return "eight";
    case Pattern::Line: return "line";
    case Pattern::Ramp: return "ramp";
    case Pattern::Wobble: return "wobble";
    case Pattern::Flicks: return "flicks";
    }
    return "?";
}

struct Point {
    double x;
    double y;
};

// Один "рывок" к цели: старт в startMs из точки from в точку to за durMs по
// профилю минимального рывка (Flash & Hogan): колоколообразная скорость, как у
// руки. Между рывками - пауза (позиция стоит).
struct FlickSeg {
    double startMs;
    double durMs;
    Point from;
    Point to;
};

// Последовательность рывков на durationMs: случайные цели в квадрате
// +-size вокруг старта, длительность по закону Фиттса (дальше - дольше),
// паузы 120-450 мс. Детерминирована seed'ом.
inline std::shared_ptr<const std::vector<FlickSeg>> makeFlicks(uint64_t seed, double durationMs, double size) {
    auto segs = std::make_shared<std::vector<FlickSeg>>();
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    Point cur{0.0, 0.0};
    double t = 150.0;
    while (t < durationMs) {
        Point to{(uni(rng) * 2.0 - 1.0) * size, (uni(rng) * 2.0 - 1.0) * size};
        const double dist = std::hypot(to.x - cur.x, to.y - cur.y);
        if (dist < 25.0) continue;
        const double dur = 110.0 + 75.0 * std::log2(1.0 + dist / 30.0) + uni(rng) * 40.0;
        segs->push_back({t, dur, cur, to});
        cur = to;
        t += dur + 120.0 + uni(rng) * 330.0;
    }
    return segs;
}

// Траектории постоянной (или ограниченной снизу) скорости: на каждом тике
// есть ненулевая дельта, иначе пустой тик = нет HID-отчёта = "провал" поллинга.
struct Trajectory {
    Pattern pattern = Pattern::Circle;
    double speed = 2.0;   // пикс/мс (= отсчётов/мс)
    double size = 150.0;  // радиус / амплитуда, пикс
    std::shared_ptr<const std::vector<FlickSeg>> flicks;  // для Pattern::Flicks

    [[nodiscard]] Point at(double tauMs) const {
        switch (pattern) {
        case Pattern::Flicks: {
            if (!flicks || flicks->empty()) return {0.0, 0.0};
            // Последний рывок, начавшийся не позже tau.
            auto it = std::upper_bound(flicks->begin(), flicks->end(), tauMs,
                [](double t, const FlickSeg& s) { return t < s.startMs; });
            if (it == flicks->begin()) return flicks->front().from;
            const FlickSeg& s = *(it - 1);
            const double u = std::clamp((tauMs - s.startMs) / s.durMs, 0.0, 1.0);
            const double m = u * u * u * (10.0 + u * (-15.0 + 6.0 * u));  // минимальный рывок
            return {s.from.x + (s.to.x - s.from.x) * m, s.from.y + (s.to.y - s.from.y) * m};
        }
        case Pattern::Circle: {
            const double w = speed / size;  // рад/мс, |v| = speed
            return {size * std::cos(w * tauMs) - size, size * std::sin(w * tauMs)};
        }
        case Pattern::Eight: {
            // Лиссажу 1:2. |v| = A*w*sqrt(cos^2 + cos^2(2)) >= 0.661*A*w,
            // подбираем w так, чтобы минимальная скорость = speed.
            const double w = speed / (0.6614 * size);
            return {size * std::sin(w * tauMs), 0.5 * size * std::sin(2.0 * w * tauMs)};
        }
        case Pattern::Line: {
            // Туда-обратно по X с постоянной скоростью: 0 -> +2A -> 0.
            const double span = 2.0 * size;
            const double d = std::fmod(speed * tauMs, 2.0 * span);
            return {d <= span ? d : 2.0 * span - d, 0.0};
        }
        case Pattern::Ramp:
            // Только вправо с постоянной скоростью: без разворотов сумма
            // сырых отсчётов в Raw Input точна (для замера потерь пути).
            return {speed * tauMs, 0.0};
        case Pattern::Wobble:
            // Как ramp, но Y дрожит на 1-2 отсчёта с несоизмеримым периодом:
            // соседние HID-отчёты почти никогда не совпадают байт в байт.
            return {speed * tauMs, 1.6 * std::sin(tauMs * 2.17) + 0.9 * std::sin(tauMs * 0.71)};
        }
        return {0.0, 0.0};
    }
};

struct RunConfig {
    std::string label;
    double hz = 1000.0;
    Trajectory trajectory;
    double durationS = 10.0;
    bool hybridWait = false;
    double catchupLimitMs = 25.0;
    int core = -1;
    // Имитация "руки": в момент burstAtS отправить burstFrames команд MOVE
    // подряд (как если бы прошивке пришлось вставить чужие отчёты). Путь при
    // этом не искажается - следующие тики компенсируют.
    int burstFrames = 0;
    double burstAtS = 3.0;
    // Диагностика: каждые pauseEveryS секунд не слать ничего pauseMs мс
    // (траектория на это время замирает, прыжка после паузы нет).
    double pauseEveryS = 0.0;
    double pauseMs = 0.0;
    // ABCurves Renderer: если задан, каждый тик 1 мс гладкое смещение
    // траектории проходит через рендерер, а отправляются его целочисленные
    // отчёты (частота принудительно 1000 Гц - модель обучена на 1 мс).
    AbcRenderer* renderer = nullptr;
    uint64_t rendererSeed = 2026;
};

// Живые счётчики: пишет поток тайминга, читает UI.
struct LiveStats {
    static constexpr std::array<double, 5> kLateEdgesUs{50, 100, 250, 500, 1000};
    static constexpr std::array<const char*, 6> kLateNames{
        "< 50 мкс", "50-100", "100-250", "250-500", "500-1000", "> 1 мс"};

    std::atomic<bool> running{false};
    std::atomic<bool> stopRequest{false};
    std::atomic<uint64_t> ticks{0};
    std::atomic<uint64_t> moves{0};        // команд с ненулевой дельтой, принятых транспортом
    std::atomic<uint64_t> deferred{0};     // тиков, когда транспорт был занят
    std::atomic<uint64_t> skippedTicks{0}; // тиков, пропущенных из-за опоздания потока
    std::atomic<uint64_t> stalls{0};       // фризов > catchupLimit
    std::atomic<uint64_t> burstSent{0};
    std::atomic<int64_t> sentX{0};
    std::atomic<int64_t> sentY{0};
    std::atomic<int64_t> sentAbs{0};  // длина отправленного пути |dx|+|dy|
    std::atomic<double> windowLateMaxUs{0.0};  // сбрасывает UI
    std::atomic<double> runLateMaxUs{0.0};
    std::array<std::atomic<uint64_t>, 6> lateHist{};
    std::atomic<double> elapsedS{0.0};
    std::atomic<bool> rendererError{false};
    std::atomic<uint64_t> rendererReports{0};  // ненулевых отчётов рендерера

    void reset() {
        running = false;
        stopRequest = false;
        ticks = 0;
        moves = 0;
        deferred = 0;
        skippedTicks = 0;
        stalls = 0;
        burstSent = 0;
        sentX = 0;
        sentY = 0;
        sentAbs = 0;
        windowLateMaxUs = 0.0;
        runLateMaxUs = 0.0;
        for (auto& h : lateHist) h = 0;
        elapsedS = 0.0;
        rendererError = false;
        rendererReports = 0;
    }
};

inline void runMotion(const RunConfig& cfg, Transport& tx, LiveStats& live) {
    tuneTimingThread(cfg.core);
    PreciseWaiter waiter(cfg.hybridWait);

    const double periodTicksF = static_cast<double>(Clock::freq()) / cfg.hz;
    const auto periodTicks = static_cast<int64_t>(std::llround(periodTicksF));
    const double periodMs = 1000.0 / cfg.hz;
    const int64_t catchupLimit = Clock::fromUs(cfg.catchupLimitMs * 1000.0);
    const auto totalTicks = static_cast<uint64_t>(std::llround(cfg.durationS * cfg.hz));
    const auto burstTick = static_cast<uint64_t>(std::llround(cfg.burstAtS * cfg.hz));
    bool burstDone = cfg.burstFrames <= 0;

    int64_t sentX = 0;
    int64_t sentY = 0;
    uint64_t moves = 0;
    double pausedMs = 0.0;

    auto sendDelta = [&](int64_t dx, int64_t dy) {
        if (tx.move(static_cast<int16_t>(dx), static_cast<int16_t>(dy))) {
            sentX += dx;
            sentY += dy;
            ++moves;
            live.moves.store(moves, std::memory_order_relaxed);
            live.sentX.store(sentX, std::memory_order_relaxed);
            live.sentY.store(sentY, std::memory_order_relaxed);
            live.sentAbs.fetch_add(std::llabs(dx) + std::llabs(dy), std::memory_order_relaxed);
            return true;
        }
        live.deferred.fetch_add(1, std::memory_order_relaxed);
        return false;
    };

    // --- ABCurves Renderer ---
    AbcRenderer* renderer = cfg.renderer;
    Point prevP = cfg.trajectory.at(0.0);
    uint64_t renderedK = 0;
    int64_t pendX = 0;  // отчёты рендерера, ещё не ушедшие в порт
    int64_t pendY = 0;
    auto renderTick = [&](double sdx, double sdy) {
        Report16 r{};
        if (!renderer->step(sdx, sdy, r)) return false;
        if (r.dx != 0 || r.dy != 0) {
            pendX += r.dx;
            pendY += r.dy;
            live.rendererReports.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    };
    if (renderer) {
        std::string err;
        if (!renderer->beginEvent(cfg.rendererSeed, err)) {
            live.rendererError = true;
            return;
        }
    }

    live.running = true;
    const int64_t start = Clock::now() + Clock::fromUs(2000.0);
    int64_t t0 = start;

    for (uint64_t k = 0; k <= totalTicks && !live.stopRequest.load(std::memory_order_relaxed);) {
        int64_t due = t0 + static_cast<int64_t>(std::llround(static_cast<double>(k) * periodTicksF));
        waiter.waitUntil(due);
        const int64_t now = Clock::now();
        int64_t late = now - due;

        if (late > catchupLimit) {
            // Фриз ОС: сдвигаем сетку, траектория продолжится с того же места.
            t0 += late;
            late = 0;
            live.stalls.fetch_add(1, std::memory_order_relaxed);
        } else if (late >= periodTicks) {
            // Пропустили тики: прыгаем на последний наступивший, путь догоняем
            // одной дельтой (положение во времени остаётся точным).
            const auto missed = static_cast<uint64_t>(late / periodTicks);
            k += missed;
            live.skippedTicks.fetch_add(missed, std::memory_order_relaxed);
            due = t0 + static_cast<int64_t>(std::llround(static_cast<double>(k) * periodTicksF));
            late = now - due;
            if (k > totalTicks) {
                break;
            }
        }

        const double wallMs = static_cast<double>(k) * periodMs;
        if (renderer) {
            // Шагаем рендерер по каждому тику 1 мс, включая пропущенные:
            // его внутренние часы - это и есть 1-мс отсчёты.
            bool ok = true;
            for (uint64_t j = renderedK + 1; j <= k && ok; ++j) {
                const Point p = cfg.trajectory.at(static_cast<double>(j) * periodMs);
                ok = renderTick(p.x - prevP.x, p.y - prevP.y);
                prevP = p;
            }
            renderedK = k;
            if (!ok) {
                live.rendererError = true;
                break;
            }
            if (pendX != 0 || pendY != 0) {
                const int64_t dx = std::clamp<int64_t>(pendX, -32767, 32767);
                const int64_t dy = std::clamp<int64_t>(pendY, -32767, 32767);
                if (sendDelta(dx, dy)) {
                    pendX -= dx;
                    pendY -= dy;
                } else if (!tx.healthy()) {
                    break;
                }
            }
        } else if (cfg.pauseEveryS > 0.0 && cfg.pauseMs > 0.0 && wallMs >= 1000.0 * cfg.pauseEveryS &&
            std::fmod(wallMs, 1000.0 * cfg.pauseEveryS) < cfg.pauseMs) {
            pausedMs += periodMs;
            ++k;
            live.ticks.store(k, std::memory_order_relaxed);
            continue;
        }

        if (!renderer) {
            // Траектория в номинальный момент тика.
            const Point p = cfg.trajectory.at(wallMs - pausedMs);
            const int64_t dx = std::clamp<int64_t>(std::llround(p.x) - sentX, -32767, 32767);
            const int64_t dy = std::clamp<int64_t>(std::llround(p.y) - sentY, -32767, 32767);
            if (dx != 0 || dy != 0) {
                if (!sendDelta(dx, dy) && !tx.healthy()) {
                    break;
                }
            }
        }

        if (!burstDone && k >= burstTick) {
            // Всплеск: N команд по 1 пикселю подряд, без пауз. Следующие тики
            // вычтут этот путь из траектории (dx = цель - отправлено).
            burstDone = true;
            for (int i = 0; i < cfg.burstFrames; ++i) {
                if (sendDelta(1, 0)) {
                    live.burstSent.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // Статистика опозданий отправки относительно сетки.
        const double lateUs = Clock::toUs(std::max<int64_t>(0, late));
        size_t bucket = 0;
        while (bucket < LiveStats::kLateEdgesUs.size() && lateUs >= LiveStats::kLateEdgesUs[bucket]) {
            ++bucket;
        }
        live.lateHist[bucket].fetch_add(1, std::memory_order_relaxed);
        if (lateUs > live.windowLateMaxUs.load(std::memory_order_relaxed)) {
            live.windowLateMaxUs.store(lateUs, std::memory_order_relaxed);
        }
        if (lateUs > live.runLateMaxUs.load(std::memory_order_relaxed)) {
            live.runLateMaxUs.store(lateUs, std::memory_order_relaxed);
        }

        ++k;
        live.ticks.store(k, std::memory_order_relaxed);
        live.elapsedS.store(Clock::toUs(now - start) / 1e6, std::memory_order_relaxed);
    }

    // Рендерер держит дробный остаток пути: дошагиваем нулевым намерением
    // (до 120 мс), чтобы он выпустил накопленное, и отправляем хвост.
    if (renderer && !live.rendererError.load() && !live.stopRequest.load()) {
        int64_t due = Clock::now();
        int quiet = 0;
        for (int i = 0; i < 120 && quiet < 30; ++i) {
            due += periodTicks;
            waiter.waitUntil(due);
            if (!renderTick(0.0, 0.0)) break;
            if (pendX == 0 && pendY == 0) {
                ++quiet;
                continue;
            }
            quiet = 0;
            if (sendDelta(std::clamp<int64_t>(pendX, -32767, 32767), std::clamp<int64_t>(pendY, -32767, 32767))) {
                pendX = 0;
                pendY = 0;
            }
        }
    }
    tx.finish();  // raw-транспорт: дождаться квитанций и дослать перенос
    live.running = false;
}

}  // namespace mover
