#!/usr/bin/env python3
"""The host's side of net.c: send MB MiB to the PS2, take as much back, then
send one byte. Gives up after its timeouts, whatever the PS2 does.

  net_host.py [ip [pace]]   pace: most bytes/s to send, 0 for no limit"""
import socket, struct, sys, time

ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.10"
PORT, TOTAL = 18765, 8 << 20
end = time.time() + 40
while True:
    try:
        s = socket.create_connection((ip, PORT), timeout=2)
        break
    except OSError:
        if time.time() > end:
            sys.exit("net_host: nobody listens")
        time.sleep(0.2)
s.settimeout(30)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
pace = int(sys.argv[2]) if len(sys.argv) > 2 else 0
if pace:
    s.setsockopt(socket.SOL_SOCKET, 47, pace)  # SO_MAX_PACING_RATE
block = bytes(64 * 1024)
sent = 0
t0 = time.time()
while sent < TOTAL:
    s.sendall(block)
    sent += len(block)
got = 0
while got < TOTAL:
    data = s.recv(1 << 16)
    if not data:
        sys.exit(f"net_host: closed after {got}")
    if not got:  # the PS2 has all that was sent
        i = struct.unpack("8B24I", s.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 104))[8:]
        print(f"net_host: to the PS2 in {time.time() - t0:.2f} s, {i[23]} segments sent again, "
              f"rto {i[0] // 1000} ms, rtt {i[15] / 1000:.1f} ms, cwnd {i[18]}")
    got += len(data)
s.sendall(b"!")
print(f"net_host: done in {time.time() - t0:.2f} s")
