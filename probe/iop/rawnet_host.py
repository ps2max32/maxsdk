#!/usr/bin/env python3
"""The host's side of rawnet.c: frames to the PS2 at rising rates, then frames
from it, each reported by the module. Gives up after its timeouts."""
import socket, struct, sys, time

# rawnet_mx.irx adds mxeth's and mxdev9's counters (mxeth.h, mxdev9.h)
EXTRA = ("rx", "rx_bytes", "rx_err", "starved", "overrun", "untaken", "tx", "tx_err", "tx_drop",
         "dmaen_spins", "wakes", "smap_irqs", "rx_dmas", "t_irq", "t_clients", "t_dma", "t_thread", "dmas", "dma_waited", "dma_depth", "dev9_irqs",
         "dev9_rounds", "dma_spins")
extra = ()
ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.10"
PORT, DATA, TICKS = 18766, 1472, 36864000
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16 << 20)
s.connect((ip, PORT))


def ask(cmd, want, timeout=2.0):
    """Send cmd until a report of type want comes, ignoring data frames."""
    end = time.time() + timeout
    s.settimeout(0.2)
    while time.time() < end:
        try:
            s.send(cmd)
        except OSError:
            pass  # no ARP answer yet
        try:
            while True:
                d = s.recv(2048)
                if d[:1] == want and len(d) >= 32:
                    global extra
                    extra = struct.unpack(f">{(len(d) - 32) // 4}I", d[32:32 + (len(d) - 32) // 4 * 4])
                    return struct.unpack(">7I", d[4:32])
        except OSError:
            pass
    sys.exit(f"rawnet_host: no answer to {cmd[:1]}")


def line(what, sent, frames, nbytes, ticks, idle, rate, waits, drops):
    ms = ticks * 1000 / TICKS
    left = 100 * idle / ms / rate if ms and rate else 0
    print(f"{what}: {frames} of {sent} frames in {ms:.0f} ms: "
          f"{nbytes / ms / 1000:.2f} MB/s on the wire, {left:.0f}% of the IOP left, "
          f"{waits} waits, {drops} drops", flush=True)
    if extra:
        print("  " + " ".join(f"{k}={v / 36.864e3:.0f}ms" if k[:2] == "t_" else f"{k}={v}"
                              for k, v in zip(EXTRA, extra) if k != "rx_bytes"), flush=True)


t0 = time.time()
ask(b"H", b"H", 35)
print(f"rawnet_host: PS2 answers after {time.time() - t0:.1f} s", flush=True)
frame = b"D" + bytes((i * 7) & 255 for i in range(1, DATA))
for sif, mbs in ((0, 12), (1, 2), (1, 4), (1, 6), (1, 8), (1, 10), (1, 12)):
    ask(b"M" + bytes([sif]), b"M")
    ask(b"S", b"S")
    gap, n, start = DATA / (mbs * 1e6), 0, time.perf_counter()
    while (t := time.perf_counter() - start) < 1.0:
        if n * gap <= t:
            s.send(frame)
            n += 1
    time.sleep(0.05)
    line(f"receive at {mbs} MB/s{' to the EE' if sif else ''}", n, *ask(b"E", b"E"))
ask(b"M\0", b"M")
for count in (4000,):
    s.settimeout(10)
    s.send(b"T" + struct.pack(">I", count))
    got, end = 0, time.time() + 8
    r = None
    while time.time() < end and r is None:
        try:
            d = s.recv(2048)
        except OSError:
            break
        if d[:1] == b"T":
            r = struct.unpack(">7I", d[4:32])
            if len(d) > 32:
                extra = struct.unpack(f">{(len(d) - 32) // 4}I", d[32:32 + (len(d) - 32) // 4 * 4])
        elif d[:1] == b"D":
            got += 1
    if r is None:
        sys.exit(f"rawnet_host: send test: {got} frames, no report")
    line(f"send, host got {got}", count, *r)
s.send(b"Q")
time.sleep(0.1)
s.send(b"Q")
print("rawnet_host: done", flush=True)
