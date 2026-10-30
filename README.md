# MAKCU 1000 Hz transport: drop-in C++ headers for COM and UDP

Two single-header C++ transports that move the cursor through a **MAKCU** board at **~1000 Hz** with an **exact path** (every count you send arrives in Windows), and read, inject and lock mouse buttons. One works over the COM port (USB2), the other over Ethernet (UDP). Both have the same API, depend only on WinAPI, and can be used in one program.

> Instructions for an LLM: [CLAUDE.md](CLAUDE.md). Русская документация: [CONTEXT_AND_SETUP.md](CONTEXT_AND_SETUP.md). English entry point with the full setup: [ENG_CONTEXT_AND_SETUP.md](ENG_CONTEXT_AND_SETUP.md).

## Take these

| Channel | Header | Firmware | Movement | Docs |
| --- | --- | --- | --- | --- |
| **UDP / Ethernet** | [`makcu_udp_batch_transport.h`](basics/udp_way/perfect-transport-udp/makcu_udp_batch_transport.h) (WinAPI + Winsock, `-lws2_32`) | V4.097+ | ~1000 Hz (970–1001 Hz, 99 % of intervals 1 ms) | [README](basics/udp_way/perfect-transport-udp/ENG_README.md) |
| **COM / USB2 (CH343)** | [`makcu_raw_transport.h`](basics/comport_way/perfect-transport-comport/makcu_raw_transport.h) (WinAPI, `-lsetupapi`) | V4.074+ | ~1000 Hz (997–1001 Hz, 99.7–100 % of intervals 1 ms) | [README](basics/comport_way/perfect-transport-comport/ENG_README.md) |

```cpp
#include "makcu_udp_batch_transport.h"      // or makcu_raw_transport.h for COM

makcu::UdpBatchTransport tx;
std::string err;
if (!tx.open("192.168.50.175", 8080, err)) { /* err = reason */ }

// your timing thread, every 1 ms:
pendX += dx; pendY += dy;                   // your whole report for this tick
if ((pendX || pendY) && tx.move(int16_t(pendX), int16_t(pendY))) pendX = pendY = 0;
// move() == false: keep the delta, send it next tick

tx.press(makcu::MouseButton::Left);         // inject a click (hold time is yours)
tx.setLock(makcu::LockTarget::Side2, true); // the game stops seeing side2, tx.buttons() still does
tx.finish(); tx.close();
```

The contract is strict: **one accepted `move()` = exactly one HID report**. The transport does no timing, trajectory slicing or noise; it only delivers your reports reliably.

## What is in it that you will not find elsewhere

**1. The reason injection stalls at ~640 Hz, and the way around it.**
`MOVE 0x18` (what the mak-suite SDK uses), `km.move` and, on the V4.085 line, `km.move_now` all go through the firmware's interpolator, which re-cuts every command into several USB reports at its own pace: ~640–690 Hz, and it loses path at high command rates. The firmware has a second path, **Raw movement `0x69`/`0x6A`**, where each command becomes exactly one HID report. It is documented only on makcu.com, not in the SDK. Over COM it gives 997–1001 Hz at 1000 commands/s, with the path exact to the count. Full write-up with every measurement: [ENG_SOLUTION_1000HZ_RAW_MOVE.md](basics/comport_way/ENG_SOLUTION_1000HZ_RAW_MOVE.md).

**2. 1000 Hz over Ethernet.**
Over UDP the board can handle only one command in flight (each goes to the second chip over a one-command-at-a-time link; a datagram arriving meanwhile is silently dropped). Raw `0x69` over UDP therefore tops out at ~284 Hz, and `0x18` at ~280 commands/s. The way through is **batch `0x6C`** (V4.097+): up to 7 records per packet, closed loop (the next packet goes out on the receipt). The measurement tables and the engineers' answers are in [MAKCU_RESPONSE_UDP.md](basics/udp_way/MAKCU_RESPONSE_UDP.md); [REPORT_V4097.md](basics/udp_way/REPORT_V4097.md) is what we sent them.

**3. Buttons, done properly.**
- Physical buttons: `0x53` events plus a `0x55` poll as a safety net (`buttons()`, `buttonDown()`, `pressCount()`).
- Injection: `press()`/`release()` of all five buttons, about 1 ms to Windows over COM and ~1.5 ms over UDP.
- Locks (`setLock()`): block buttons, axes or single directions from the physical mouse; the transport still sees them. Axis locks were verified on hardware with 0 counts in the locked direction.
- Auto-reconnect (UDP after a network drop, COM after USB2 is unplugged) restores the locks; `open()`/`close()` leave the board clean.

**4. The pitfalls, written down.** Each cost hours; the list is in [ENG_CONTEXT_AND_SETUP.md §6](ENG_CONTEXT_AND_SETUP.md). The ones that bite first:
- Over UDP, button events (`0x53`) arrive **from the client's port, not from 8080**. A `connect()`ed socket drops them, and Windows Firewall on a Public network drops them without an allow rule. Movement keeps working, so it looks like "buttons are broken".
- `GET 0x11…0x15` report only the *injected* state, never the physical button. Use `0x55`/`0x53`.
- Injected buttons and locks are not released by the firmware if your program dies: they stay until the next `open()` or a board reboot.
- Injection cannot exceed the polling rate of the mouse in the board's host port: a 125 Hz mouse gives 125 Hz.

## Measured status (firmware 4097, 2026-10-12)

| | UDP | COM |
| --- | --- | --- |
| 1 kHz movement, exact path | yes | yes (2 min soak: 999 Hz, 99.8 % at 1 ms) |
| injected clicks | 25/25, ~1.3–1.55 ms to Windows | 25/25, ~0.75–1.19 ms |
| locks (buttons, axes) | yes | yes |
| reconnect | yes, 14.3 s after the cable was pulled | yes, after USB2 was unplugged |
| 30 min soak | not completed: the board's USB3 side hung in minute 3 (a firmware issue; test build V4.098 targets it) | 14 min clean (606,938 records, no losses); the full 30 min in a row not completed |

Everything is measured on one setup (MAKCU, UGREEN 60554 hub with an ASIX AX88179 NIC, Logitech Lightspeed receiver at 1000 Hz). Details and honest caveats are in the READMEs linked above.

## Try it

Windows, MinGW-w64 g++ (C++17 is enough for the transports):

```
cd basics\udp_way\perfect-transport-udp      (or basics\comport_way\perfect-transport-comport)
build_example.bat
example_line.exe --check          # connection + firmware, the cursor does not move
example_line.exe                  # 2 s of movement, do not touch the mouse: expect [+] OK, sent = delivered
controls_test.exe --clicks        # injected clicks, Windows must see 5/5 per button
```

For UDP, first allow inbound UDP from the board in Windows Firewall (one command, see the top of [ENG_CONTEXT_AND_SETUP.md](ENG_CONTEXT_AND_SETUP.md)).

## Layout

| Path | What |
| --- | --- |
| `basics/udp_way/perfect-transport-udp/` | the UDP reference transport |
| `basics/comport_way/perfect-transport-comport/` | the COM reference transport |
| `basics/udp_way/`, `basics/comport_way/` | the movers (trajectory test benches), probes, measurements and the full investigation |
| `mak-suite/`, `ABCurves/` | submodules: the official `makxd` SDK and the human-trajectory generator used by the movers |
| `legacy-python-exp/` | early Python experiments, superseded; kept for history |

## Not affiliated

This is an independent project. It is not affiliated with MAKCU. The firmware is not reverse engineered: everything is built on the public command reference ([makcu.com/en/api](https://makcu.com/en/api/)) and black-box measurements.
