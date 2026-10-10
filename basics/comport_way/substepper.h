// Субстеппер с накопителем (без рендерера, без шума).
//
// Вход - гладкое намерение за тик 1 мс (дробные отсчёты, оси мыши, Y вниз).
// Дробная часть копится по каждой оси и выходит целым отчётом, когда накопится
// до 0.5 отсчёта (округление к ближайшему). Путь сохраняется точно, остаток
// никогда не больше 0.5 отсчёта на ось.
//
// Частоту отчётов здесь задаёт только скорость (отправляются ненулевые отчёты):
//   >= ~1 отсчёта/мс по оси - отчёт каждую мс (1000 Гц);
//   0.25 отсчёта/мс        - отчёт ровно каждые 4 мс (250 Гц), интервалы ровные;
//   по диагонали оси квантуются независимо - отчётов больше, чем по одной оси.
// Это эталон "чистого квантования": с ним сравнивается рендерер и рука.
#pragma once

#include "abc_renderer.h"  // Report16

#include <algorithm>
#include <cmath>

namespace mover {

class Accumulator {
public:
    void reset() {
        accX_ = 0.0;
        accY_ = 0.0;
    }

    Report16 step(double dx, double dy) {
        accX_ += dx;
        accY_ += dy;
        // std::round: половина - от нуля, чтобы 0.25 отсчёта/мс давало ровно 1 отчёт на 4 мс.
        const double ox = std::clamp(std::round(accX_), -32767.0, 32767.0);
        const double oy = std::clamp(std::round(accY_), -32767.0, 32767.0);
        accX_ -= ox;
        accY_ -= oy;
        return {static_cast<int16_t>(ox), static_cast<int16_t>(oy)};
    }

    [[nodiscard]] double debtX() const { return accX_; }
    [[nodiscard]] double debtY() const { return accY_; }

private:
    double accX_ = 0.0;
    double accY_ = 0.0;
};

}  // namespace mover
