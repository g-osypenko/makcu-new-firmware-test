// Сверка пути через Raw Input: сумма дельт, реально пришедших в Windows,
// против суммы отправленных команд (0 потерь / 0 дублей).
//
// ВАЖНО: частоту здесь НЕ меряем. Windows 11 ограничивает Raw Input для
// фоновых окон (~125 Гц, сообщения склеиваются), а консоль во время теста
// всегда в фоне - счётчик отчётов врёт. Поллинг смотрите внешним тестером
// в фокусе. Сумма дельт при склейке сохраняется.
#pragma once

#include "mover_common.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace mover {

class RawInputMonitor {
public:
    struct DeviceStats {
        HANDLE handle = nullptr;
        std::string name;
        uint64_t reports = 0;
        int64_t sumX = 0;
        int64_t sumY = 0;
        int64_t sumAbs = 0;  // длина пути |dx|+|dy| (при склейке сообщений почти точна)
    };

    ~RawInputMonitor() { stop(); }

    bool start() {
        if (thread_.joinable()) {
            return true;
        }
        ready_ = false;
        thread_ = std::thread([this] { threadMain(); });
        for (int i = 0; i < 200 && !ready_; ++i) {
            Sleep(5);
        }
        return ok_;
    }

    void stop() {
        if (!thread_.joinable()) {
            return;
        }
        if (HWND hwnd = hwnd_.load()) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        thread_.join();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        devices_.clear();
    }

    [[nodiscard]] std::vector<DeviceStats> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return devices_;
    }

    // Устройство с наибольшим числом сообщений с момента reset().
    [[nodiscard]] std::optional<DeviceStats> primary() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (devices_.empty()) {
            return std::nullopt;
        }
        return *std::max_element(devices_.begin(), devices_.end(),
            [](const DeviceStats& a, const DeviceStats& b) { return a.reports < b.reports; });
    }

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<RawInputMonitor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_INPUT && self) {
            self->onInput(reinterpret_cast<HRAWINPUT>(lp));
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        if (msg == WM_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (msg == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void threadMain() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &RawInputMonitor::wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MakcuMoverRawInputSink";
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
            nullptr, wc.hInstance, nullptr);
        if (!hwnd) {
            ok_ = false;
            ready_ = true;
            return;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01;  // Generic Desktop
        rid.usUsage = 0x02;      // Mouse
        rid.dwFlags = RIDEV_INPUTSINK;  // получать ввод и в фоне
        rid.hwndTarget = hwnd;
        ok_ = RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE;
        hwnd_ = hwnd;
        ready_ = true;

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            DispatchMessageW(&msg);
        }
        rid.dwFlags = RIDEV_REMOVE;
        rid.hwndTarget = nullptr;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        hwnd_ = nullptr;
    }

    void onInput(HRAWINPUT input) {
        RAWINPUT raw{};
        UINT size = sizeof(raw);
        if (GetRawInputData(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
            return;
        }
        if (raw.header.dwType != RIM_TYPEMOUSE) {
            return;
        }
        if (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
            return;  // планшеты/RDP - не наш случай
        }
        std::lock_guard<std::mutex> lock(mutex_);
        DeviceStats* dev = nullptr;
        for (auto& d : devices_) {
            if (d.handle == raw.header.hDevice) {
                dev = &d;
                break;
            }
        }
        if (!dev) {
            devices_.push_back(DeviceStats{});
            dev = &devices_.back();
            dev->handle = raw.header.hDevice;
            dev->name = deviceName(raw.header.hDevice);
        }
        ++dev->reports;
        dev->sumX += raw.data.mouse.lLastX;
        dev->sumY += raw.data.mouse.lLastY;
        dev->sumAbs += std::abs(raw.data.mouse.lLastX) + std::abs(raw.data.mouse.lLastY);
    }

    static std::string deviceName(HANDLE h) {
        UINT chars = 0;
        GetRawInputDeviceInfoA(h, RIDI_DEVICENAME, nullptr, &chars);
        if (chars == 0) {
            return "unknown";
        }
        std::string name(chars, '\0');
        GetRawInputDeviceInfoA(h, RIDI_DEVICENAME, name.data(), &chars);
        name.resize(std::strlen(name.c_str()));
        // Оставляем только VID/PID для читаемости.
        std::string upper = name;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        const auto vid = upper.find("VID_");
        if (vid != std::string::npos && vid + 17 <= upper.size()) {
            return upper.substr(vid, 17);
        }
        return name.size() > 40 ? name.substr(0, 40) : name;
    }

    std::thread thread_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> ok_{false};
    std::atomic<HWND> hwnd_{nullptr};
    mutable std::mutex mutex_;
    std::vector<DeviceStats> devices_;
};

}  // namespace mover
