/* Sockets (<mx/net.h>): UDP and TCP against sock_host.py on the host, and a
   connection the other way. Loads mxsock.irx itself, so the console stays.

     p29_sock host:mxsock.irx <a.b.c.d of the host> [stage]

   stage: 1 only loads the module, 2 opens and closes a socket, 3 adds UDP, 4 TCP,
   5 (the default) all, 6 leaves a socket open, to see the next run find it closed.
   A first argument "-" takes the service the IOP has. */

#include <mx/iop.h>
#include <mx/mx.h>
#include <mx/net.h>
#include <mx/sif.h>
#include <mx/timer.h>

#define UDP_PORT 18770
#define TCP_PORT 18771
#define LISTEN_PORT 18772
#define BYTES (2u * 1024 * 1024)

static u8 irx[64 * 1024] MX_ALIGNED(64);
static u8 buf[4096] MX_ALIGNED(64);

/* cycles since t0 are over seconds */
static int late(u32 t0, u32 seconds) { return mx_count() - t0 > seconds * MX_EE_HZ; }

#define FAIL(...)                                                                          \
  do {                                                                                     \
    mx_printf("p29: FAILED: " __VA_ARGS__);                                                \
    return 1;                                                                              \
  } while (0)

static int test_udp(u32 host) {
  int s = mx_net_socket(MX_NET_UDP);
  if (s < 0 || mx_net_bind(s, 18773) < 0)
    FAIL("udp socket %d\n", s);
  u32 t0 = mx_count(), rtt = 0;
  for (u32 i = 0; i < 100; i++) {
    u32 sent = i * 7 + 1, ip = 0, port = 0, t1 = mx_count();
    if (mx_net_sendto(s, &sent, 4, host, UDP_PORT) != 4)
      FAIL("sendto\n");
    int n;
    while ((n = mx_net_recvfrom(s, buf, sizeof buf, &ip, &port)) == MX_NET_AGAIN)
      if (late(t0, 5))
        FAIL("no echo %u\n", i);
    if (n != 4 || *(u32 *)buf != sent || ip != host || port != UDP_PORT)
      FAIL("echo %d from %x:%u\n", n, ip, port);
    rtt += mx_count() - t1;
  }
  mx_printf("p29: udp: 100 echoes, %u us each\n", rtt / 100 / (MX_EE_HZ / 1000000u));
  return mx_net_close(s) < 0;
}

/* BYTES out and back through the echo server */
static int test_tcp(u32 host) {
  int s = mx_net_socket(MX_NET_TCP), r;
  u32 t0 = mx_count();
  if (s < 0)
    FAIL("tcp socket\n");
  while ((r = mx_net_connect(s, host, TCP_PORT)) == MX_NET_AGAIN)
    if (late(t0, 5))
      FAIL("connect\n");
  if (r < 0)
    FAIL("connect %d\n", r);
  u32 sent = 0, got = 0;
  t0 = mx_count();
  while (got < BYTES) {
    if (late(t0, 12))
      FAIL("tcp stalled at %u %u\n", sent, got);
    if (sent < BYTES) {
      u32 n = BYTES - sent > sizeof buf ? sizeof buf : BYTES - sent;
      for (u32 i = 0; i < n; i++)
        buf[i] = (u8)(sent + i);
      int w = mx_net_send(s, buf, n);
      if (w > 0)
        sent += (u32)w;
      else if (w != MX_NET_AGAIN)
        FAIL("send %d\n", w);
    }
    int n = mx_net_recv(s, buf, sizeof buf);
    if (n == 0 || (n < 0 && n != MX_NET_AGAIN))
      FAIL("recv %d at %u\n", n, got);
    for (int i = 0; i < n; i++)
      if (buf[i] != (u8)(got + (u32)i))
        FAIL("wrong byte at %u\n", got + (u32)i);
    if (n > 0)
      got += (u32)n;
  }
  u32 ms = (mx_count() - t0) / (MX_EE_HZ / 1000u);
  mx_printf("p29: tcp: %u KiB out and back in %u ms\n", BYTES / 1024, ms);
  return mx_net_close(s) < 0;
}

static int test_listen(void) {
  int l = mx_net_socket(MX_NET_TCP);
  if (l < 0 || mx_net_bind(l, LISTEN_PORT) < 0 || mx_net_listen(l, 2) < 0)
    FAIL("listen\n");
  mx_printf("p29: listening\n");
  u32 t0 = mx_count(), ip = 0, port = 0;
  int c;
  while ((c = mx_net_accept(l, &ip, &port)) == MX_NET_AGAIN)
    if (late(t0, 14))
      FAIL("nobody came\n");
  if (c < 0)
    FAIL("accept %d\n", c);
  int n;
  while ((n = mx_net_recv(c, buf, sizeof buf)) == MX_NET_AGAIN)
    if (late(t0, 12))
      FAIL("no data\n");
  if (n != 5 || mx_net_send(c, buf, 5) != 5)
    FAIL("hello %d\n", n);
  while ((n = mx_net_recv(c, buf, sizeof buf)) == MX_NET_AGAIN)
    if (late(t0, 12))
      FAIL("no end\n");
  if (n != 0)
    FAIL("end %d\n", n);
  if (mx_net_recv(99, buf, 4) != -9)
    FAIL("a bad socket\n");
  mx_printf("p29: accepted, the other side closed\n");
  mx_net_close(c);
  return mx_net_close(l) < 0;
}

/* a line that reaches the host, even if the IOP stops right after it */
#define STEP(...)                                                                          \
  do {                                                                                     \
    mx_printf("p29: " __VA_ARGS__);                                                        \
    mx_delay_us(200000);                                                                   \
  } while (0)

int main(int argc, char **argv) {
  int bad = 1, stage = argc > 3 ? argv[3][0] - '0' : 5;
  if (argc < 3 || mx_sif_init() < 0)
    goto end;
  if (argv[1][0] != '-') {
    int fd = mx_fio_open(argv[1], MX_FIO_RDONLY);
    int size = fd < 0 ? -1 : mx_fio_read(fd, irx, sizeof irx);
    if (fd >= 0)
      mx_fio_close(fd);
    int res = 0;
    STEP("read %d bytes of the module\n", size);
    int id = size > 0 ? mx_iop_load_module_buffer(irx, (u32)size, 0, 0, &res) : -1;
    STEP("module %d, result %d\n", id, res);
    bad = id < 0;
    if (bad || stage == 1)
      goto end;
  }
  bad = 1;
  int init = mx_net_init();
  STEP("init %d\n", init);
  if (init < 0)
    goto end;
  int s = mx_net_socket(MX_NET_UDP);
  STEP("socket %d\n", s);
  int c = s < 0 ? -1 : mx_net_close(s);
  STEP("close %d\n", c);
  if (s < 0 || c < 0)
    goto end;
  bad = 0;
  if (stage == 2)
    goto end;
  if (stage == 6) {
    int l = mx_net_socket(MX_NET_TCP);
    bad = l < 0 || mx_net_bind(l, LISTEN_PORT) < 0 || mx_net_listen(l, 2) < 0;
    STEP("left listening on socket %d\n", l);
    goto end;
  }
  u32 host = 0, part = 0, shift = 0;
  for (const char *p = argv[2];; p++) {
    if (*p == '.' || !*p) {
      host |= part << shift, shift += 8, part = 0;
      if (!*p)
        break;
    } else
      part = part * 10 + (u32)(*p - '0');
  }
  bad = test_udp(host);
  if (!bad && stage >= 4)
    bad = test_tcp(host);
  if (!bad && stage >= 5)
    bad = test_listen();
end:
  mx_printf("p29: %s\n", bad ? "FAILED" : "passed");
  MX_TEST_END(bad);
  return bad;
}
