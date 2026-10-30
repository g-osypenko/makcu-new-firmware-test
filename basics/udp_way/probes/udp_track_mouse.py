# Physical mouse over UDP: what can be tracked. Static part: firmware 0x04, PHYS_BUTTONS 0x55,
# INPUT_STREAM mouse 0x52, SCREEN 0x64, GETPOS 0x63 (binary) and km.getpos() via the 0x6B text tunnel.
# Live part (--seconds): enables INPUT_STREAM mouse, closed loop (one command in flight) alternating
# GET 0x55 and GET 0x63; prints 0x53 events, mask changes and every --every-th position change.
# Restores the stream state. Cursor is never moved; km.screen is only read.
#
#   python udp_track_mouse.py [--host 192.168.50.175] [--seconds 20] [--every 10]
import argparse
import socket
import struct
import time

ap = argparse.ArgumentParser()
ap.add_argument("--host", default="192.168.50.175")
ap.add_argument("--seconds", type=float, default=0)
ap.add_argument("--every", type=int, default=10)
a = ap.parse_args()
B = (a.host, 8080)

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
try:
    s.ioctl(socket.SIO_UDP_CONNRESET, False)
except Exception:
    pass
s.setblocking(False)
seq = int(time.time() * 1000) << 8
events = []  # (t, id, value, src)
NAMES = ["L", "R", "M", "S1", "S2"]


def frame(op, pl=b""):
    return b"\xDE\xAD" + struct.pack("<H", len(pl)) + bytes([op]) + pl


def pump(until, want=None):
    """Read datagrams until `until`; 0x53 is dispatched by opcode first; returns the reply frame for nonce `want`."""
    while True:
        try:
            d, src = s.recvfrom(4096)
        except BlockingIOError:
            if time.perf_counter() >= until:
                return None
            continue
        f = d[9:]
        if len(f) >= 8 and f[:2] == b"\xDE\xAD" and f[4] == 0x53:
            events.append((time.perf_counter(), f[6], f[7], src))
            continue
        if want is not None and struct.unpack("<Q", d[1:9])[0] == want:
            return f


def ask(op, pl=b"", timeout=0.05):
    global seq
    seq += 1
    s.sendto(b"\x55" + struct.pack("<Q", seq) + frame(op, pl), B)
    return pump(time.perf_counter() + timeout, seq)


def set_noreply(op, pl):
    global seq
    seq += 1
    s.sendto(b"\x55" + struct.pack("<Q", seq) + frame(op, pl), B)
    pump(time.perf_counter() + 0.003)


def getpos():
    r = ask(0x63)
    return None if r is None or len(r) < 9 else struct.unpack("<HH", r[5:9])


fw = ask(0x04)
print("link:", "OK" if fw else "NO REPLY", "| FIRMWARE 0x04:", struct.unpack("<I", fw[5:9])[0] if fw else None)
m = ask(0x55)
print("PHYS_BUTTONS 0x55:", f"mask={m[5]:05b}" if m else None)
st = ask(0x52, b"\x01")
print("INPUT_STREAM mouse:", st[5] if st else None)
sc = ask(0x64)
print("SCREEN 0x64:", struct.unpack("<HH", sc[5:9]) if sc else None, "| GETPOS 0x63:", getpos())
t = ask(0x6B, b"m.getpos()", 0.1)
print("text 0x6B m.getpos():", t[4:].decode(errors="replace").split("\r\n")[0] if t else None)

if a.seconds > 0 and st:
    set_noreply(0x52, b"\x01\x01")
    m = ask(0x55)
    mask = m[5] if m else 0
    pos = getpos()
    print(f"\n>>> live {a.seconds:.0f} s: move the mouse, click buttons. start mask={mask:05b} pos={pos}")
    t0 = time.perf_counter()
    polls = pos_polls = pos_changes = mask_changes = lost = 0
    ev_seen = 0
    while time.perf_counter() - t0 < a.seconds:
        r = ask(0x55)
        if r is None:
            lost += 1
        else:
            polls += 1
            if r[5] != mask:
                mask_changes += 1
                print(f"  {time.perf_counter()-t0:6.2f}s poll 0x55 mask {mask:05b} -> {r[5]:05b}")
                mask = r[5]
        p = getpos()
        if p is None:
            lost += 1
        else:
            pos_polls += 1
            if p != pos:
                pos_changes += 1
                if pos_changes % a.every == 1:
                    print(f"  {time.perf_counter()-t0:6.2f}s getpos {pos} -> {p}")
                pos = p
        for te, i, v, src in events[ev_seen:]:
            nm = NAMES[i] if i < 5 else ("OVERFLOW" if i == 0xFF else str(i))
            print(f"  {te-t0:6.2f}s EVENT 0x53 {nm} {'down' if v else 'up'} (from port {src[1]})")
        ev_seen = len(events)
    set_noreply(0x52, bytes([1, st[5]]))
    sec = time.perf_counter() - t0
    print(f"\nsummary: {polls} 0x55 polls, {pos_polls} getpos polls ({pos_polls/sec:.0f} Hz), {lost} lost, "
          f"getpos changed {pos_changes}x (last {pos}), 0x53 events {len(events)}, poll mask changes {mask_changes}")
    r = ask(0x52, b"\x01")
    print("stream restored to", r[5] if r else None)
