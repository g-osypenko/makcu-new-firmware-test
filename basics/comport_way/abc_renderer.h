// Обёртка над нативным ABCurves Renderer (ABCurves/runtime/c, модель
// models/renderer_global_h80.bin).
//
// Что делает рендерер: превращает гладкое намерение движения (дробное
// смещение за каждую 1 мс) в целочисленные отчёты с "текстурой" настоящего
// сенсора мыши - когда отчёт вылетает, как квантуются отсчёты, микродрожание.
// Остаток (дробная часть) копится внутри, суммарный путь сохраняется.
//
// Оси: рендерер работает в каноническом пространстве X вправо / Y ВВЕРХ.
// HID и экран - Y ВНИЗ. Обёртка переворачивает Y на входе и выходе, снаружи
// всё в координатах мыши (Y вниз).
//
// Жизненный цикл (по runtime/c/README.md):
//   модель (44 484 байта, должна жить всё время) -> профиль из ровно 256
//   настоящих 1-мс отчётов (неизменяемый шаблон) -> на каждый прогон копия
//   шаблона + begin(seed) -> step() раз в 1 мс.
#pragma once

extern "C" {
#include "abc_online.h"
}

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace mover {

struct Report16 {
    int16_t dx;
    int16_t dy;
};

class AbcRenderer {
public:
    static constexpr int kProfileTicks = 256;

    bool loadModel(const std::string& path, std::string& error) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            error = "не открыт файл модели: " + path;
            return false;
        }
        blob_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (blob_.size() != ABC_ONLINE_BLOB_BYTES) {
            error = "размер модели " + std::to_string(blob_.size()) + " байт, ожидается " +
                    std::to_string(ABC_ONLINE_BLOB_BYTES);
            return false;
        }
        model_ = std::make_unique<abc_online_model_t>();
        // Загрузчик сам проверяет CRC, идентичность и порядок API.
        const int st = abc_online_model_init(model_.get(), blob_.data(), blob_.size());
        if (st != ABC_FIXED_OK) {
            error = "abc_online_model_init: статус " + std::to_string(st);
            model_.reset();
            return false;
        }
        return true;
    }

    // CSV: строки "dx,dy" (оси мыши, Y вниз), '#' - комментарий. Ровно 256.
    static bool readProfileCsv(const std::string& path, std::vector<Report16>& out, std::string& error) {
        std::ifstream f(path);
        if (!f) {
            error = "не открыт профиль: " + path;
            return false;
        }
        out.clear();
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            int dx = 0;
            int dy = 0;
            char comma = 0;
            std::istringstream ss(line);
            if (!(ss >> dx >> comma >> dy) || comma != ',' || dx < -32768 || dx > 32767 || dy < -32768 ||
                dy > 32767) {
                error = "плохая строка профиля: '" + line + "'";
                return false;
            }
            out.push_back({static_cast<int16_t>(dx), static_cast<int16_t>(dy)});
        }
        if (out.size() != kProfileTicks) {
            error = "в профиле " + std::to_string(out.size()) + " отчётов, нужно ровно 256";
            return false;
        }
        return true;
    }

    // Подготовка неизменяемого шаблона (~3 мс, вне горячего пути).
    bool prepareProfile(const std::vector<Report16>& reports, std::string& error) {
        if (!model_) {
            error = "модель не загружена";
            return false;
        }
        if (reports.size() != kProfileTicks) {
            error = "нужно ровно 256 отчётов";
            return false;
        }
        profile_ = std::make_unique<abc_online_renderer_t>();
        int st = abc_online_reset(profile_.get(), model_.get());
        for (size_t i = 0; st == ABC_FIXED_OK && i < reports.size(); ++i) {
            // Y вниз (мышь) -> Y вверх (канон).
            st = abc_online_observe_raw(profile_.get(), reports[i].dx, static_cast<int16_t>(-reports[i].dy));
        }
        if (st != ABC_FIXED_OK) {
            error = "подготовка профиля: статус " + std::to_string(st);
            profile_.reset();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ready() const { return profile_ != nullptr; }

    // Новый независимый поток из шаблона (один на прогон).
    bool beginEvent(uint64_t seed, std::string& error) {
        if (!profile_) {
            error = "профиль не подготовлен";
            return false;
        }
        event_ = std::make_unique<abc_online_renderer_t>(*profile_);
        const int st = abc_online_begin(event_.get(), seed);
        if (st != ABC_FIXED_OK) {
            error = "abc_online_begin: статус " + std::to_string(st);
            event_.reset();
            return false;
        }
        return true;
    }

    // Один тик 1 мс. Вход - гладкое смещение за этот тик (пикс/отсчёты,
    // оси мыши). Выход - целочисленный отчёт (часто 0,0 - это нормально).
    bool step(double smoothDx, double smoothDy, Report16& out) {
        if (!event_) return false;
        const double qx = std::nearbyint(smoothDx * 65536.0);
        const double qy = std::nearbyint(-smoothDy * 65536.0);
        if (std::fabs(qx) >= 2147483647.0 || std::fabs(qy) >= 2147483647.0) return false;
        abc_fixed_report_t r{};
        if (abc_online_step(event_.get(), static_cast<int32_t>(qx), static_cast<int32_t>(qy), &r) != ABC_FIXED_OK) {
            return false;
        }
        out.dx = r.dx;
        out.dy = static_cast<int16_t>(-r.dy);
        return true;
    }

private:
    std::vector<uint8_t> blob_;  // должен жить дольше модели и всех потоков
    std::unique_ptr<abc_online_model_t> model_;
    std::unique_ptr<abc_online_renderer_t> profile_;
    std::unique_ptr<abc_online_renderer_t> event_;
};

}  // namespace mover
