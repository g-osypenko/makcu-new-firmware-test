# Instructions for an LLM: the MAKCU project

*Short English summary of [`CLAUDE.md`](CLAUDE.md), which is the full version (commands, test table, how to read results). As of 2026-10-30.*

Two single-header C++ transports for the **MAKCU** board: COM (`makcu_raw_transport.h`) and UDP (`makcu_udp_batch_transport.h`), same API.
Bench: `basics/track_bench.h` with `track_test_com.cpp` and `track_test.cpp`.

**Owner's rules:** report in Russian; warn before every live test what the hand must do; the test window text must match the chat, because the
owner follows the window; cursor-moving tests only in their own clipped window; do not reverse the firmware and do not update it for now
(board on V4.097); Raw Input and hooks only in tests; document in detail for an LLM; never run COM and UDP tests at the same time.

**Open:**
1. COM revision 5 is on branch `com-rev5`; `master` still has revision 3.
2. UDP does not verify `km.screen` (`SET 0x64` has no reply).
3. Report holes on COM: 31.8–32.4 or 97.6–112.8 ms vanish while the board reports everything as sent; same rate on revisions 3 and 5; cause unknown.
4. Tail with a hand moving the mouse: COM 4.5–5.4 ms, UDP 6–9 ms; likely the owner's "3–4 frames" on UDP.
5. USB3 hang on 4097: during long UDP runs the mouse and network die until the board is replugged. Warn before long UDP runs.
6. Not run: COM `--soak 30`, UDP revision 5 `--reconnect` with tracking and `--soak`; G HUB as a cause of the holes.

**`km.screen` 32767×32767** is not a monitor setting: it is the bounds of the board's position counter, which Windows and games never see. At
1920×1080 the counter clamps 960/540 counts from the centre and the count is lost; 32767 is the field maximum and gives ±16383 counts.
`setTracking()` sets and restores it by itself.

**Refuted, do not retest:** holes caused by separate `0x6A` writes (revision 3 shows the same holes); COM tail reaching 12 ms quickly; `0x6A` showing the queue.
A cause cannot be claimed from a handful of rare events: run the old build in the same scenario.
