// Проверка ревизии 3 makcu_raw_transport.h (COM) на железе: инжект кнопок, блокировки,
// переподключение, долгий прогон. Все клики уходят в своё тестовое окно: курсор заперт
// в нём (ClipCursor), поэтому ничего вокруг не нажимается. Ctrl+C - корректный выход
// (транспорт отпускает кнопки и снимает блокировки). Esc (из любого окна) или крестик
// тестового окна - тоже корректный выход: курсор заперт в окне, а консоль может быть не видна.
//
//   controls_test.exe [--port COM5] MODE
//     --clicks         каждая кнопка 5 раз: press -> 40 мс -> release. Windows должна увидеть
//                      столько же нажатий и отпусканий; задержка press() -> событие в Windows.
//     --locks S        S секунд заблокированы side1, side2, middle: жмите их - транспорт видит
//                      физику, Windows - нет. Потом всё снимается.
//     --axes S         блокировка осей: 5 фаз по S секунд (ось X, ось Y, только +X, только -Y,
//                      ничего). Водите мышью во все стороны: Windows не должен получить ни одного
//                      отсчёта в заблокированном направлении.
//     --reconnect S    S секунд движения по кругу + блокировка side2; выдерните кабель USB2 (COM,
//                      средний разъём) на 3-5 с и вставьте обратно: обрыв, переподключение,
//                      блокировка на месте.
//     --draw S         наглядно: S секунд - зажать левую, провести круг (в окне рисуется
//                      окружность), отпустить; потом по цветной точке от каждой другой кнопки.
//     --watch S        S секунд только смотреть: статус в окне, жмите физические кнопки сами.
//     --soak M         M минут: каждая минута = 35 с движения 1 кГц с кликами и блокировками,
//                      20 с простоя, возобновление (задержка первого отчёта после простоя).
//                      Журнал по минутам: HID, интервалы, путь, клики, связь, память, хэндлы.
//
// Сборка: build_example.bat (собирает и пример, и этот тест).
#include "makcu_raw_transport.h"

#include <psapi.h>
#include <timeapi.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using makcu::LockTarget;
using makcu::MouseButton;
using Q = makcu::detail::Qpc;

std::atomic<bool> g_stop{false};

BOOL WINAPI onCtrl(DWORD) {
    g_stop = true;
    return TRUE;
}

// ---- тестовое окно: счётчики того, что увидел Windows ---------------------------------
struct Ui {
    HWND hwnd = nullptr;
    RECT clip{};
    std::atomic<bool> ready{false};
    std::atomic<uint64_t> moves{0}, oneMs{0};
    std::atomic<int64_t> lastMoveT{0};
    std::atomic<int64_t> firstMoveAfter{0};  // время первого отчёта движения после метки
    std::atomic<int64_t> mark{0};
    std::array<std::atomic<uint32_t>, 5> down{}, up{};
    std::array<std::atomic<int64_t>, 5> lastDownT{};
    std::atomic<int64_t> rawX{0}, rawY{0};
    std::atomic<int64_t> xPos{0}, xNeg{0}, yPos{0}, yNeg{0};  // путь по знаку, модуль (--axes)
};
Ui g_ui;
std::atomic<const wchar_t*> g_hint{nullptr};  // подсказка фазы теста (красным в окне)
HHOOK g_hook = nullptr;
makcu::RawTransport* g_tx = nullptr;  // для статуса в окне

// Рисование в окне: след зажатой левой и цветные точки остальных кнопок.
constexpr int kStatusHeight = 64;
POINT g_last{};
bool g_drawing = false;
const COLORREF kButtonColor[5] = {RGB(0, 0, 0), RGB(220, 30, 30), RGB(30, 80, 230), RGB(20, 160, 60), RGB(240, 140, 0)};

void dot(HWND h, LPARAM lp, int button) {
    HDC dc = GetDC(h);
    HBRUSH br = CreateSolidBrush(kButtonColor[button]);
    HGDIOBJ old = SelectObject(dc, br);
    const int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
    Ellipse(dc, x - 6, y - 6, x + 7, y + 7);
    SelectObject(dc, old);
    DeleteObject(br);
    ReleaseDC(h, dc);
}

void drawStatus(HDC dc, HWND h) {
    RECT r;
    GetClientRect(h, &r);
    r.bottom = kStatusHeight;
    FillRect(dc, &r, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    wchar_t line[256];
    const wchar_t* names[5] = {L"L", L"R", L"M", L"S1", L"S2"};
    auto maskText = [&](unsigned m, wchar_t* out, size_t cap) {
        out[0] = 0;
        for (int i = 0; i < 5; ++i) {
            if (m & (1u << i)) {
                wcsncat(out, names[i], cap - wcslen(out) - 1);
                wcsncat(out, L" ", cap - wcslen(out) - 1);
            }
        }
        if (!out[0]) wcsncpy(out, L"-", cap);
    };
    wchar_t phys[32], inj[32];
    const bool conn = g_tx && g_tx->connected();
    maskText(g_tx ? g_tx->buttons() : 0, phys, 32);
    maskText(g_tx ? g_tx->injectedButtons() : 0, inj, 32);
    swprintf(line, 256, L"связь: %ls | физика (MAKCU): %ls | инжект: %ls | блокировки: %04X   (Esc - выход)",
        conn ? L"есть" : L"НЕТ", phys, inj, g_tx ? g_tx->locks() : 0);
    TextOutW(dc, 8, 6, line, static_cast<int>(wcslen(line)));
    swprintf(line, 256, L"Windows получил нажатий: L %u  R %u  M %u  S1 %u  S2 %u   |   HID-отчётов движения: %llu",
        g_ui.down[0].load(), g_ui.down[1].load(), g_ui.down[2].load(), g_ui.down[3].load(), g_ui.down[4].load(),
        static_cast<unsigned long long>(g_ui.moves.load()));
    TextOutW(dc, 8, 26, line, static_cast<int>(wcslen(line)));
    const wchar_t* hint = g_hint.load();
    const wchar_t* legend = hint ? hint : L"след левой - чёрный, точки: R красная, M синяя, S1 зелёная, S2 оранжевая";
    const COLORREF oldColor = SetTextColor(dc, hint ? RGB(200, 0, 0) : GetSysColor(COLOR_WINDOWTEXT));
    TextOutW(dc, 8, 46, legend, static_cast<int>(wcslen(legend)));
    SetTextColor(dc, oldColor);
}

int buttonFromMsg(WPARAM wp, const MSLLHOOKSTRUCT* info, bool& isDown) {
    switch (wp) {
    case WM_LBUTTONDOWN: isDown = true; return 0;
    case WM_LBUTTONUP: isDown = false; return 0;
    case WM_RBUTTONDOWN: isDown = true; return 1;
    case WM_RBUTTONUP: isDown = false; return 1;
    case WM_MBUTTONDOWN: isDown = true; return 2;
    case WM_MBUTTONUP: isDown = false; return 2;
    case WM_XBUTTONDOWN: isDown = true; return HIWORD(info->mouseData) == XBUTTON1 ? 3 : 4;
    case WM_XBUTTONUP: isDown = false; return HIWORD(info->mouseData) == XBUTTON1 ? 3 : 4;
    default: return -1;
    }
}

LRESULT CALLBACK hookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (!(info->flags & LLMHF_INJECTED)) {  // MAKCU - железо, SendInput не считаем
            const int64_t now = Q::now();
            if (wp == WM_MOUSEMOVE) {
                const int64_t prev = g_ui.lastMoveT.exchange(now);
                g_ui.moves.fetch_add(1);
                if (prev) {
                    const double us = Q::toUs(now - prev);
                    if (us >= 750.0 && us < 1250.0) g_ui.oneMs.fetch_add(1);
                }
                int64_t zero = 0;
                if (g_ui.mark.load() && now >= g_ui.mark.load()) g_ui.firstMoveAfter.compare_exchange_strong(zero, now);
            } else {
                bool isDown = false;
                const int b = buttonFromMsg(wp, info, isDown);
                if (b >= 0) {
                    if (isDown) {
                        g_ui.down[b].fetch_add(1);
                        g_ui.lastDownT[b].store(now);
                    } else {
                        g_ui.up[b].fetch_add(1);
                    }
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_INPUT: {
        RAWINPUT raw{};
        UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) !=
                static_cast<UINT>(-1) &&
            raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            g_ui.rawX.fetch_add(raw.data.mouse.lLastX);
            g_ui.rawY.fetch_add(raw.data.mouse.lLastY);
            const LONG dx = raw.data.mouse.lLastX, dy = raw.data.mouse.lLastY;
            if (dx > 0) g_ui.xPos.fetch_add(dx);
            if (dx < 0) g_ui.xNeg.fetch_add(-dx);
            if (dy > 0) g_ui.yPos.fetch_add(dy);
            if (dy < 0) g_ui.yNeg.fetch_add(-dy);
        }
        return DefWindowProcW(h, m, wp, lp);
    }
    case WM_TIMER: {
        ClipCursor(&g_ui.clip);  // Windows снимает ограничение при смене активного окна
        HDC dc = GetDC(h);
        drawStatus(dc, h);
        ReleaseDC(h, dc);
        return 0;
    }
    case WM_LBUTTONDOWN:
        g_drawing = true;
        g_last = POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        dot(h, lp, 0);
        return 0;
    case WM_MOUSEMOVE:
        if (g_drawing && (wp & MK_LBUTTON)) {
            HDC dc = GetDC(h);
            HPEN pen = CreatePen(PS_SOLID, 3, kButtonColor[0]);
            HGDIOBJ old = SelectObject(dc, pen);
            MoveToEx(dc, g_last.x, g_last.y, nullptr);
            g_last = POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            LineTo(dc, g_last.x, g_last.y);
            SelectObject(dc, old);
            DeleteObject(pen);
            ReleaseDC(h, dc);
        }
        return 0;
    case WM_LBUTTONUP:
        g_drawing = false;
        return 0;
    case WM_RBUTTONDOWN: dot(h, lp, 1); return 0;
    case WM_MBUTTONDOWN: dot(h, lp, 2); return 0;
    case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK:
        return 0;
    case WM_XBUTTONDOWN:
        dot(h, lp, HIWORD(wp) == XBUTTON1 ? 3 : 4);
        return TRUE;
    case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        return TRUE;  // не превращать в WM_APPCOMMAND ("назад" в браузере)
    case WM_APPCOMMAND:
    case WM_CONTEXTMENU:
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        drawStatus(dc, h);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        g_stop = true;
        return 0;
    default:
        return DefWindowProcW(h, m, wp, lp);
    }
}

void uiThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MakcuControlsTest";
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    const int w = 760, h = 520;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_ui.hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"MAKCU controls test (Esc - выход)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    RECT client;
    GetClientRect(g_ui.hwnd, &client);
    POINT tl{client.left + 20, client.top + kStatusHeight + 10}, br{client.right - 20, client.bottom - 20};
    ClientToScreen(g_ui.hwnd, &tl);
    ClientToScreen(g_ui.hwnd, &br);
    g_ui.clip = RECT{tl.x, tl.y, br.x, br.y};
    SetForegroundWindow(g_ui.hwnd);
    SetCursorPos((tl.x + br.x) / 2, (tl.y + br.y) / 2);
    ClipCursor(&g_ui.clip);
    SetTimer(g_ui.hwnd, 1, 200, nullptr);
    RAWINPUTDEVICE rid{0x01, 0x02, RIDEV_INPUTSINK, g_ui.hwnd};
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, hookProc, GetModuleHandleW(nullptr), 0);
    g_ui.ready = true;
    MSG msg;
    while (!g_stop.load()) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    }
    ClipCursor(nullptr);
    if (g_hook) UnhookWindowsHookEx(g_hook);
    DestroyWindow(g_ui.hwnd);
}

// ---- движение: круг на сетке 1 мс ------------------------------------------------------
struct Mover {
    makcu::RawTransport& tx;
    double phase = 0.0, fx = 0.0, fy = 0.0;
    int64_t pendX = 0, pendY = 0, sentX = 0, sentY = 0;
    uint64_t accepted = 0, ticks = 0;
    int64_t firstAcceptT = 0;
    explicit Mover(makcu::RawTransport& t) : tx(t) {}
    // Один тик: круг радиусом 120 отсчётов, оборот за 300 мс: ~2.5 отсчёта/мс, поэтому каждый
    // тик несёт движение (медленнее - часть тиков округлится в 0 и HID будет ниже 1000 Гц).
    void tick() {
        const double r = 120.0, w = 2.0 * 3.14159265358979 / 300.0;
        fx += -r * w * std::sin(phase);
        fy += r * w * std::cos(phase);
        phase += w;
        const int64_t ix = static_cast<int64_t>(std::llround(fx)), iy = static_cast<int64_t>(std::llround(fy));
        fx -= static_cast<double>(ix);
        fy -= static_cast<double>(iy);
        pendX += ix;
        pendY += iy;
        ++ticks;
        if ((pendX || pendY) && tx.move(static_cast<int16_t>(pendX), static_cast<int16_t>(pendY))) {
            if (!firstAcceptT) firstAcceptT = Q::now();
            sentX += pendX;
            sentY += pendY;
            pendX = pendY = 0;
            ++accepted;
        } else if (!tx.connected()) {
            pendX = pendY = 0;  // связи нет: накопленное устарело, не копить рывок
        }
    }
};

// Esc из любого окна - выход (проверка не чаще раза в 20 мс).
void checkEsc() {
    static int64_t next = 0;
    const int64_t now = Q::now();
    if (now < next) return;
    next = now + Q::fromUs(20000.0);
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) g_stop = true;
}

void spinUntil(int64_t t) {
    checkEsc();
    while (Q::now() < t && !g_stop.load()) YieldProcessor();
}

double processMemoryMb() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
}
DWORD processHandles() {
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return n;
}

const MouseButton kAll[] = {MouseButton::Left, MouseButton::Right, MouseButton::Middle, MouseButton::Side1, MouseButton::Side2};

// ---- режимы ----------------------------------------------------------------------------
bool runClicks(makcu::RawTransport& tx) {
    std::printf(">>> клики: каждая кнопка 5 раз, удержание 40 мс\n");
    g_hint = L"НЕ ТРОГАЙТЕ МЫШЬ: кнопки нажимаются программно";
    bool ok = true;
    for (MouseButton b : kAll) {
        const int i = static_cast<int>(b);
        const uint32_t d0 = g_ui.down[i].load(), u0 = g_ui.up[i].load();
        double latSum = 0.0, latMax = 0.0;
        int latN = 0;
        for (int k = 0; k < 5 && !g_stop.load(); ++k) {
            const int64_t t0 = Q::now();
            const int64_t before = g_ui.lastDownT[i].load();
            tx.press(b);
            while (g_ui.lastDownT[i].load() == before && Q::toUs(Q::now() - t0) < 200000.0) Sleep(0);
            if (g_ui.lastDownT[i].load() != before) {
                const double lat = Q::toUs(g_ui.lastDownT[i].load() - t0) / 1000.0;
                latSum += lat;
                latMax = std::max(latMax, lat);
                ++latN;
            }
            spinUntil(t0 + Q::fromUs(40000.0));
            tx.release(b);
            Sleep(150);
        }
        Sleep(100);
        const uint32_t dn = g_ui.down[i].load() - d0, upn = g_ui.up[i].load() - u0;
        const bool good = dn == 5 && upn == 5;
        ok = ok && good;
        std::printf("  %-6s: нажатий в Windows %u/5, отпусканий %u/5, задержка press()->Windows ср %.2f max %.2f мс %s\n",
            makcu::buttonName(b), dn, upn, latN ? latSum / latN : 0.0, latMax, good ? "OK" : "<-- РАСХОЖДЕНИЕ");
    }
    return ok;
}

bool runLocks(makcu::RawTransport& tx, double seconds) {
    const LockTarget targets[] = {LockTarget::Side1, LockTarget::Side2, LockTarget::Middle};
    for (LockTarget t : targets) tx.setLock(t, true);
    Sleep(50);
    const auto s0 = tx.stats();
    std::printf(">>> блокировки: side1, side2, middle заблокированы (на плате %04X). %.0f с: жмите их и другие кнопки\n",
        s0.locksOnBoard, seconds);
    uint32_t d0[5], p0[5];
    for (int i = 0; i < 5; ++i) {
        d0[i] = g_ui.down[i].load();
        p0[i] = tx.pressCount(kAll[i]);
    }
    const int64_t until = Q::now() + Q::fromUs(seconds * 1e6);
    uint8_t last = 0xFF;
    while (Q::now() < until && !g_stop.load()) {
        checkEsc();
        const uint8_t m = tx.buttons();
        if (m != last) {
            std::printf("  физика (транспорт): маска %02X\n", m);
            last = m;
        }
        Sleep(2);
    }
    bool ok = true;
    std::printf("  итог: кнопка | нажатий видит транспорт | дошло до Windows\n");
    for (int i = 0; i < 5; ++i) {
        const uint32_t phys = tx.pressCount(kAll[i]) - p0[i], win = g_ui.down[i].load() - d0[i];
        const bool lockedBtn = i >= 2;
        if (lockedBtn && win != 0) ok = false;
        std::printf("    %-6s%s | %3u | %3u %s\n", makcu::buttonName(kAll[i]), lockedBtn ? " (блок)" : "       ", phys, win,
            lockedBtn ? (win == 0 ? "OK: заблокирована" : "<-- ПРОШЛА В WINDOWS") : "");
    }
    tx.unlockAll();
    Sleep(50);
    std::printf("  блокировки сняты: на плате %04X\n", tx.stats().locksOnBoard);
    return ok;
}

// Блокировка осей: по фазе на цель, пользователь водит мышью во все стороны. Raw Input считает
// физическое движение, дошедшее до Windows, отдельно по знаку: у заблокированного направления 0.
bool runAxes(makcu::RawTransport& tx, double seconds) {
    struct Phase {
        const char* name;
        const wchar_t* hint;
        int target;   // -1 - ничего не заблокировано
        int blocked;  // 0 ось X, 1 ось Y, 2 только +X, 3 только -Y, -1 нет
    };
    const Phase phases[] = {
        {"ось X", L"ЗАБЛОКИРОВАНА ОСЬ X: водите мышью во все стороны - курсор только вверх-вниз",
            static_cast<int>(LockTarget::X), 0},
        {"ось Y", L"ЗАБЛОКИРОВАНА ОСЬ Y: водите мышью во все стороны - курсор только влево-вправо",
            static_cast<int>(LockTarget::Y), 1},
        {"+X (вправо)", L"ЗАБЛОКИРОВАНО ВПРАВО (+X): водите во все стороны - вправо курсор не идёт",
            static_cast<int>(LockTarget::XPos), 2},
        {"-Y (вверх)", L"ЗАБЛОКИРОВАНО ВВЕРХ (-Y): водите во все стороны - вверх курсор не идёт",
            static_cast<int>(LockTarget::YNeg), 3},
        {"ничего", L"ВСЁ СНЯТО: водите мышью - курсор ходит во все стороны", -1, -1},
    };
    std::printf(">>> блокировка осей: 5 фаз по %.0f с. В каждой водите мышью во все стороны (подсказка в окне)\n", seconds);
    std::printf("  фаза          | плата  | вправо  влево  вниз  вверх (отсчёты в Windows) | итог\n");
    bool ok = true;
    for (const Phase& ph : phases) {
        if (g_stop.load()) break;
        tx.unlockAll();
        if (ph.target >= 0) tx.setLock(static_cast<LockTarget>(ph.target), true);
        g_hint = ph.hint;
        Sleep(300);  // команда дошла, сверена снимком; рука успевает понять фазу
        const int64_t xp0 = g_ui.xPos.load(), xn0 = g_ui.xNeg.load(), yp0 = g_ui.yPos.load(), yn0 = g_ui.yNeg.load();
        const int64_t until = Q::now() + Q::fromUs(seconds * 1e6);
        while (Q::now() < until && !g_stop.load()) {
            checkEsc();
            tx.service();
            Sleep(5);
        }
        const int64_t xp = g_ui.xPos.load() - xp0, xn = g_ui.xNeg.load() - xn0;
        const int64_t yp = g_ui.yPos.load() - yp0, yn = g_ui.yNeg.load() - yn0;
        const int64_t kMoved = 50;  // столько отсчётов - "двигали"
        bool blockedOk = true, movedOk = true;
        switch (ph.blocked) {
        case 0: blockedOk = xp == 0 && xn == 0; movedOk = yp > kMoved && yn > kMoved; break;
        case 1: blockedOk = yp == 0 && yn == 0; movedOk = xp > kMoved && xn > kMoved; break;
        case 2: blockedOk = xp == 0; movedOk = xn > kMoved; break;
        case 3: blockedOk = yn == 0; movedOk = yp > kMoved; break;
        default: movedOk = xp > kMoved && xn > kMoved && yp > kMoved && yn > kMoved; break;
        }
        const char* verdict = !blockedOk ? "<-- ПРОШЛО ЧЕРЕЗ БЛОКИРОВКУ"
                              : !movedOk ? "? мало движения - не проверено"
                              : ph.blocked < 0 ? "OK: всё проходит" : "OK: заблокировано";
        if (!blockedOk || !movedOk) ok = false;
        std::string name = ph.name;  // дополнить пробелами по символам, а не байтам (UTF-8)
        size_t chars = 0;
        for (const unsigned char c : name) chars += (c & 0xC0) != 0x80;
        name.append(chars < 13 ? 13 - chars : 0, ' ');
        std::printf("  %s | %04X  | %6lld %6lld %6lld %6lld | %s\n", name.c_str(), tx.locksOnBoard(),
            static_cast<long long>(xp), static_cast<long long>(xn), static_cast<long long>(yp), static_cast<long long>(yn),
            verdict);
        std::fflush(stdout);
    }
    g_hint = nullptr;
    tx.unlockAll();
    Sleep(50);
    std::printf("  блокировки сняты: на плате %04X\n", tx.locksOnBoard());
    return ok;
}

bool runReconnect(makcu::RawTransport& tx, double seconds) {
    tx.setLock(LockTarget::Side2, true);
    const bool tracking = tx.setTracking(true);  // переподключение возвращает и слежение (km.screen)
    std::printf(">>> переподключение: %.0f с движения по кругу, side2 заблокирована.\n"
                "    Выдерните кабель USB2 (COM, средний разъём MAKCU) на 3-5 с и вставьте обратно.\n", seconds);
    g_hint = L"ВЫДЕРНИТЕ USB2 (средний разъём) на 3-5 с и вставьте обратно. Мышь не трогайте";
    Mover mv(tx);
    timeBeginPeriod(1);
    const int64_t period = Q::fromUs(1000.0);
    int64_t due = Q::now() + period;
    const int64_t until = Q::now() + Q::fromUs(seconds * 1e6);
    bool wasConnected = true;
    int64_t lostAt = 0;
    uint64_t lastMoves = g_ui.moves.load();
    int64_t nextReport = Q::now() + Q::fromUs(1e6);
    while (Q::now() < until && !g_stop.load()) {
        spinUntil(due);
        due += period;
        mv.tick();
        const bool c = tx.connected();
        if (c != wasConnected) {
            if (!c) {
                lostAt = Q::now();
                std::printf("  [%s] СВЯЗЬ ПОТЕРЯНА\n", "!");
            } else {
                const auto s = tx.stats();
                std::printf("  [+] связь восстановлена через %.0f мс (обрыв по оценке транспорта %.0f мс), блокировки на плате %04X\n",
                    Q::toUs(Q::now() - lostAt) / 1000.0, s.lastOutageMs, s.locksOnBoard);
            }
            wasConnected = c;
        }
        if (Q::now() >= nextReport) {
            nextReport += Q::fromUs(1e6);
            const uint64_t mvs = g_ui.moves.load();
            std::printf("  HID %4llu Гц | связь %s\n", static_cast<unsigned long long>(mvs - lastMoves), c ? "есть" : "НЕТ");
            lastMoves = mvs;
        }
    }
    timeEndPeriod(1);
    const bool finished = tx.finish();
    Sleep(300);
    const auto s = tx.stats();
    std::printf("  обрывов %llu, переподключений %llu, сброшено записей %llu, блокировки на плате %04X (ждём side2 = 0010), finish %s\n",
        static_cast<unsigned long long>(s.linkLost), static_cast<unsigned long long>(s.reconnects),
        static_cast<unsigned long long>(s.droppedRecords), s.locksOnBoard, finished ? "ok" : "НЕТ");
    const auto ph = tx.physical();
    std::printf("  слежение: включено %s, идёт %s, опросов 0x63 %llu, ответов %llu, у края %llu\n", tracking ? "да" : "НЕТ",
        ph.live ? "да" : "НЕТ", static_cast<unsigned long long>(s.posPolls), static_cast<unsigned long long>(s.posReplies),
        static_cast<unsigned long long>(s.clamped));
    const bool ok = s.linkLost >= 1 && s.reconnects == s.linkLost && s.connected && (s.locksOnBoard & 0x10) && tracking && ph.live;
    tx.unlockAll();
    g_hint = nullptr;
    return ok;
}

// Наглядно: зажать левую, провести круг (рисуется окружность), отпустить; потом по точке
// от правой, средней, side1, side2. Повторять seconds секунд.
bool runDraw(makcu::RawTransport& tx, double seconds) {
    std::printf(">>> рисование %.0f с: в окне должны появляться чёрные окружности и цветные точки\n", seconds);
    Mover mv(tx);
    timeBeginPeriod(1);
    const int64_t period = Q::fromUs(1000.0);
    const int64_t until = Q::now() + Q::fromUs(seconds * 1e6);
    uint32_t circles = 0;
    const uint32_t d0 = g_ui.down[0].load();
    while (Q::now() < until && !g_stop.load()) {
        tx.press(MouseButton::Left);
        Sleep(15);  // нажатие доходит за ~1.5 мс; запас, чтобы круг рисовался с самого начала
        int64_t due = Q::now() + period;
        for (int t = 0; t < 300 && !g_stop.load(); ++t) {  // один оборот круга
            spinUntil(due);
            due += period;
            mv.tick();
        }
        (void)tx.finish(100);
        tx.release(MouseButton::Left);
        ++circles;
        Sleep(250);
        for (MouseButton b : {MouseButton::Right, MouseButton::Middle, MouseButton::Side1, MouseButton::Side2}) {
            if (g_stop.load()) break;
            for (int t = 0; t < 40; ++t) {  // сдвинуться, чтобы точки не легли одна на другую
                spinUntil(Q::now() + period);
                (void)tx.move(6, 0);
            }
            (void)tx.finish(100);
            tx.press(b);
            Sleep(60);
            tx.release(b);
            Sleep(200);
            checkEsc();
        }
        for (int t = 0; t < 160 && !g_stop.load(); ++t) {  // вернуться назад
            spinUntil(Q::now() + period);
            (void)tx.move(-6, 0);
        }
        (void)tx.finish(100);
        Sleep(400);
        checkEsc();
    }
    timeEndPeriod(1);
    const uint32_t got = g_ui.down[0].load() - d0;
    std::printf("  окружностей: %u, нажатий левой дошло до Windows: %u\n", circles, got);
    return got == circles;
}

// Только смотреть: статус в окне (физика, инжект, блокировки, связь), жмите кнопки сами.
bool runWatch(makcu::RawTransport& tx, double seconds) {
    std::printf(">>> %.0f с: жмите кнопки мыши в окне - точки рисуются, статус вверху окна\n", seconds);
    const int64_t until = Q::now() + Q::fromUs(seconds * 1e6);
    while (Q::now() < until && !g_stop.load()) {
        Sleep(10);
        checkEsc();
        tx.service();
    }
    return true;
}

bool runSoak(makcu::RawTransport& tx, int minutes) {
    std::printf(">>> долгий прогон: %d мин. Каждая минута: 35 с движения 1 кГц + клики + блокировки, 20 с простоя,\n"
                "    5 с движения после простоя. Не трогайте мышь. Ctrl+C - досрочно.\n", minutes);
    std::printf("  мин | HID Гц | 1 мс %% | путь sent=raw | клики sent/down/up | после простоя мс | отказ raw | исправл | обрывы | память МБ | хэндлы | физ. кнопок | записи-сообщения\n");
    g_hint = L"НЕ ТРОГАЙТЕ МЫШЬ И КНОПКИ: идёт долгий прогон, кликать не нужно";
    Mover mv(tx);
    timeBeginPeriod(1);
    const int64_t period = Q::fromUs(1000.0);
    bool ok = true;
    double mem0 = 0.0, memLast = 0.0, resumeMax = 0.0;
    DWORD h0 = 0, hLast = 0;
    uint64_t clicksSent = 0, clicksDown0 = g_ui.down[0].load(), clicksUp0 = g_ui.up[0].load();
    auto s0 = tx.stats();
    for (int minute = 1; minute <= minutes && !g_stop.load(); ++minute) {
        const int64_t m0 = Q::now();
        const uint64_t mv0 = g_ui.moves.load(), one0 = g_ui.oneMs.load();
        const uint64_t records0 = tx.stats().submitted;
        int64_t due = Q::now() + period;
        int nextClick = 2000, releaseAt = -1;
        // 0..35 с: движение, клик левой каждые 2 с (удержание 40 мс), остальные кнопки - по разу
        for (int t = 0; t < 35000 && !g_stop.load(); ++t) {
            spinUntil(due);
            due += period;
            mv.tick();
            if (t == nextClick) {
                tx.press(MouseButton::Left);
                ++clicksSent;
                releaseAt = t + 40;
                nextClick += 2000;
            }
            if (t == releaseAt) tx.release(MouseButton::Left);
            if (t == 10000) tx.press(MouseButton::Right);
            if (t == 10040) tx.release(MouseButton::Right);
            if (t == 12000) tx.press(MouseButton::Middle);
            if (t == 12040) tx.release(MouseButton::Middle);
            if (t == 14000) tx.press(MouseButton::Side1);
            if (t == 14040) tx.release(MouseButton::Side1);
            if (t == 16000) tx.press(MouseButton::Side2);
            if (t == 16040) tx.release(MouseButton::Side2);
            if (t == 20000) {
                tx.setLock(LockTarget::X, true);
                tx.setLock(LockTarget::Y, true);
                tx.setLock(LockTarget::Side1, true);
            }
            if (t == 25000) tx.unlockAll();
        }
        const uint64_t mvMove = g_ui.moves.load() - mv0, oneMove = g_ui.oneMs.load() - one0;
        // 35..55 с: простой (движения нет, транспорт продолжает опросы и пульс)
        (void)tx.finish();
        const int64_t idleEnd = m0 + Q::fromUs(55e6);
        while (Q::now() < idleEnd && !g_stop.load()) {
            Sleep(5);
            checkEsc();
        }
        // 55..60 с: возобновление: сколько ждать первого отчёта движения
        g_ui.firstMoveAfter = 0;
        g_ui.mark = Q::now();
        mv.firstAcceptT = 0;
        due = Q::now() + period;
        for (int t = 0; t < 5000 && !g_stop.load(); ++t) {
            spinUntil(due);
            due += period;
            mv.tick();
        }
        const int64_t first = g_ui.firstMoveAfter.load();
        const double resumeMs = first && mv.firstAcceptT ? Q::toUs(first - mv.firstAcceptT) / 1000.0 : -1.0;
        g_ui.mark = 0;
        (void)tx.finish();
        Sleep(60);
        const auto s = tx.stats();
        const double mem = processMemoryMb();
        const DWORD hs = processHandles();
        if (minute == 1) {
            mem0 = mem;
            h0 = hs;
        }
        memLast = mem;
        hLast = hs;
        resumeMax = std::max(resumeMax, resumeMs);
        const int64_t rx = g_ui.rawX.load(), ry = g_ui.rawY.load();
        const bool pathOk = rx == mv.sentX && ry == mv.sentY;
        const uint32_t cd = g_ui.down[0].load() - static_cast<uint32_t>(clicksDown0);
        const uint32_t cu = g_ui.up[0].load() - static_cast<uint32_t>(clicksUp0);
        const bool clicksOk = cd == clicksSent && cu == clicksSent;
        const bool linkOk = s.linkLost == s0.linkLost;
        ok = ok && pathOk && clicksOk && linkOk && resumeMs >= 0.0 && resumeMs < 20.0;
        // Больше 0 - отчёты пропали по дороге (дыра), меньше 0 - лишние отчёты физической мыши.
        const long long recordsGap = static_cast<long long>(s.submitted - records0) - static_cast<long long>(g_ui.moves.load() - mv0);
        std::printf("  %3d | %6.0f | %6.1f | %s (%lld,%lld) | %llu/%u/%u%s | %7.1f | %6llu | %5llu | %4llu | %8.1f | %5lu | %llu | %+lld%s\n", minute,
            mvMove / 35.0, mvMove > 1 ? 100.0 * static_cast<double>(oneMove) / static_cast<double>(mvMove - 1) : 0.0,
            pathOk ? "ДА " : "НЕТ", static_cast<long long>(rx - mv.sentX), static_cast<long long>(ry - mv.sentY),
            static_cast<unsigned long long>(clicksSent), cd, cu, clicksOk ? "" : " <--", resumeMs,
            static_cast<unsigned long long>((s.rejected + s.immediate) - (s0.rejected + s0.immediate)),
            static_cast<unsigned long long>(s.repairs - s0.repairs), static_cast<unsigned long long>(s.linkLost - s0.linkLost),
            mem, static_cast<unsigned long>(hs), static_cast<unsigned long long>(s.buttonEvents - s0.buttonEvents), recordsGap,
            s.buttonEvents != s0.buttonEvents ? " <-- мышью пользовались, прогон не зачётный" : "");
        std::fflush(stdout);
    }
    timeEndPeriod(1);
    std::printf("  память: %.1f -> %.1f МБ, хэндлы: %lu -> %lu, худшая задержка после простоя %.1f мс\n", mem0, memLast,
        static_cast<unsigned long>(h0), static_cast<unsigned long>(hLast), resumeMax);
    if (memLast - mem0 > 5.0 || hLast > h0 + 20) ok = false;
    g_hint = nullptr;
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(onCtrl, TRUE);
    std::string port, mode;
    double arg = 0.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = argv[++i];
        else if (a == "--clicks") mode = a;
        else if ((a == "--locks" || a == "--axes" || a == "--reconnect" || a == "--soak" || a == "--draw" || a == "--watch") && i + 1 < argc) {
            mode = a;
            arg = std::atof(argv[++i]);
        } else {
            mode.clear();
            break;
        }
    }
    if (mode.empty()) {
        std::printf("controls_test.exe [--port COM5] --clicks | --draw S | --watch S | --locks S | --axes S | --reconnect S | --soak MINUTES\n");
        return 2;
    }
    makcu::RawTransport tx;
    std::string err;
    if (!tx.open(port, err)) {
        std::printf("[-] %s\n", err.c_str());
        return 1;
    }
    std::printf("[+] %s, %u бод | %s\n", tx.port().c_str(), tx.baud(), tx.info().c_str());
    g_tx = &tx;
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    std::thread ui(uiThread);
    while (!g_ui.ready.load()) Sleep(1);
    Sleep(300);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    bool ok = false;
    if (mode == "--clicks") ok = runClicks(tx);
    else if (mode == "--locks") ok = runLocks(tx, arg);
    else if (mode == "--axes") ok = runAxes(tx, arg);
    else if (mode == "--reconnect") ok = runReconnect(tx, arg);
    else if (mode == "--soak") ok = runSoak(tx, static_cast<int>(arg));
    else if (mode == "--draw") ok = runDraw(tx, arg);
    else if (mode == "--watch") ok = runWatch(tx, arg);

    std::printf("%s\n", tx.statsLine().c_str());
    g_stop = true;
    ui.join();  // окно больше не читает транспорт
    g_tx = nullptr;
    tx.close();  // отпустить кнопки, снять блокировки
    std::printf(ok ? "[+] OK\n" : "[-] ЕСТЬ ПРОБЛЕМЫ - см. выше\n");
    return ok ? 0 : 1;
}
