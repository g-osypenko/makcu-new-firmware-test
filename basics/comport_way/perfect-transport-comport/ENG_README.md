# perfect-transport-comport: the frozen MAKCU transport (COM, Raw 0x69/0x6A)

*English translation of [`README.md`](README.md) (Russian original). Source-code comments and program output are still in Russian.*

The reference working transport, split out into one folder so that it can be dropped into any
program and not confused with substeppers, accumulators and renderers. It **only delivers**
whole `(dx, dy)` reports to MAKCU. The pace, trajectory slicing and quantization stay on the side of
the calling code.

**Do not edit.** If you need a different version, create a new folder. Source: `../makcu_transport.h`
(`RawComTransport`, `Format::RawMove`, `Mode::Async`) as of 2026-10-10. The Raw logic was carried over
unchanged. Only the comparison formats (`0x18`, `km.*`), the sync mode, the SDK transport
and the dependency on the mak-suite SDK were removed.

**Revision 2026-10-12 (2): mouse buttons** (left, right, middle, two side buttons). The Raw movement logic
did not change, the movement self-check after the change is the same: 2000 = 2000 = 2000, rejections 0.

**Revision 2026-10-12 (3): button injection, locks, reconnect** — modelled on the UDP reference
([`../../udp_way/perfect-transport-udp`](../../udp_way/perfect-transport-udp/ENG_README.md)), the same API.
`press()`/`release()` of all five buttons (no timing: the hold time is set by the caller), locks of
physical input (`setLock()`, 14 targets), verification with the board snapshot `0x56` and a retry of lost commands,
auto-reconnect after USB2 is unplugged, state reset on `open()` and `close()`. The Raw movement
logic did not change (after the change: 2000 = 2000 = 2000, rejections 0). The write ring is now under a
mutex: a button can be pressed from any thread, and it goes out at once, without waiting for `move()`.

## Files

| File | What it is |
| --- | --- |
| `makcu_raw_transport.h` | **the whole transport**: one header, WinAPI only (`setupapi`) |
| `example_line.cpp` | an embedding example and a self-check on hardware (a model of the 1 ms loop) |
| `controls_test.cpp` | a check of buttons, button and axis locks, reconnect and a long run (a test window, the cursor is confined to it) |
| `build_example.bat` | build of both: MinGW-w64 g++, C++17+ |
| `../probe_controls_com.py`, `../probe_controls_gap_com.py` | probes: the `0x56` format over COM, whether a pause is needed after `SET`, whether `SET`s get lost |

To use it: copy `makcu_raw_transport.h`, link `-lsetupapi` (in MSVC that is `setupapi.lib`).

## Usage

```cpp
#include "makcu_raw_transport.h"

makcu::RawTransport tx;
std::string err;
if (!tx.open("", err)) { /* "" - find the MAKCU itself; or "COM5" */ }

// timing thread, every tick (usually 1 ms):
pendX += dx; pendY += dy;                      // your whole report for the tick
if ((pendX || pendY) && tx.move(int16_t(pendX), int16_t(pendY))) pendX = pendY = 0;
// move() == false - the delta stays in pend and goes out on the next tick

tx.finish();   // end of movement: wait for the receipts, send the carry-over (up to 300 ms)
tx.close();
```

Buttons (from any thread, non-blocking):

```cpp
if (tx.buttonDown(makcu::MouseButton::Side2)) { /* side 2 is held right now */ }
uint8_t mask = tx.buttons();                    // bit0 left .. bit4 side2
uint32_t n = tx.pressCount(makcu::MouseButton::Left);  // presses since open(): the difference = clicks between reads
```

Button injection and locks (from any thread, non-blocking):

```cpp
tx.press(makcu::MouseButton::Left);             // press; the hold is on your side (>= 3-5 ms)
tx.release(makcu::MouseButton::Left);           // release
tx.setLock(makcu::LockTarget::Side2, true);     // side2 no longer reaches the game, but buttons() sees it
tx.setLock(makcu::LockTarget::X, true);         // the physical mouse's X axis (both directions)
tx.unlockAll();                                 // clear all locks
if (!tx.connected()) { /* USB2 is gone, the transport reconnects by itself */ }
```

## Contract

| | |
| --- | --- |
| `open(port, err)` | `""` = the first CH343 `1A86:55D3`. Probes 4,000,000 / 1,000,000 / 115,200 baud by the `DEVICE 0x02` reply, DTR/RTS off, then Raw: metadata `0x6A(id=0)`, cleanup of records left by past sessions, the window. `false` + a reason: port busy, the board did not answer, firmware without the Raw API |
| `move(dx, dy)` | **one accepted call = exactly one HID report**. Never blocks on USB (a short wait for the mutex is possible while another thread sends a button). `false` = the window (≤ 12 live records) or the driver ring (32) is busy, or there is no link: the caller keeps the delta |
| `move(0, 0)` / `service()` | nothing goes to the mouse, only receipts and carry-over |
| `finish(ms = 300)` | acknowledge the live records and send the carry-over. `true` = everything closed, carry-over `(0,0)` |
| `stats()` / `statsLine()` | counters, can be called from any thread |
| `buttons()` / `buttonDown(b)` | mask of physical buttons (bit0 left, 1 right, 2 middle, 3 side1, 4 side2). Any thread, non-blocking. Updated by the `0x53` event right on change |
| `pressCount(b)` | presses (0 → 1 transitions) since `open()`. A click is not lost even if `buttons()` is read rarely. A button already held at `open()` does not count as a press |
| `pollButtons()` | re-read the `0x55` mask (any thread). Usually not needed: after a stream overflow the transport re-reads by itself (in `move()`/`service()` or by the watchdog within ≤ 2 ms) |
| `press(b)` / `release(b)` / `setButtonState(b, down)` / `releaseAllButtons()` | button injection (`SET 0x11…0x15`). Goes out **at once from the calling thread** in a single write together with a verifying snapshot, movement does not wait. Measured: from `press()` to the event in Windows 0.75–1.19 ms on average, max 1.34 ms. Injection is added to the physical state (OR) |
| `injectedButtons()` / `injectedButtonsOnBoard()` | the requested mask of injected buttons / what is on the board per the last snapshot |
| `setLock(t, on)` / `setLocks({t…}, on)` / `unlockAll()` / `locked(t)` / `locks()` / `locksOnBoard()` | physical input lock (`SET LOCK 0x60`): 5 buttons, the X/Y axes and the wheel as a whole or per direction. What is locked does not reach the PC from the physical mouse; injection passes; `buttons()` sees the physical state even for a locked button. `setLocks` (2026-10-30, for the same API as UDP) sets several targets in one call — one write to the port. The UDP reference already sends movement directions as one `0x16` (physical movement stops after ~1.3 ms instead of 7.7). Not done here yet: the USB2 cable is needed to test it |
| `controlsVerified()` | `false` — the board did not answer the `0x56` snapshot: commands go out unverified |
| `connected()` / `healthy()` | there is a link to the board. `false` — a reconnect is in progress, `move()` returns `false` |
| `firmware()` | the firmware version (`FIRMWARE 0x04`), `0` — no answer |
| `open()` | additionally: clears injected buttons and locks left over from a crashed program |
| `close()` | releases all injected buttons, clears all locks (confirmed with a snapshot), returns the `INPUT_STREAM mouse` stream to its state before `open()` |
| threads | `move`/`service`/`finish` are called **from one thread**. The rest — from any. Inside there are a read thread and a watchdog thread (2 ms tick) |
| carry-over | the transport returns clamping > 32767 and board rejections to the carry-over by itself: the path is not lost |

What the transport does **not** do: it keeps no clock and pace, does not slice the trajectory, does not accumulate fractional
deltas and does not add noise. The HID rate equals the rate of accepted non-zero `move()` calls and is no higher than the USB polling
rate of the cloned mouse.

## Mouse buttons

`open()` enables the event stream `INPUT_STREAM mouse` (`0x52 01 01`), then reads the starting mask
`GET PHYS_BUTTONS 0x55` (V4.036+). After that the read thread parses the events
`DE AD 03 00 53 01 <id> <0|1>` and keeps the mask. The event `53 01 FF FF` means events were lost:
the transport re-reads the mask via `0x55` in the nearest `move()`/`service()`. If the program does not
call either for a long time, after an overflow the mask may lag until the next event.

### Pitfalls on which an integration breaks

- **The binary `0x11…0x15` (LEFT…SIDE2) do not see the physical button.** They return only the
  injected state ("software holds the button"), so the physical press there is always 0. Physical state
  comes from `0x55 PHYS_BUTTONS`, the `0x53` events and the text `km.left()…km.side2()` (0 none / 1 phys. / 2 inj. / 3 both).
- **The old `0x10 BUTTONS` stream is not needed.** In modes 1/2 it inserts the text `km.<mask>\r\n` into COM between
  binary frames (the mak-suite SDK enables it on connect). The transport does not touch or parse it.
- **Every enabling of the `0x52` stream starts from "everything released".** A held button will arrive as an event only
  with the next mouse report, so the transport reads the starting mask via `0x55` after enabling.
- **Over UDP the `0x53` events do arrive, but from a "foreign" port.** The board sends them to the client's port, and sets the source port
  equal to the client's port, not 8080. A socket with `connect()` silently drops such datagrams (which is why the
  first check showed "0 events"), and Windows Firewall on a Public network drops them for a program without an
  allow rule. The UDP transport does not connect the socket and insures itself with `0x55` polling.

## Button injection and locks

The board's commands `SET 0x11…0x15` (buttons) and `SET 0x60` (locks) are **without a reply** — over COM too
(probe: not a single frame within 50 ms). How COM differs from UDP (measurements of 2026-10-12, fw 4097):

- **no pause is needed after a command.** `SET` → pause 0…2 ms → `GET 0x56`: 300/300 at any pause;
  `SET` → `SET` with no pause: 300/300 (over UDP the next command sooner than ~1 ms is silently lost);
- **but a `SET` is occasionally lost**: 0.3–0.7 % in some runs, up to 12 % in a burst in another; re-reading
  after 30 ms confirms the command was not applied, rather than applied late. The cause is not established;
- **the `GET 0x56 INJECT_SNAPSHOT` snapshot** over COM is the same 40 bytes (`[0]` injected buttons, `[1..2]` lock
  mask), with a reply in ~0.3 ms (max 0.62).

So the transport sends all "requested ↔ sent" differences and `GET 0x56` **in a single
WriteFile** right from the thread that called `press()`/`setLock()`. Replies over COM arrive in the order of
the requests, so the snapshot is matched to its write without a nonce. If the board does not have what was sent,
the difference is sent again (`stats().repairs`). The pulse snapshot is once per 250 ms.

The transport stores the **requested** state, not a command queue. `press()` and an immediate `release()` both go
out, but if they land between two USB polls (1 ms), Windows will not see the click: **hold a button
for at least 3–5 ms** (the tests hold 40 ms).

**Axes in locks.** The board has two bits per axis, "+" and "−". The whole-axis bit (`X`, `Y`, `Wheel`)
is set by the board itself when both directions are locked. So `setLock(X, on)` sets or
clears both directions, and `setLock(XPos, false)` on a locked axis leaves only −X.

## Reconnect

A drop over COM is an unplugged USB2 or a vanished port (a `WriteFile`/`ReadFile` error), or silence:
not a single byte from the board for 750 ms with a 250 ms pulse. Then:

- `connected()` and `healthy()` give `false`, `move()` returns `false`. Live movement records and the carry-over
  are dropped (`stats().droppedRecords`): by the time of recovery they are stale;
- the watchdog thread closes the handle and every 250 ms tries to open the port again (the same one; with `open("")`
  the first MAKCU found), selects the baud rate, `prepareRaw()` (ids continue, the tail of records
  is cleaned up), enables the event stream, re-reads the buttons and from the snapshot re-sends the requested locks
  and buttons.

Checked by the owner on 2026-10-12 (`controls_test --reconnect 60`, USB2 unplugged): the link
is restored by itself, the lock is in place. The recovery time was not recorded.

**Limitation.** If there is no link at the moment of `close()`, or the program crashed before reaching `close()`, the buttons and locks
held by the program **stay on the board**: the firmware does not release them itself. They are cleared by the
next `open()` of this transport or by a board reboot. Install a Ctrl+C and console-close handler
that calls `close()`.

## Requirements

- Firmware with the Raw API: V4.074 and newer (verified on **4094** and **4097**). On an older one `open()` returns an error.
  Injection and locks with snapshot verification — firmware with `0x56`/`0x60` (verified on 4097).
- The mouse in MAKCU's host port is polled at 1000 Hz: the board copies its polling interval.
- The COM port is exclusive: only one program can hold it at a time.
- The transport itself does not touch "Enhance pointer precision". For an exact path in Windows the caller turns it off.

## Confirmed by measurements (fw 4094, COM5, 2026-10-10)

| Run | Result |
| --- | --- |
| 1000 commands/s, 30 s (F2) | ~1000 Hz HID, 99.7–100 % of intervals at 1 ms |
| speed ladder 24 s (F10, accumulator) | sent 15673 = in the FIFO 15673 = in USB 15673, rejections 0, path (300,0) = (300,0), call avg 2.1 / max 88 µs, window max 4 |
| flicks 30 s (F4, accumulator) | 9978 = 9978 = 9978, rejections 0, path (187,34) = (187,34) |
| buttons, `example_line.exe --buttons 12` (fw 4097, 2026-10-12) | 109 `0x53` events, all 5 buttons, presses left 11 / right 9 / middle 9 / side1 12 / side2 13, overflows 0. Raw events matched the `0x55` poll |
| revision 3, `example_line.exe` (fw 4097, 2026-10-12) | 2000 = 2000 = 2000, rejections 0, carry-over (0,0), call avg 2.2 / max 27 µs, pulse snapshots 8, lost 0 |
| revision 3, `controls_test --clicks` | 25/25 presses and releases in Windows; `press()` → Windows avg 0.75–1.19 ms per button, max 1.34 ms |
| revision 3, `controls_test --soak 2` | HID 999 Hz, 99.8 % of intervals at 1 ms, exact path (0,0), clicks 34/34/34, after 20 s idle the first report in 0.4–0.6 ms, raw rejections 0, repairs 0, snapshots 572 (lost 0), memory 2.1 MB and handles 286 stable |
| revision 3, `controls_test --locks 15` and `--reconnect 60` (run by the owner) | locks work; after USB2 is unplugged the link returns by itself. Numbers not recorded |
| revision 3, `controls_test --axes 10` (the owner moved the mouse) | axis locks work: X, Y, only +X, only −Y — **0 counts** in the locked direction, the free ones — thousands; with no locks everything passes. Board: X = `00E0` (X, +X, −X), Y = `0700` |
| revision 3, `controls_test --soak 30` | minutes 1–14 clean: HID 999 Hz, 99.8–99.9 % at 1 ms, exact path, clicks 238/238/238, after 20 s idle the first report in 0.4–1.2 ms, rejections/repairs/drops 0, memory 2.0–2.1 MB. In minute 15 the physical mouse moved (the path diverged by the external movement), in minute 16 the run was interrupted externally. Transport over 16 min: 606,938 = 606,938 = 606,938, 4,324 snapshots with no losses. The full 30 min in a row were not completed |

The numbers were obtained on `../makcu_transport.h`. The Raw logic in this copy is the same. A check of the copy itself on
hardware: `example_line.exe` (below).

## Self-check

```
build_example.bat
example_line.exe --check     connection + Raw API, the cursor does not move
example_line.exe             2 s: 1 s right, 1 s left at 2 counts/ms (total 0), do not touch the mouse
example_line.exe --buttons 15   show the buttons for 15 s (press all five), the cursor does not move
controls_test.exe --clicks       each button 5 times programmatically; Windows must see 5/5 presses and releases
controls_test.exe --locks 15     side1, side2, middle are locked: press them - the transport sees, Windows does not
controls_test.exe --axes 10      axis locks: X, Y, +X, -Y, nothing for 10 s each - move the mouse in all directions
controls_test.exe --reconnect 60 movement + a lock; unplug USB2 for 3-5 s - the link returns by itself
controls_test.exe --soak 30      30 minutes: 1 kHz movement, clicks, locks, idles; a per-minute log
```

`[+] OK` is expected: `sent = handed to the USB endpoint`, rejections 0, carry-over `(0,0)`.
`controls_test` opens its own window and confines the cursor in it: all clicks land in it. Esc from
any window or the window's X is a correct exit (buttons are released, locks are cleared).

## Protocol on the wire (for reference)

A MAK_API frame: `DE AD | LEN u16 | CMD u8 | PAYLOAD`.

- `0x69 RAW_MOVE`: `id u32, x i16, y i16, timeout_uf u16 (0 = 1 s)` → `state u8, reason u8, id u32`.
  `1` = in the FIFO; `255` = an immediate error (`reason 3` = no space).
- `0x6A RAW_ACK`: `id u32, consume u8` → the same reply. `2` = handed to the USB endpoint, `3` = rejected,
  `255/6` = still queued. `id = 0`: metadata `schema, last_accepted_id, free_records, capacity (16)`.
- IDs strictly increase within one board boot. A receipt no earlier than 2 ms after sending,
  a retry no more than once per 1 ms. If there is no reply to `0x69` for 50 ms, the record is queried via `0x6A`.
- `0x52 INPUT_STREAM`: `GET kind:u8` → `enabled:u8`; `SET kind:u8 enabled:u8` (no reply). kind 1 = mouse.
- `0x53 INPUT_CHANGE` (unsolicited): `kind:u8 id:u8 value:u8`, for the mouse id = button number 0…4, value 0/1.
  `kind FF FF` = overflow.
- `0x55 PHYS_BUTTONS` (V4.036+): an empty request → `mask:u8` of physical buttons.
- `0x11…0x15 LEFT…SIDE2`: `SET state:u8` (no reply) — injection; `GET` → only the injected state.
- `0x60 LOCK`: `SET target:u8 state:u8` (no reply), targets 0…13 (buttons, X/+X/−X, Y/+Y/−Y, wheel/+/−).
- `0x56 INJECT_SNAPSHOT`: an empty request → 40 bytes, `[0]` injected buttons, `[1..2]` lock mask u16.
- `0x04 FIRMWARE`: an empty request → `u32` (4097).
