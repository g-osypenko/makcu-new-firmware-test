# Handoff: tracking the real mouse through MAKCU (2026-10-24)

Task for a dedicated session: figure out how to read the **physical mouse** (movement and buttons) through
the board over COM and over UDP. Pick a method, measure it and, if it works, build it into both reference
transports. Project overview: `ENG_CONTEXT_AND_SETUP.md` (read first).

## What is already known

- **Buttons are solved** in both references: `0x53` events (stream `0x52`) plus a safety `0x55` poll
  (`buttons()`, `buttonDown()`, `pressCount()`). 2026-10-24 over UDP: 16 `0x53` events for 8 clicks,
  the `0x55` poll caught 14 of 16 changes (the poll misses short clicks, the events do not).
- **No V4 firmware has a movement stream**, including the official V4.108 (2026-10-19).
  `mak-suite/protocol/MAK_API.md`: "Mouse motion and wheel are not emitted". `km.axis` (a stream of X/Y/wheel
  deltas) existed only in V3 and is not among the V4 commands. V4.105+ added axis streams for game
  controllers only (`0x73`/`0x74` → `0x75` events).
- **The only way to see movement is the position the board keeps:**
  - **GET `0x63` GETPOS** (empty request) → `x:u16 y:u16` LE (example `de ad 04 00 63 c9 00 f7 02` = 201, 759).
    Text form `km.getpos()` (over UDP through the `0x6B` tunnel: `frame(0x6B, b"m.getpos()")` →
    `km.getpos(201,759)\r\n>>> `).
  - **GET `0x64` SCREEN** → `w:u16 h:u16`, currently 1920×1080 (the default). `km.screen(w,h)` (1…32767)
    sets the size **and moves the position to the centre**.
  - Per the docs the position is "an estimate from every mouse report the PC accepted, **clamped to the
    screen**". So it holds physical movement **plus our injection**, it is in mouse counts (not Windows
    pixels), and movement past the edges is lost: in the live run x hit 0/1919 and y hit 0/1079.
- **UDP measurements (board V4.097, `192.168.50.175`), 2026-10-24:**
  - `0x63` in a closed loop: 1161/1161 replies in 2 s → **~580 Hz**; the text `km.getpos()` gives the same.
    Alternating `0x55` + `0x63`: ~290 Hz each.
  - Live 20 s run with the owner moving the mouse: the position changed 877 times and followed the hand.
- **COM was not tested**: on 2026-10-24 the USB2 cable was unplugged (no CH343 `1A86:55D3` in the system).
  It was still unplugged on 2026-10-30. The board is reachable over UDP: firmware 4097, `km.screen`
  1920×1080, the `0x52` mouse stream is off.

## New firmware (MCP `firmware_versions`, checked 2026-10-30)

- Since 4108 only test, lab and diagnostic builds **V4.109…V4.122** (2026-10-24…27) have come out.
  **V4.108** is still the official one. No opcodes or km commands were added after 4105. **There is still
  no mouse motion stream in 4122.** The descriptions of `0x60`/`0x63`/`0x64`/`km.getpos`/`km.screen` did
  not change. Documented `0x60` targets: 0 left … 4 side2, 5 X, 6 +X, 7 −X, 8 Y, 9 +Y, 10 −Y, 11 wheel,
  12 wheel up, 13 wheel down. "A lock is the same state as the matching mask": `km.move_mask`/`km.lock_*`
  from another program or the Toolkit change the same bits.
- **V4.111 / V4.114 (lab, not for customers):** "while software injection is active, MAKCU no longer
  slows down how often it reads the physical mouse". So on 4097/4108 the physical mouse **is read less
  often during injection**. This matters for measurement 3 (subtracting injection) and option B. The sum is
  probably kept while the report count drops — to be checked.
- **V4.119 / V4.121 / V4.122 (test, based on 4108):** the USB3 device reconnects by itself after MAKCU
  restarts. The mouse recovers by itself when its input stalls after a lost internal acknowledgement.
- **In 4108 (since V4.103):** `km.autorelease(ms)` / `0x6E` and `km.heartbeat()` / `0x6F` release the
  **injected buttons** when the program goes silent. They do **not** clear `0x60` locks: after a program
  crash the axis stays locked until `open()` or a board reboot. `km.api_version()` / `0x70` (since V4.105).
- The V4.110 note mentions "the mouse 1 ms polling rewrite introduced in V4.108". It changes nothing for
  our Lightspeed (already 1 kHz). The claim "injection cannot exceed the mouse polling rate"
  (`ENG_CONTEXT_AND_SETUP.md` §2) may be outdated for 4108.
- Probe: `basics/udp_way/probes/udp_track_mouse.py [--seconds 20]`. Reads the firmware, `0x55`, `0x52`,
  `0x64`, `0x63` and the text `km.getpos()`; in live mode prints events, mask and position changes.
  Does not move the cursor, only reads `km.screen`, returns the `0x52` stream to its previous state.

## Results 2026-10-30 (UDP, board V4.097)

**The owner's answers:**
- what is needed is physical mouse counts **before a lock starts**; during a lock or injection the position
  is not needed;
- the main thing is a fairly accurate count, ideally every report;
- PC-side Raw Input/hooks only in tests, in production everything goes through the board;
- check why movement is cut off 3–4 frames late.

Rig: `basics/udp_way/probes/track_test.cpp` (build line in the file header). A test window with Raw Input
in focus, its own UDP link to the board (one command in flight) or the reference transport.

| Measurement | Result |
| --- | --- |
| `--accuracy 20`: back-to-back `0x63` polls, a hand moving the mouse freely | **509 Hz**, 0 lost, path 54,988 counts. Every poll matched the Raw Input sum to within the neighbouring report (100 %, max error 0), 83 % at exactly the same moment. **Total after stopping: discrepancy 0,0.** On average 1.22 mouse reports between polls (max 3), position step per poll up to 35 counts. The board position runs ~0.5 ms ahead of Raw Input (lag +2.2…2.5 ms from sending the request, RTT ~2 ms) |
| `--lock 16`: X+Y lock, 400 ms, `0x63` polled under the lock | **the board position does not count movement under a lock** (0 counts over 400 ms with a hand on the mouse). After unlock the "position − Raw Input" shift is 0…±4 (one report of alignment): nothing lost or added |
| `--lock 16`, commands compared | one `SET 0x16 MOVE_MASK`: physical movement stops after **2.0 ms (max 2.5)**, leak 3–15 counts. 4×`0x60` 2 ms apart (as in reference revision 3): **one of the four commands was lost in 8 of 8 attempts**, the Y axis leaked for the whole 400 ms |
| `--lockloss`, where commands get lost | without polling before the series, 620/620 land even at 2 ms (with and without a hand, any `km.screen`). **`0x63` polling right up to the lock + a hand on the mouse**: at 2 ms one of four `0x60` is lost in 33 of 46 attempts, at 3 and 4 ms 0 of 46, a single `0x16` 0 of 45 |
| `--reflock`: `tx.setLock(X)` + `tx.setLock(Y)` in reference revision 3 | **7.65 ms median, max 12.3**, leak 3–23 counts, −Y (the last direction) leaks ~8 ms. This is the "3–4 frames late" |
| `--reflock` after the fix (revision 4) | `setLocks({X, Y})` = one `0x16`: **1.3 ms, max 2.3**, leak 0–11. Two `setLock` calls in a row: 4.3 ms, max 5.0 |
| `--inject 10`: 1 kHz circle through the reference, stop, hands off | the first report 1.65 ms after the first `move()`. **After the last `move()` movement continues for 5.7 ms (6 reports)**, 10 of 10 (once 7.7 ms): the transport queue, the packet in flight and the board queue. The path is exact |

**Done:** UDP reference, revision 4 (`makcu_udp_batch_transport.h`):
- movement directions are locked with one `0x16`, the wheel with one `0x17`;
- added `setLocks({…}, on)`;
- the pause after a no-reply command is 3 ms;
- verified: `controls_test --axes 5` (0 counts in locked directions), `--clicks` 25/25,
  `example_line --check`.

The COM reference only got `setLocks` for the same API (compiles, not tested on hardware: no cable).

**Conclusions for tracking:**
- Polling `0x63` gives an exact count of the physical mouse: nothing is lost from the sum, resolution ~2 ms
  (1–3 reports per poll). Each report separately through the board is impossible: no V4 has a motion
  stream.
- The position does not change under a lock, so "read before the lock" is sound: after `setLocks`, read the
  position once more after the snapshot confirms the lock. That also captures the 1–2 leaked reports that
  Windows did receive.
- The position includes our injection. Right after injecting, tail reports keep coming for up to ~6 ms
  (measurement 3): wait for them or subtract them by `move()`.

**Next:**
1. Build position reading into the UDP reference:
   - poll `0x63` in free slots while there is no movement and no lock;
   - accumulated counts in int64;
   - `km.screen` 32767×32767, re-centred under a lock;
   - decide with the owner whether it is turned on by a separate flag. Polling takes the single slot, so
     the first `move()`/`setLocks()` waits up to ~2 ms.
2. COM: connect USB2, repeat the measurements (`0x63` over COM), switch locks to `0x16`, run
   `controls_test --axes`.
3. The 5.7 ms injection tail: if it matters, look at the queue depth (merging records on stop, the `dt`
   field of `0x6C`).

## What to measure

1. **Accuracy.** Move only the physical mouse and compare Δ`getpos` with the sum of Raw Input deltas over
   the same time (`basics/comport_way/rawinput_monitor.h`). Do the counts match one to one away from the edges?
2. **Edges.** `km.screen(32767,32767)` → position in the centre, ±16k counts of headroom. How long that
   lasts, how to "re-centre" without losing movement between polls. Does `km.screen` survive a board reboot?
   What else it changes (`km.moveto` works from this position).
3. **Subtracting injection.** Physical = Δposition − Δour injection. Count the injection by receipts
   (`0x6A`/`0x6C`, the record went out in a report), not by sending. Check with 1 kHz movement + a hand:
   does the sum converge, and how much does a single poll scatter?
4. **Locks.** With axis `setLock` the physical movement does not reach the PC. Does `getpos` see it
   (probably not — "a report the PC accepted")? If not, intercepting movement (lock it and decide
   ourselves) is not possible today.
5. **Cost over UDP.** One command in flight: every `0x63` takes a slot (~1.7 ms) that would otherwise go to
   a `0x6C` batch. How a position poll rate (50/100/250 Hz) affects the reference's 1 kHz movement.
6. **COM.** The `0x63`/`0x64` reply over COM, poll rate together with `0x69/0x6A` movement (COM handles
   > 2000 frames/s — likely enough even for 1 kHz).
7. **Wheel.** There are neither events nor a position for the physical wheel — confirm there is no way.

## Options

- **A. Poll `0x63` through the board.** Works on both channels with nothing on the PC side. Downsides:
  polling rather than per report; edges; injection included; over UDP it takes slots from movement.
- **B. Raw Input / hook on the PC** (the MAKCU mouse in Windows = a clone of the `046D:C547` receiver).
  Every report, 1 kHz, no board involved. Downsides: physical + injection together as well; nothing is
  visible under locks; Windows 11 thins out Raw Input for a background window (~125 Hz, the sum is kept) —
  see `rawinput_monitor.h`, `hook_monitor.h`.
- **C. Ask MAKCU for a mouse movement stream** modelled on `km.controller_axes` (physical dx/dy before
  locks, an event per report). Through the support bot in Discord, like `basics/udp_way/MAKCU_REQUEST_UDP.md`.
  They ship new test builds within days. Prepare the request text; the owner sends it.

## Questions for the owner (ask at the start, do not decide for them)

1. What is tracking for: just seeing it (log, visualisation), mixing with injection (compensation), or
   intercepting (lock the physical input and decide ourselves what Windows gets)?
2. Is physical movement needed separately from our injection, or is the combined movement enough?
3. What rate/latency is needed: every report at 1 kHz, or an accumulated position at ~100–500 Hz?
4. Is reading the mouse on the PC side (Raw Input/hook) acceptable, or only through the board?
5. Update the firmware (the board has 4097, the official one is 4108), and write to support about a
   movement stream?

## Pitfalls

- **UDP: one command in flight** (`ENG_CONTEXT_AND_SETUP.md` §6.1). Position polling competes with movement.
- **`km.screen(w,h)` resets the position to the centre.** After experiments restore 1920×1080 (or whatever was there).
- **The `0x52` mouse stream was already on** on 2026-10-24 before we touched it (someone left it). The
  subscription belongs to whoever changed it last: with two programs using the stream at once, only one
  gets the events.
- Over UDP parse `0x53` events by opcode before the nonce; they come from the client's port — the
  "MAKCU UDP events" firewall rule is required (present, `RemoteAddress 192.168.50.175`).
- In a text reply the opcode byte = the first letter of the reply (`k`), followed by `m.getpos(x,y)\r\n>>> `.
- "Enhance pointer precision" and Windows pointer speed: board counts ≠ cursor pixels.
- Live checks need the owner to move the mouse: warn them **before** starting and give them time.
  Tests that move the cursor themselves run only in their own window with `ClipCursor`. No firmware reversing.

## How to start

1. Read `ENG_CONTEXT_AND_SETUP.md`, this file, `perfect-transport-udp/ENG_README.md` (the buttons section).
2. Ask the owner the questions above; the answers decide which option to dig into.
3. If COM is needed, ask them to plug in USB2 (COM5) and check
   `basics/comport_way/perfect-transport-comport/example_line.exe --check`.
4. Run `python basics/udp_way/probes/udp_track_mouse.py --seconds 20` (the owner moves the mouse and
   presses every button), write the same probe for COM, then do the measurements above.
5. Record results in this file, the reference READMEs and `CONTEXT_AND_SETUP.md`; commit.
