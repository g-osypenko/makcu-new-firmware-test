# MAKCU: context, setup and where everything lives

*English translation of [`CONTEXT_AND_SETUP.md`](CONTEXT_AND_SETUP.md) (Russian original). Links point to the English (`ENG_`) versions of the documents where one exists. Source-code comments and program output are still in Russian.*

Entry point of the project. Current as of **2026-10-12**. Details are in the READMEs of the folders, linked below.

> ## The perfect transport (start here)
>
> - **UDP / Ethernet, ~1000 Hz** — [`basics/udp_way/perfect-transport-udp/`](basics/udp_way/perfect-transport-udp/ENG_README.md) (`makcu_udp_batch_transport.h`)
> - **COM / USB2, ~1000 Hz** — [`basics/comport_way/perfect-transport-comport/`](basics/comport_way/perfect-transport-comport/ENG_README.md) (`makcu_raw_transport.h`)
>
> One WinAPI header each, the same API (movement, buttons, injection, locks, auto-reconnect).
> Russian original: [`CONTEXT_AND_SETUP.md`](CONTEXT_AND_SETUP.md).

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

## 1. Goal and result

**Goal:** a C++ module that moves the cursor through a **MAKCU** board at **1000 Hz** with an exact path
(everything sent arrives in Windows), and reads the mouse buttons. The main channel is the **local network
(UDP)**, so that Windows sees no COM port for the board. The fallback channel is COM.

**Result:**

| Channel | Movement | How | Reference |
| --- | --- | --- | --- |
| COM (USB2, CH343) | **~1000 Hz**, exact path | Raw `0x69` + receipts `0x6A` | [`basics/comport_way/perfect-transport-comport/`](basics/comport_way/perfect-transport-comport/ENG_README.md) |
| UDP (Ethernet) | **~1000 Hz** (970–1001 Hz, 99 % of intervals 1 ms), exact path | batch `0x6C`, closed loop, ≤ 7 records per packet | [`basics/udp_way/perfect-transport-udp/`](basics/udp_way/perfect-transport-udp/ENG_README.md) |

Both references read the **physical mouse buttons** (left, right, middle, two side buttons): `buttons()`,
`buttonDown()`, `pressCount()`. Both are a single WinAPI header, and can be included in the same program.

Both references (revision 3, same API) also can: **press buttons programmatically**
(`press`/`release`; ~1.5 ms to Windows over UDP, ~1 ms over COM), **lock physical input**
(`setLock`: buttons, axes, directions — the game does not see it, the transport does), **reconnect by
themselves** after a drop (UDP — the network, COM — USB2 unplugged) and restore the locks, and
`open()`/`close()` bring the board to a clean state.

**UDP reference, revision 5 (2026-10-30)** — UDP only so far, COM next:
- **the cursor stops at once.** The movement queue is capped at 5 records (`setMaxLag`). Without a cap it
  grew, and after 10 s of movement the injection kept going for 8 ms ("freezes 3–4 frames late"). Now the
  tail is 3.6 ms, and `stopMovement()` stops the cursor after 2.7–3.6 ms;
- **both axes locked with one command** `0x16` (`setLocks({X, Y})`): physical movement stops after 1.3 ms;
- **live-mouse counts** `setTracking`/`physical()`: the board position `0x63` while there is no injection.
  The sum matches Raw Input to the count.

Details and measurements: [`ENG_HANDOFF_MOUSE_TRACKING.md`](ENG_HANDOFF_MOUSE_TRACKING.md).

Verification status as of 2026-10-12:

| | UDP revision 3 | COM revision 3 |
| --- | --- | --- |
| 1 kHz movement, exact path | yes | yes (2000 = 2000, 2-min soak: 999 Hz, 99.8 % at 1 ms) |
| clicks `--clicks` | 25/25 | 25/25, avg 0.75–1.19 ms |
| locks `--locks` (physical input does not reach Windows) | yes | yes (run by the owner, numbers not recorded) |
| axis locks `--axes` (X, Y, +X, −Y: physical input does not reach Windows) | yes, 0 counts in the locked direction | yes, 0 counts |
| reconnect `--reconnect` | yes, 14.3 s after the cable | yes, after unplugging USB2 (run by the owner) |
| long run `--soak 30` | 2 min clean, the board's USB3 died in the 3rd (firmware, see §4) | 14 min clean (606,938 records, no losses), interrupted externally at minute 15–16; the full 30 minutes in a row has not been completed |

## 2. Hardware setup

MAKCU connectors:

| Connector | Goes to | Purpose |
| --- | --- | --- |
| **USB 1** (PC, left) | PC | power + the HID mouse that Windows sees (a clone of a real mouse's descriptor) |
| **USB 2** (COM, middle) | PC — **COM path only**; disconnected for the network scenario | CH343 bridge `1A86:55D3`, COM5 for us, 4,000,000 baud |
| **USB 3** (Host, right) | **UGREEN 60554** hub | the board's USB host: mouse + network card |

The **UGREEN 60554** hub (USB 3.0 → 3×USB 3.0 + gigabit RJ45): Genesys Logic hub chip `05E3:0610`,
network card **ASIX AX88179 `0B95:1790`** (not Realtek, as was thought earlier). A
**Logitech Lightspeed `046D:C547`** receiver, polling at 1000 Hz, is plugged into the same hub. The hub's USB-C power input does not affect the result.

Network: the PC's onboard Realtek 2.5GbE card (1 Gbit/s link) → switch → the hub's RJ45.

> The injection rate cannot exceed the USB 3 mouse's polling rate: the board copies its interval.
> A 125 Hz mouse gives 125 Hz injection.

`MAKCU Virtual COM Port (COM8)` in Device Manager is a separate channel for the Toolkit and has nothing to do
with the network path.

## 3. Network

| | |
| --- | --- |
| Board IP | `192.168.50.175` (router DHCP; MAC `6c-1f-f7-c5-c6-68`, OUI Ugreen) |
| Command port | UDP `8080` |
| PC IP on this network | `192.168.50.87` (`Ethernet` interface) |
| UDP encryption | off (the AES key is written into the board once and cannot be changed — do not enable it without need) |
| Windows Firewall | `Ethernet` network = **Public**, Public firewall on. Added the rule **"MAKCU UDP events"**: inbound UDP from `192.168.50.175` for any program. Without it button events over UDP are dropped — see the "Required setup" block at the top of the file |

If the board gets a different IP, find it by MAC `6c-1f-f7-...` and pass `--host`.

## 4. Firmware

- Currently **V4.097**. Since 2026-10-11 it is MAKCU's **official** firmware (`current_official` in their
  MCP `firmware_versions`); before that we installed it as a test build supplied by support. It is the first
  with batch **`0x6C`**, which UDP 1000 Hz relies on. Older builds (V4.095 and earlier) do not support
  `0x6C` — do not roll back to them.
- Test builds on top of 4097: **V4.098** (the USB3 side restarts itself after a hang) and **V4.099**
  (+ injection with wireless receivers is no longer held back after 9–18 s of mouse idle — our case with the
  Lightspeed). Both were "not yet tested on hardware" as of 2026-10-12.
- Interpolation `0x1F` = 0. It does not affect Raw/batch.
- Correspondence with MAKCU engineers (through the support LLM bot on Discord) and all measurements:
  [`basics/udp_way/MAKCU_RESPONSE_UDP.md`](basics/udp_way/MAKCU_RESPONSE_UDP.md) (already in English).

## 5. Protocol on the wire

MAK_API frame: `DE AD | LEN u16 | CMD u8 | PAYLOAD`. Over UDP there is a RAW wrapper:
`0x55 | nonce u64 | DE AD | LEN | CMD | PAYLOAD`, and the reply carries the same nonce. A bare frame without the wrapper
is ignored by the board over UDP. One command per datagram.

What is used for what:

| Task | Command | Note |
| --- | --- | --- |
| 1000 Hz movement over UDP | **`0x6C` batch** | ≤ 7 records `(id, x, y, dt)` per packet, one receipt; wait for the receipt, `X` = the highest terminal id in it |
| 1000 Hz movement over COM | **`0x69` RAW_MOVE + `0x6A`** | each record = exactly one HID report; over UDP only ~284 Hz (2 datagrams per move) |
| Movement "the old way" | `0x18` MOVE | goes through the firmware interpolator: ~640–700 Hz over COM, ≤ ~280 commands/s over UDP and no reply |
| Press/release a button (injection) | SET `0x11…0x15` (LEFT…SIDE2), `1`/`0` | no reply. UDP: pause ≥ 1.5 ms after it; COM: no pause needed, but the command is occasionally lost — verify with a snapshot |
| Lock physical input | SET `0x60` LOCK `target state`; all movement directions at once — SET `0x16` MOVE_MASK `left right down up` (wheel `0x17`) | 14 targets; no reply; the lock does not affect injection or button reading. `0x16` sets the same bits in one command: over UDP physical movement stops after ~1.3 ms (UDP reference revision 4, `setLocks({X, Y})`); a series of `0x60` gave 7.7 ms |
| Check injection and locks | GET `0x56` INJECT_SNAPSHOT | `[0]` injected buttons, `[1..2]` lock mask |
| Read physical buttons | **GET `0x55` PHYS_BUTTONS** → mask | bit0 left … bit4 side2 |
| Button events | `0x52` INPUT_STREAM (enable) → events **`0x53`** | `DE AD 03 00 53 01 <button> <0/1>`; `FF FF` = overflow |
| Raw metadata | `0x6A` with `id = 0` | `last_accepted_id`, free records (capacity 16) |

## 6. Pitfalls (time was lost on each of these)

1. **UDP: one command in flight.** Each command goes to the board's second chip over an internal channel
   that carries one command at a time. A datagram that arrives during a transfer is **silently dropped**. Hence
   the closed loop: the next command only after the reply to the previous one.
2. **GET `0x11…0x15` do not see the physical button** — only the injected state. Physical state comes from `0x55`
   and the `0x53` events (and the text `km.left()` …).
3. **`0x53` events over UDP come from the board's port equal to the client's port, not from 8080.** A socket
   with `connect()` drops them. The firewall on a Public network drops them without the rule from §3.
4. **An event carries the nonce of one of our earlier commands.** Parse `0x53` by opcode before matching
   replies by nonce, otherwise it will be taken for a receipt.
5. **Raw records on the board live until reboot.** An interrupted program leaves occupied slots (there are 16) —
   the references clean them up on open.
6. **"Enhance pointer precision"** in Windows distorts the cursor path. The movers turn it off while running.
7. **The COM port is exclusive:** only one program can open it (including the mak-suite SDK).
8. **Commands without a reply** (buttons `0x11…0x15`, locks `0x60`, `SET 0x52`) over UDP: the next command
   sooner than ~1 ms later is silently lost. Use a 3 ms pause and verify with a `0x56` snapshot. 2 ms is enough with the mouse
   still, but after board polling and with a hand on the mouse every 4th command of a series was lost (measured 2026-10-30,
   `ENG_HANDOFF_MOUSE_TRACKING.md`). **Over COM** no pause is needed
   (SET → SET/GET without a pause 300/300), but the `SET` itself is occasionally lost (0.3–0.7 %, sometimes in bursts) —
   snapshot verification is mandatory there too (a snapshot over COM replies in ~0.3 ms).
9. **An axis lock is two bits** (+ and −); the whole-axis bit is set by the board itself. Clearing "+X" while the
   X axis is locked leaves only "−X".
10. **Injected buttons and locks are not released by the firmware itself.** If the program crashed or closed without a
    connection, they stay on the board until the next reference `open()` (it cleans up) or a board reboot.

## 7. Where everything lives

| Path | What |
| --- | --- |
| [`basics/udp_way/perfect-transport-udp/`](basics/udp_way/perfect-transport-udp/ENG_README.md) | **UDP reference**: `makcu_udp_batch_transport.h` + example/self-check |
| [`basics/comport_way/perfect-transport-comport/`](basics/comport_way/perfect-transport-comport/ENG_README.md) | **COM reference**: `makcu_raw_transport.h` + example/self-check + `controls_test` |
| [`basics/udp_way/`](basics/udp_way/ENG_README.md) | UDP mover (`makcu_udp_mover.exe`: trajectories, F7 — format, buttons), bench probes `udp_18_closed`/`udp_raw_batch`, `probes/` of measurements on 4094, a script bundle for engineers |
| [`basics/comport_way/`](basics/comport_way/ENG_README.md) | COM mover (`makcu_mover.exe`), shared mover headers (`motion_engine.h`, HID/path monitors), the 1000 Hz solution over COM: [`SOLUTION_1000HZ_RAW_MOVE.md`](basics/comport_way/ENG_SOLUTION_1000HZ_RAW_MOVE.md), board settings snapshot |
| `mak-suite/` | submodule: the official `makxd` SDK and protocol docs (`protocol/MAK_API.md`, `KM_API.md`) |
| `ABCurves/` | submodule: a generator of "human" trajectories, used by the movers (`--abc`) |
| [`legacy-python-exp/`](legacy-python-exp/ENG_README.md) | early Python experiments of 6–8 October, do not use |
| `build/` | build object files (ABCurves, mak-suite), outside git |

## 8. Build and run

- Compiler: MinGW-w64 g++ 15.3 (`C:\mingw64\bin`), C++23 for the movers, C++17 is enough for the references.
- `basics\udp_way\build.bat`, `basics\comport_way\build.bat` — the movers; `build_example.bat` in each
  reference's folder.
- Self-check: `example_line.exe --check` (connection), `example_line.exe` (2 s of movement, do not touch the mouse),
  `example_line.exe --buttons 15` (buttons). For the UDP reference add `--host <IP>` if the IP is different.
- Buttons/locks/drop/long run: `controls_test.exe --clicks | --locks S | --axes S | --reconnect S |
  --soak M | --draw S | --watch S` in each reference's folder (UDP: `--host`, COM: `--port`). It opens its own window,
  the cursor is confined to it, Esc exits.
- Python probes: `.venv\Scripts\python.exe` or `C:\anaconda3\python.exe` (it has an allowing
  firewall rule).

## 9. Sources

- Command reference: <https://makcu.com/en/api/>, opcode matrix by build: <https://makcu.com/api/versions>.
- MAKCU documentation MCP server: `https://makcu.com/mcp` (tools: `search_docs`, `get_command`, `firmware_versions`).
- Support: the MAKCU Discord bot (an LLM relay to the engineers).
- Owner's decision: we do not reverse-engineer the board's firmware.
