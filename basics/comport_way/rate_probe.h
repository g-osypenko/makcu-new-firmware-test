// Замер "скорость -> частота отчётов" (стенд, в модуль не входит).
//
// Прогон раскладывается в ряд 1-мс слотов: скорость в слоте (отсчётов/мс) и
// число HID-событий, которые увидел LL-хук. Для инжекта ещё - выдал ли
// субстеппер/рендерер ненулевой отчёт в этом тике. Дальше слоты группируются
// по скорости, и для каждой корзины печатается:
//   - частота отчётов субстеппера (что решил генератор);
//   - частота HID (что дошло до Windows; с raw 0x69 должна совпадать);
//   - медиана и разброс (CV = СКО/среднее) интервалов между HID-событиями:
//     у чистого накопителя интервалы ровные (CV ~ 0, остаток - джиттер ОС),
//     у руки и рендерера - "рваные";
//   - доля интервалов 1 / 2 / 3 / 4-5 / 6+ мс.
// Рука (F12) меряется тем же способом, скорость - по разности pt хука.
#pragma once

#include "hook_monitor.h"
#include "motion_engine.h"  // TickLog

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace mover {

struct RateSeries {
    std::vector<float> speed;     // отсчётов/мс
    std::vector<uint16_t> hid;    // HID-событий в слоте
    std::vector<uint8_t> report;  // ненулевой отчёт генератора в слоте (для руки пусто)
    std::vector<std::pair<int64_t, int32_t>> events;  // время события и его слот (-1 - вне ряда)
};

// Инжект: слоты = тики журнала. Событие хука относится к последнему тику,
// обработанному до него (задержка доставки ~1-2 мс сдвигает его на 1-2 слота -
// для корзин по скорости это не важно).
inline RateSeries seriesFromRun(const TickLog& log, const std::vector<HookMonitor::Event>& events) {
    RateSeries s;
    s.speed = log.speed;
    s.report = log.report;
    s.hid.assign(log.speed.size(), 0);
    if (log.tSend.empty()) return s;
    const int64_t tailLimit = Clock::fromUs(3000.0);
    for (const auto& e : events) {
        int32_t slot = -1;
        const auto it = std::upper_bound(log.tSend.begin(), log.tSend.end(), e.t);
        if (it != log.tSend.begin() && e.t - log.tSend.back() <= tailLimit) {
            slot = static_cast<int32_t>(it - log.tSend.begin() - 1);
            if (s.hid[slot] < 65535) ++s.hid[slot];
        }
        s.events.emplace_back(e.t, slot);
    }
    return s;
}

// Рука: слоты по времени от первого события, скорость - модуль смещения
// курсора за окно 11 мс по центру слота, делённый на 11.
inline RateSeries seriesFromHand(const std::vector<HookMonitor::Event>& events) {
    RateSeries s;
    if (events.size() < 2) return s;
    const double ticksPerMs = static_cast<double>(Clock::freq()) / 1000.0;
    const int64_t t0 = events.front().t;
    const auto n = static_cast<size_t>(static_cast<double>(events.back().t - t0) / ticksPerMs) + 1;
    std::vector<double> dx(n, 0.0);
    std::vector<double> dy(n, 0.0);
    s.hid.assign(n, 0);
    for (size_t i = 0; i < events.size(); ++i) {
        const auto slot = std::min(n - 1, static_cast<size_t>(static_cast<double>(events[i].t - t0) / ticksPerMs));
        if (s.hid[slot] < 65535) ++s.hid[slot];
        if (i > 0) {
            dx[slot] += events[i].x - events[i - 1].x;
            dy[slot] += events[i].y - events[i - 1].y;
        }
        s.events.emplace_back(events[i].t, static_cast<int32_t>(slot));
    }
    constexpr int kHalf = 5;
    std::vector<double> px(n + 1, 0.0);
    std::vector<double> py(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) {
        px[i + 1] = px[i] + dx[i];
        py[i + 1] = py[i] + dy[i];
    }
    s.speed.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t a = i >= kHalf ? i - kHalf : 0;
        const size_t b = std::min(n, i + kHalf + 1);
        const double w = static_cast<double>(b - a);
        s.speed[i] = static_cast<float>(std::hypot(px[b] - px[a], py[b] - py[a]) / w);
    }
    return s;
}

inline void printRateTable(const RateSeries& s, const char* title) {
    // Корзины с центрами на ступеньках лестницы kStepSpeeds (каждая ступенька -
    // в своей корзине), границы посередине между ними.
    static constexpr std::array<double, 12> kEdges{0.01, 0.075, 0.15, 0.25, 0.4, 0.6, 0.85, 1.25, 1.75, 2.5, 4.0, 6.5};
    static constexpr std::array<const char*, 13> kNames{
        "покой", "~0.05", "~0.1", "~0.2", "~0.3", "~0.5", "~0.7", "~1", "~1.5", "~2", "~3", "~5", ">= 6.5"};
    auto binOf = [](double v) {
        size_t b = 0;
        while (b < kEdges.size() && v >= kEdges[b]) ++b;
        return b;
    };
    struct Bin {
        uint64_t slots = 0;
        uint64_t reports = 0;
        uint64_t hid = 0;
        std::vector<float> intervalsMs;
    };
    std::array<Bin, kNames.size()> bins{};
    for (size_t i = 0; i < s.speed.size(); ++i) {
        Bin& b = bins[binOf(s.speed[i])];
        ++b.slots;
        b.hid += s.hid[i];
        if (!s.report.empty()) b.reports += s.report[i];
    }
    // Интервал засчитывается корзине, если оба соседних события в ней и между
    // ними нет покоя (иначе пауза между рывками выглядела бы как "интервал").
    std::vector<uint32_t> restBefore(s.speed.size() + 1, 0);  // слотов покоя в [0, i)
    for (size_t i = 0; i < s.speed.size(); ++i) {
        restBefore[i + 1] = restBefore[i] + (binOf(s.speed[i]) == 0 ? 1 : 0);
    }
    for (size_t k = 1; k < s.events.size(); ++k) {
        const int32_t a = s.events[k - 1].second;
        const int32_t c = s.events[k].second;
        if (a < 0 || c <= a) continue;
        const size_t ba = binOf(s.speed[a]);
        if (ba == 0 || ba != binOf(s.speed[c]) || restBefore[c] != restBefore[a + 1]) continue;
        bins[ba].intervalsMs.push_back(
            static_cast<float>(Clock::toUs(s.events[k].first - s.events[k - 1].first) / 1000.0));
    }

    const bool hasReports = !s.report.empty();
    std::printf("\n  %s: частота по скорости (|смещение| за 1 мс, отсчётов/мс; корзины по ступенькам F10)\n", title);
    std::printf("  скорость  | время, с | генератор, Гц | HID, Гц | интервал: медиана, мс | разброс CV |"
                "  1 мс   2 мс   3 мс  4-5 мс  6+ мс\n");
    for (size_t i = 0; i < bins.size(); ++i) {
        Bin& b = bins[i];
        if (b.slots < 200) continue;  // меньше 0.2 с в корзине - шум
        const double sec = static_cast<double>(b.slots) / 1000.0;
        char gen[16] = "-";
        if (hasReports) std::snprintf(gen, sizeof(gen), "%.0f", static_cast<double>(b.reports) / sec);
        if (i == 0) b.intervalsMs.clear();  // в покое интервалы не считаем
        double median = 0.0;
        double cv = 0.0;
        std::array<double, 5> share{};
        if (!b.intervalsMs.empty()) {
            auto& v = b.intervalsMs;
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
            median = v[v.size() / 2];
            double sum = 0.0;
            double sq = 0.0;
            for (float x : v) {
                sum += x;
                sq += static_cast<double>(x) * x;
                const size_t k = x < 1.5f ? 0 : x < 2.5f ? 1 : x < 3.5f ? 2 : x < 5.5f ? 3 : 4;
                share[k] += 1.0;
            }
            const double mean = sum / static_cast<double>(v.size());
            cv = mean > 0.0 ? std::sqrt(std::max(0.0, sq / static_cast<double>(v.size()) - mean * mean)) / mean : 0.0;
            for (double& x : share) x = 100.0 * x / static_cast<double>(v.size());
        }
        // printf выравнивает по байтам, а кириллица в UTF-8 - по 2 байта на букву.
        size_t width = 0;
        for (const char* q = kNames[i]; *q; ++q) width += (static_cast<unsigned char>(*q) & 0xC0) != 0x80;
        std::printf("  %s%*s | %8.1f | %13s | %7.0f | %21.2f | %10.2f | %5.1f%% %5.1f%% %5.1f%% %6.1f%% %5.1f%%\n",
            kNames[i], static_cast<int>(width < 9 ? 9 - width : 0), "", sec, gen, static_cast<double>(b.hid) / sec, median, cv, share[0], share[1], share[2], share[3],
            share[4]);
    }
}

inline bool writeRateCsv(const RateSeries& s, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "ms,speed,report,hid\n");
    for (size_t i = 0; i < s.speed.size(); ++i) {
        std::fprintf(f, "%zu,%.4f,%d,%u\n", i, static_cast<double>(s.speed[i]), s.report.empty() ? -1 : s.report[i],
            static_cast<unsigned>(s.hid[i]));
    }
    std::fclose(f);
    return true;
}

}  // namespace mover
