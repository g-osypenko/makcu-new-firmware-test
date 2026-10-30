# perfect-transport-udp: the frozen MAKCU transport (UDP/Ethernet, batch 0x6C)

*English translation of [`README.md`](README.md) (Russian original). Source-code comments and program output (for example the "СОБЫТИЯ НЕ ДОХОДЯТ" status) are still in Russian.*

> ## ⚠ Required Windows setup for UDP: the firewall
>
> The board sends button events `0x53` **from a port equal to the program's own port, not from 8080**. If
> the network the board is on is marked **Public** (our `Ethernet` interface is Public), Windows Firewall
> **silently drops** these packets for any program that has no allow rule. Movement still works, but
> buttons fall back to the 5 ms backup poll (`statsLine()` prints "СОБЫТИЯ НЕ ДОХОДЯТ", i.e. "EVENTS ARE NOT ARRIVING").
>
> **Once, in PowerShell as administrator**, pick one:
>
> ```powershell
> # allow inbound UDP only from the board, for any program (recommended; done on our machine 2026-10-12)
> New-NetFirewallRule -DisplayName "MAKCU UDP events" -Direction Inbound -Protocol UDP -RemoteAddress 192.168.50.175 -Action Allow
> ```
>
> or mark the network as private (on our machine the Private firewall is off):
>
> ```powershell
> Set-NetConnectionProfile -InterfaceAlias "Ethernet" -NetworkCategory Private
> ```
>
> **If the board's IP changes** (DHCP), the rule with `-RemoteAddress` has to be updated:
> `Set-NetFirewallRule -DisplayName "MAKCU UDP events" -RemoteAddress <new IP>`.
> To check the rule exists: `Get-NetFirewallRule -DisplayName "MAKCU UDP events"`.

The reference working transport for the network, split out into one folder, modelled on
[`../../comport_way/perfect-transport-comport`](../../comport_way/perfect-transport-comport/ENG_README.md). It **only
delivers** whole `(dx, dy)` reports to MAKCU over UDP. The pace, trajectory slicing and quantization stay on
the side of the calling code. This is the **only path to ~1000 Hz over Ethernet**: Raw `0x69` over UDP is stuck
at ~284 Hz, `0x18` at ~280 commands/s.

**Do not edit.** If you need a different version, create a new folder. Source: `../makcu_udp_transport.h`
(`RawUdpTransport`, `Format::Batch`) as of 2026-10-12. The `0x69`/`0x18`/`0x67` formats, the token bucket,
the intake probes and the HOST wrapper were removed.

**Revision 2026-10-12 (2): mouse buttons** (left, right, middle, two side buttons): `0x53` events plus
a safety `0x55` poll. The movement logic did not change. The environment of the loop changed: the socket has no `connect()`
(events arrive from another port of the board), the receive thread wakes on a Windows high-resolution timer,
not on `select()`, so polling runs on schedule and without `timeBeginPeriod(1)` on the caller's side.

**Revision 2026-10-12 (3): button injection, locks, reconnect.** `press()`/`release()` of all five
buttons (no timing: the hold time is set by the caller), locks of physical input (`setLock()`,
14 targets), verification with the board snapshot `0x56` and a retry of lost commands, auto-reconnect with
recovery, state reset on `open()` and `close()`. The movement logic did not change (the movement self-check
after the change: 2000 of 2000, 99.7–99.8 % of intervals at 1 ms).

**Revision 2026-10-30 (4): fast axis locks.** Movement directions go out as one `SET MOVE_MASK 0x16`
command (the wheel as one `0x17`) instead of a series of `0x60`. Added `setLocks({X, Y}, on)`: several
targets in one call. The pause after a command without a reply is 3 ms instead of 2. Measured with a hand
on the mouse: physical movement stops reaching Windows after **1.3 ms median, max 2.3**
(`setLocks({X, Y})`). It used to be 7.7 ms, max 12.3: four `0x60` 2 ms apart, and with board polling
before the lock every 4th command was lost until the snapshot repaired it. The movement logic did not
change.

**Difference from the mover:** if the queue is full (7 records), `move()` returns `false` and the delta stays with
the caller. In the mover the records are merged in this case. This keeps the contract "one accepted `move()`
= exactly one HID report". With a real RTT of ~1.9 ms the queue has no more than 3–4 records, so
this limit does not trigger in practice.

## Files

| File | What it is |
| --- | --- |
| `makcu_udp_batch_transport.h` | **the whole transport**: one header, WinAPI only (Winsock 2) |
| `example_line.cpp` | an embedding example and a self-check on hardware (a model of the 1 ms loop + a HID counter) |
| `controls_test.cpp` | a check of buttons, button and axis locks, reconnect and a long run (a test window, the cursor is confined to it) |
| `build_example.bat` | build of both: MinGW-w64 g++, C++17+ |

To use it: copy `makcu_udp_batch_transport.h`, link `-lws2_32` (in MSVC that is `ws2_32.lib`).

## Usage

```cpp
#include "makcu_udp_batch_transport.h"

makcu::UdpBatchTransport tx;
std::string err;
if (!tx.open("192.168.50.175", 8080, err)) { /* err - the reason */ }

// timing thread, every tick (usually 1 ms):
pendX += dx; pendY += dy;                      // your whole report for the tick
if ((pendX || pendY) && tx.move(int16_t(pendX), int16_t(pendY))) pendX = pendY = 0;
// move() == false - the delta stays in pend and goes out on the next tick

tx.finish();   // end of movement: flush the queue, wait for the receipt, free the records (up to 300 ms)
tx.close();
```

Buttons (from any thread, non-blocking):

```cpp
if (tx.buttonDown(makcu::MouseButton::Side2)) { /* side 2 is held right now */ }
uint8_t mask = tx.buttons();                    // bit0 left .. bit4 side2
uint32_t n = tx.pressCount(makcu::MouseButton::Left);  // presses since open(): the difference = clicks between reads
tx.setButtonPollMs(50);                         // the safety poll (50 ms by default, 0 - off)
```

Button injection and locks (from any thread, non-blocking):

```cpp
tx.press(makcu::MouseButton::Left);             // press; the hold is on your side
tx.release(makcu::MouseButton::Left);           // release
tx.setLock(makcu::LockTarget::Side2, true);     // side2 no longer reaches the game, but buttons() sees it
tx.setLock(makcu::LockTarget::X, true);         // the physical mouse's X axis (both directions)
tx.setLocks({makcu::LockTarget::X, makcu::LockTarget::Y}, true);  // both axes in ONE command: ~2 ms
tx.unlockAll();                                 // clear all locks
if (!tx.connected()) { /* the link is lost, the transport reconnects by itself */ }
```

## Contract

| | |
| --- | --- |
| `open(host, port, err)` | UDP `host:port` (8080), RAW wrapper. `DEVICE 0x02` (mouse route), `FIRMWARE 0x04` ≥ 4097, Raw metadata `0x6A(id=0)`, cleanup of records left by past sessions. `false` + a reason: the board is not on the network, no mouse, firmware without `0x6C` |
| `move(dx, dy)` | **one accepted call = exactly one record = exactly one HID report**. Never blocks on the network. `false` = the queue of 7 records is full or the transport is faulty: the caller keeps the delta |
| `move(0, 0)` / `service()` | nothing goes to the mouse. Send a packet if the previous one has already been acknowledged (usually the receive thread does this) |
| `finish(ms = 300)` | flush the queue, wait for the receipt, free the tail of records on the board. `true` = everything handed over, the board is clean |
| `stats()` / `statsLine()` | counters, can be called from any thread |
| `buttons()` / `buttonDown(b)` | mask of physical buttons (bit0 left, 1 right, 2 middle, 3 side1, 4 side2). Any thread, non-blocking. Updated by the `0x53` event (~1 ms), insured by the `0x55` poll |
| `pressCount(b)` | presses (0 → 1 transitions) since `open()`. A click is not lost even if `buttons()` is read rarely. A button already held at `open()` does not count as a press. If the firewall drops the events, a click shorter than 5 ms may slip past the poll |
| `setButtonPollMs(ms)` | the interval of the safety `0x55` poll, 50 ms by default; `0` turns the poll off entirely. Without events the poll itself runs no less often than every 5 ms |
| `press(b)` / `release(b)` / `setButtonState(b, down)` / `releaseAllButtons()` | button injection (`SET 0x11…0x15`). Goes out as the next command of the loop, **ahead of movement** from the queue: it waits only for the command already in flight. Measured: from `press()` to the event in Windows 1.3–1.55 ms on average, max 1.87 ms. Injection is added to the physical state (OR): releasing a physical button does not cancel a software press |
| `injectedButtons()` / `injectedButtonsOnBoard()` | the requested mask of injected buttons / what is on the board per the last snapshot (up to 250 ms ago) |
| `setLock(t, on)` / `setLocks({t…}, on)` / `unlockAll()` / `locked(t)` / `locks()` / `locksOnBoard()` | physical input lock: 5 buttons, the X/Y axes and the wheel as a whole or per direction. Movement directions go as one `SET 0x16`, the wheel as one `0x17`, buttons as `SET 0x60`. What is locked does not reach the PC from the physical mouse; injection passes; `buttons()` sees the physical state even for a locked button. **Lock both axes with `setLocks({X, Y}, on)`**: one command, physical movement stops after ~1.3 ms (max 2.3). Two `setLock` calls in a row are two commands 3 ms apart (4.3 ms, max 5) |
| `connected()` | there is a link to the board. `false` — a reconnect is in progress, `move()` returns `false` |
| `open()` | additionally: enables the event stream, brings the board to a clean state — clears injected buttons and locks left over from a crashed program |
| `close()` | releases all injected buttons, clears all locks (confirmed with a snapshot), returns the `INPUT_STREAM mouse` stream to its state before `open()` |
| threads | `move`/`service`/`finish` are called **from one thread**. Inside there is its own receive thread: it also sends the next packet right on the receipt |
| losses | records not accepted by the board (no space) are returned to the queue by the transport itself. No receipt for 30 ms: poll `last_accepted_id`, what was accepted is counted, the rest goes back to the queue. The path is neither lost nor duplicated |

What the transport does **not** do: it keeps no clock and pace, does not slice the trajectory, does not accumulate fractional deltas and does not
add noise. The HID rate equals the rate of accepted non-zero `move()` calls and is no higher than the USB polling rate of the cloned mouse.

## How it works

Each UDP command goes to MAKCU's second chip over an internal channel that carries one command at a time.
A datagram that arrives during a transfer is **silently dropped** (confirmed by the MAKCU engineers). Therefore:

- **one packet** in flight; the next goes out from the receive thread right on the receipt (closed loop);
- **up to 7 records** per packet, one per `move()` tick. A receipt reports no more than 7 terminal rows,
  so 16 records give the same speed: the extras come back with `reason 3` ("no space");
- the `X` field of the next packet = the highest terminal id from the receipts (flag `bit0`): the board frees
  records without separate `0x6A` receipts.

With an RTT of ~1.9 ms about 2 records accumulate per round trip, so a packet carries 1.9 records on average, and
the delay from `move()` to sending is no more than one round trip.

## Mouse buttons

`open()` enables the event stream `INPUT_STREAM mouse` (`0x52 01 01`, no reply), then reads the starting
mask `GET PHYS_BUTTONS 0x55`. After that the mask is held by two sources:

- **`0x53` events**: `55 <nonce> DE AD 03 00 53 01 <id> <0|1>` arrive right on the press. Inside is the
  nonce of one of our past commands, so the transport parses them by opcode **before** matching
  replies by nonce, otherwise the event could be taken for a packet receipt;
- **the safety `0x55` poll** once per 50 ms in the same closed loop as the movement. It corrects the mask
  if an event was lost (UDP does not retransmit packets). If events do not arrive at all and the poll has already twice
  found changes, the transport itself speeds the poll up to 5 ms. `statsLine()` then shows
  "СОБЫТИЯ НЕ ДОХОДЯТ (брандмауэр?)" ("events are not arriving (firewall?)").

`close()` returns the `0x52` stream to the state it was in before `open()`.

**Why the socket has no `connect()`.** The board sends replies to commands from port 8080, and events from a port whose number
equals the client's port (verified: `from 192.168.50.175:54444` to our `:54444`). A connected socket
accepts only `board:8080` and drops the events. The transport accepts any datagram from the board's IP.

**Windows Firewall.** If the network the board is on is marked **Public** and the Public firewall is on, it drops the events
"from a foreign port" for a program without an allow rule. Replies to commands pass, so
movement and polling work, but events do not. Then the 5 ms safety poll works (delay ≤ ~6 ms, a click
shorter than 5 ms can be missed). For events to arrive, one of two things is needed (administrator rights):

```powershell
# a rule only for packets from the board, for any program
New-NetFirewallRule -DisplayName "MAKCU UDP events" -Direction Inbound -Protocol UDP -RemoteAddress 192.168.50.175 -Action Allow
# or mark the network with the board as private (if the Private firewall is off or has a rule)
Set-NetConnectionProfile -InterfaceAlias "Ethernet" -NetworkCategory Private
```

The price for movement: a poll is the same kind of command in flight as a `0x6C` packet, only the reply is faster (RTT ~1 ms).
With a 5 ms poll the packets get denser (~3 records instead of ~1.9), and 1000 Hz is kept: 2000 of 2000,
99.3–99.9 % of intervals at 1 ms.

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

The board's commands `SET 0x11…0x15` (buttons), `SET 0x16`/`0x17` (movement and wheel masks) and
`SET 0x60` (locks) are **without a reply**. This means:

- **a 3 ms pause after each such command.** Measured 2026-10-12: a next command sooner than ~1 ms later
  is silently lost (a transfer to the second chip is in progress), with 1.5 ms 40 of 40 pass — but with the
  mouse still. Measured 2026-10-30: when board polling came right before the series and a hand was moving
  the mouse, with a 2 ms pause one of four `0x60` was lost in 33 of 46 attempts. With 3 and 4 ms pauses
  0 of 46 were lost, a single `0x16` 0 of 45. Without polling before the series nothing is lost even at
  2 ms (620 of 620);
- **verification with the `GET 0x56 INJECT_SNAPSHOT` snapshot** (40 bytes: `[0]` injected buttons, `[1..2]` lock
  mask, bit = target number) right after the commands and once per 250 ms. If the board does not have what was
  sent (a datagram was lost, the board was rebooted, another program interfered), the transport
  sends the difference again (`stats().repairs`).

The transport stores the **requested** state (button and lock masks), not a command queue: several
quick `press`/`release` calls in a row collapse to the final state. If you need a guaranteed
press shorter than a loop round trip, hold the button for at least a few milliseconds.

**Axes in locks.** The board has two bits per axis, "+" and "−". The whole-axis bit (`X`, `Y`, `Wheel`)
is set by the board itself when both directions are locked. Measured: "+X, then −X" → `{X,+X,−X}`;
"X, then clear +X" → `{−X}`. So `setLock(X, on)` sets or clears both directions, and
`setLock(XPos, false)` on a locked axis leaves only −X.

**Locking movement with one command.** `SET MOVE_MASK 0x16 left right down up` is the same state as the
`0x60` targets ("a lock is the same state as the matching mask"). Checked with the snapshot: left → −X
(bit 7), right → +X (6), down → +Y (9), up → −Y (10), all four → `07E0` including the whole-axis bits.
`0x16 0000` also clears what was set through `0x60`. Wheel: `0x17 down up` → bits 13, 12. The transport
sends all four directions as one `0x16` as soon as any of them changes.

Lock delay, `probes/track_test.exe --reflock 20` (2026-10-30, a hand moving in circles; from the call to
the last physical count in Windows Raw Input):

| How | Median | Max | Leak (counts) |
| --- | --- | --- | --- |
| revision 3: `setLock(X)` + `setLock(Y)` = four `0x60` 2 ms apart | 7.65 ms | 12.3 ms | 3–23 |
| revision 4: `setLock(X)` + `setLock(Y)` = two `0x16` 3 ms apart | 4.3 ms | 5.0 ms | 1–14 |
| revision 4: `setLocks({X, Y})` = one `0x16` | **1.3 ms** | **2.3 ms** | 0–11 |

It cannot go below ~1–2 ms: the command travels to the second chip, plus the next mouse report. Windows
did receive the leaked counts, and the board position `0x63` sees them too. The board position does not
count movement under a lock: 0 counts over 400 ms with a hand on the mouse.

## Reconnect

Polls and snapshots work as a pulse: if not a single datagram arrives from the board for 750 ms, the link is considered
lost. Then:

- `connected()` and `healthy()` give `false`, `move()` returns `false`. Movement records that did not reach the
  board are dropped (`stats().droppedRecords`): by the time of recovery they are stale. The example and the test
  also drop the accumulated delta in this time, so there is no jerk;
- the watchdog thread retries the handshake every 250 ms. After success it continues the record ids,
  enables the event stream, re-reads the buttons and from the snapshot re-sends the requested locks and buttons
  (after a board reboot they are gone);
- measured: the hub cable unplugged for a few seconds → the drop was noticed, after 14.3 s (the network card link +
  the board's DHCP) the link was restored by itself, the lock was in place, movement continued.

**Limitation.** If there is no link at the moment of `close()`, or the program crashed before reaching `close()`, the buttons and locks
held by the program **stay on the board**: the firmware does not release them itself. They are cleared by the
next `open()` of this transport (it brings the board to a clean state) or by a board reboot.
Install a Ctrl+C and console-close handler that calls `close()`.

## Requirements

- Firmware with batch `0x6C`: **V4.097** and newer. Since 2026-10-11 V4.097 is MAKCU's official firmware;
  V4.095 and older do not support `0x6C`, and `open()` returns an error on them.
- MAKCU network: USB-Ethernet in the board's USB3 port. Verified on the **UGREEN 60554** hub (Genesys Logic
  `05E3:0610` + a gigabit NIC **ASIX AX88179 `0B95:1790`**), the mouse in the same hub, PC → switch → the hub's RJ45.
  External USB-C power of the hub does not affect the result.
- The mouse in MAKCU's host port is polled at 1000 Hz: the board copies its polling interval.
- The transport itself does not touch "Enhance pointer precision". For an exact path in Windows the caller
  turns it off.

## Confirmed by measurements (fw 4097, 2026-10-12)

| Run | Result |
| --- | --- |
| `example_line.exe`, 3 runs of 2 s (this copy) | 2000 = 2000 accepted by the board each time, HID ~980–995 Hz, **99.6–100 % of intervals at 1 ms**, RTT avg 1.87–1.93 / max 2.68 ms, queue max 3, receipt losses 0, board 16/16 |
| mover `--tx batch`, circle and line, 4 s each | HID 983–1001 Hz, 98.5–99.7 % at 1 ms, 4000 = 4000, path sent = path in Windows |
| the bench `udp_raw_batch.exe`, `0x6C` N=7 @139 packets/s | 974 records/s, HID 970 Hz, 97 % at 1 ms, delivered ÷ accepted = 1.000 |
| revision 3, `controls_test`: clicks / locks / cable drop | clicks 25/25, press()→Windows 1.3–1.55 ms (max 1.87); locked middle/side1/side2: the transport saw 13/7/11 presses, Windows 0; cable unplugged → the link returned by itself after 14.3 s, the lock was in place |
| revision 3, `controls_test --axes 10` (2026-10-12, the owner moved the mouse) | axis locks work: X, Y, only +X, only −Y — **0 counts** in the locked direction, the free ones — thousands; with no locks everything passes. During active physical mouse movement 33 of 1041 `0x55` polls and 5 of 216 snapshots were lost (they repeat by themselves, no effect on the result) |
| revision 3, `controls_test --soak 30` (2026-10-12) | minutes 1–2: HID 998–999 Hz, 99.8–99.9 % at 1 ms, exact path, clicks 34/34/34, after 20 s idle the first report in 1.0–1.8 ms, memory a stable 2 MB. In minute 3 the board's USB3 "went silent" (network and mouse vanished, only reconnecting the board helped) — the symptom that the test build V4.098 fixes; the transport correctly waited for the link |
| revision 4, `controls_test --axes 5` and `--clicks` (2026-10-30, the owner moved the mouse) | X, Y, only +X, only −Y — **0 counts** in the locked direction, 8 lock commands instead of 12; clicks 25/25, press()→Windows avg 1.6–2.8 ms (max 4.0); link `--check` fine |
| revision 4, `probes/track_test.exe --reflock 20` | locking both axes with `setLocks({X, Y})`: physical movement stops after 1.3 ms (max 2.3), see "Button injection and locks" |
| movement + buttons (revision 2), 5 ms poll | 2000 = 2000, 99.3–99.9 % of intervals at 1 ms, polls with no losses |
| `0x53` events over UDP, `udp_buttons_any.py` (Python with the firewall rule) | 91–147 events per 8 s phase with presses, match the `0x55` poll |
| buttons, `example_line.exe --buttons 15` (no rule, network Public) | events 0 (the firewall drops them), the transport itself switched to the 5 ms poll: all 5 buttons, presses left 17 / right 9 / middle 20 / side1 16 / side2 19 |

For comparison on the same board: Raw `0x69` over UDP in a closed loop ~284 Hz, `0x18` is exact up to ~280–300 commands/s.
Full tables: [`../MAKCU_RESPONSE_UDP.md`](../MAKCU_RESPONSE_UDP.md), section 4.

## Self-check

```
build_example.bat
example_line.exe --check     connection + firmware + Raw API, the cursor does not move
example_line.exe             2 s: 1 s right, 1 s left at 2 counts/ms (total 0), do not touch the mouse
example_line.exe --buttons 15   show the buttons for 15 s (press all five), the cursor does not move
controls_test.exe --clicks       each button 5 times programmatically; Windows must see 5/5 presses and releases
controls_test.exe --locks 15     side1, side2, middle are locked: press them - the transport sees, Windows does not
controls_test.exe --axes 10      axis locks: X, Y, +X, -Y, nothing for 10 s each - move the mouse in all directions
controls_test.exe --reconnect 60 movement + a lock; unplug the network cable for 3-5 s - the link returns by itself
controls_test.exe --soak 30      30 minutes: 1 kHz movement, clicks, locks, idles; a per-minute log
```

`controls_test` opens its own window and confines the cursor in it: all clicks land in it. Esc from
any window or the window's X is a correct exit (buttons are released, locks are cleared).

`[+] OK` is expected: accepted by the board = records, rejections 0, queue empty, the board clean. HID ~1000 Hz and
~100 % of intervals at 1 ms. The measurement window includes `finish()`, so the rate shows slightly below 1000. A couple
of dozen extra hook events against the number of records (2010 against 2000) are the mouse's own reports.

## Protocol on the wire (for reference)

A datagram: `0x55 | nonce u64 | DE AD | LEN u16 | CMD u8 | PAYLOAD`. The reply carries the same nonce, the payload
from byte 14. One command per datagram.

- `0x6C BATCH` (V4.097+): `01 | flags | N | 00 | X u32 | N × [id u32 | x i16 | y i16 | dt u16]`, length
  exactly `8 + 10N`. `flags bit0`: free the terminal records with `id ≤ X`. `X` is no greater than the highest
  terminal id already reported by a receipt, otherwise `255 / reason 1` with no consequences.
- Receipt: `[0]=1 [1]=0 all / 1 partial / 255 rejected [2]=N [3]=accepted [4]=first failure
  [5]=reason [6]=freed [7]=free [8]=K rows (≤ 7) [9]=more [10]=queued [11]=0
  [12..15]=last_id`, then `K × (id u32 | state u8 | reason u8)` in ascending id order. Records are accepted
  in a row from the first; `state 2` = handed to the USB endpoint, `3` = rejected.
- `0x6A RAW_ACK`: `id = 0` → metadata `schema, last_accepted_id, free_records, capacity (16)`;
  `(id, consume = 1)` → free a terminal record (cleaning up the tail in `open()`/`finish()`).
- IDs strictly increase within one board boot. The board's capacity is 16 records.
- `0x55 PHYS_BUTTONS` (V4.036+): an empty request → `mask:u8` of physical buttons. The reply takes ~1 ms.
- `SET 0x11…0x15` (LEFT…SIDE2): `state:u8` (1 press, 0 release), **no reply**. `GET` of the same opcodes
  returns only the injected state, not the physical one.
- `SET LOCK 0x60`: `target:u8 state:u8`, **no reply**; `GET LOCK 0x60 target:u8` → `state:u8`. Targets:
  0 left, 1 right, 2 middle, 3 side1, 4 side2, 5 X, 6 +X, 7 −X, 8 Y, 9 +Y, 10 −Y, 11 wheel, 12 wheel+,
  13 wheel−. A lock acts only on the physical state; injection bypasses it.
- `SET MOVE_MASK 0x16`: `left:u8 right:u8 down:u8 up:u8`, **no reply** — the same bits 7, 6, 9, 10 as
  `0x60`, in one command. `SET WHEEL_MASK 0x17`: `down:u8 up:u8` → bits 13, 12.
- `GET 0x56 INJECT_SNAPSHOT` (V4.036+): 40 bytes; `[0]` injected buttons (bit0…4), `[1..2]` locks u16.
- After a command without a reply the next one passes at ≥ 1–1.5 ms with the mouse still (2026-10-12), but
  after a poll and with a hand on the mouse 2 ms is sometimes not enough (2026-10-30). The transport waits 3 ms.
- `0x52 INPUT_STREAM`: `GET kind:u8` → `enabled:u8`; `SET kind:u8 enabled:u8` (no reply). kind 1 = mouse.
- `0x53 INPUT_CHANGE` (unsolicited): `kind:u8 id:u8 value:u8`, for the mouse id = button 0…4, value 0/1;
  `kind FF FF` = overflow. Over UDP it arrives with the source port equal to the client's port.
- `0x18 MOVE` has no reply over UDP: a loss is invisible, the ceiling is ~280 commands/s. Not suitable for the reference.
