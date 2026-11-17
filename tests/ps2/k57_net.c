/* The network's state (kernel version 30, <mx/net.h>'s mx_net_status):
   the state now, then for the given seconds each change (MX_EVENT_NET) with
   the state again. Pull the cable and put it back, by hand.
     k57_net.elf [seconds]                  (status 0)
     as the kernel's shell: k57_net.elf seconds dhcp
                            k57_net.elf seconds fixed <ip> <mask> <gw> [dns]
       (the settings net.*, applied by the kernel a second later)
   The address parts are checked against each other: an address got means a
   mask; DHCP bound means a lease. */
#include <mx/msg.h>
#include <mx/net.h>
#include <mx/notify.h>
#include <mx/proc.h>
#include <mx/setting.h>
#include <mx/string.h>
#include <mx/thread.h>
#include "t.h"

static void ip(char *to, u32 a) {
  mx_snprintf(to, 16, "%u.%u.%u.%u", a & 255, a >> 8 & 255, a >> 16 & 255, a >> 24);
}

static void show(void) {
  struct mx_net_status s;
  if (mx_net_status(&s) < 0) {
    mx_printf("k57: mx_net_status: -1 (kernel before 30)\n");
    t_fail++;
    return;
  }
  char a[16], m[16], g[16], d[16];
  ip(a, s.ip), ip(m, s.mask), ip(g, s.gw), ip(d, s.dns);
  static const char *dhcp[] = {"off", "asking", "bound"};
  mx_printf("k57:   up %u, link %u (%u Mbit/s%s), mac %02x:%02x:%02x:%02x:%02x:%02x, gen %u\n", s.up,
            s.link, s.speed, s.duplex ? " full duplex" : "", s.mac[0], s.mac[1], s.mac[2], s.mac[3],
            s.mac[4], s.mac[5], s.gen);
  mx_printf("k57:   %s mask %s gw %s dns %s, DHCP %s, lease %u s\n", a, m, g, d,
            s.dhcp <= 2 ? dhcp[s.dhcp] : "?", s.lease);
  if (!s.up)
    return;
  CHECK(s.mac[0] | s.mac[1] | s.mac[2] | s.mac[3] | s.mac[4] | s.mac[5]);
  CHECK(!s.link || s.speed == 10 || s.speed == 100);
  CHECK(!s.ip || s.mask);
  CHECK(s.dhcp != MX_NET_DHCP_BOUND || s.lease);
}

static volatile int done;
static u8 tstack[4096] MX_ALIGNED(16) MX_IRQ_DATA;
static u32 secs = 10;
static void timer(void *arg) {
  (void)arg;
  mx_sleep_us(secs * 1000000u);
  done = 1;
  mx_notify(0, 0, "k57", "the time ran out"); /* an event that wakes the loop */
}

int main(int argc, char **argv) {
  if (argc > 1) {
    secs = 0;
    for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
      secs = secs * 10 + (u32)(*p - '0');
  }
  const char *what = argc > 2 ? argv[2] : "";
  if (!strcmp(what, "dhcp")) {
    CHECK_EQ(mx_setting_set("net.mode", "dhcp"), 0);
  } else if (!strcmp(what, "fixed") && argc > 5) {
    CHECK_EQ(mx_setting_set("net.mode", "manual"), 0);
    CHECK_EQ(mx_setting_set("net.ip", argv[3]), 0);
    CHECK_EQ(mx_setting_set("net.mask", argv[4]), 0);
    CHECK_EQ(mx_setting_set("net.gw", argv[5]), 0);
    CHECK_EQ(mx_setting_set("net.dns", argc > 6 ? argv[6] : ""), 0);
  }
  show();
  int port = mx_port_create("k57");
  CHECK(port > 0);
  CHECK_EQ(mx_proc_events(port), 0);
  CHECK(mx_thread_start(timer, 0, tstack, sizeof tstack, 60) > 0);
  mx_printf("k57: watching for %u s\n", secs);
  while (!done) {
    struct mx_event e;
    struct mx_msg m;
    if (mx_msg_receive(port, &e, sizeof e, &m) > 0 && e.type == MX_EVENT_NET) {
      mx_printf("k57: MX_EVENT_NET %d\n", e.status);
      show();
    }
  }
  show();
  return t_done("k57");
}
