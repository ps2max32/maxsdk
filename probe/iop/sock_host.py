#!/usr/bin/env python3
"""The host's side of p29_sock.c: echoes on UDP 18770 and TCP 18771, then connects
to the PS2 on 18772, once it should be listening. Ends by itself.

  sock_host.py [ps2 ip [seconds before connecting, 0: never]]"""
import selectors, socket, sys, threading, time

ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.10"
delay = float(sys.argv[2]) if len(sys.argv) > 2 else 10
end = time.time() + 60


def udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("", 18770))
    s.settimeout(0.5)
    while time.time() < end:
        try:
            d, a = s.recvfrom(2048)
            s.sendto(d, a)
        except OSError:
            pass


def tcp():
    l = socket.socket()
    l.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    l.bind(("", 18771))
    l.listen(2)
    l.settimeout(0.5)
    while time.time() < end:
        try:
            c, _ = l.accept()
        except OSError:
            continue
        c.settimeout(20)
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            while True:
                d = c.recv(65536)
                if not d:
                    break
                c.sendall(d)
        except OSError:
            pass
        c.close()


def connect():
    if not delay:
        return
    time.sleep(delay)
    while time.time() < end:
        try:
            s = socket.create_connection((ip, 18772), timeout=1)
        except OSError:
            time.sleep(1)
            continue
        s.settimeout(5)
        s.sendall(b"hello")
        print("sock_host:", s.recv(16), flush=True)
        s.close()
        return


for f in (udp, tcp, connect):
    threading.Thread(target=f, daemon=True).start()
while time.time() < end:
    time.sleep(0.2)
