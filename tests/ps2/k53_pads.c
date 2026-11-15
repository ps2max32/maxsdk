/* The controllers for the shell (kernel version 28, <mx/pad.h>): every
   port's mx_pad_info, the paired pads and the pairing's state, then for the
   given seconds each change (MX_EVENT_PAD) with the list again. Plug pads in
   and out, press PS on a paired one, by hand.
     k53_pads.elf [seconds]          (status 0)
     as the kernel's shell: k53_pads.elf seconds pair | forget <id> | off <port>
   Not the shell, without those: mx_pad_pair, mx_pad_forget and
   mx_pad_disconnect are refused (checked). */
#include <mx/msg.h>
#include <mx/notify.h>
#include <mx/pad.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/thread.h>
#include "t.h"

static const char *kind_name(int k) {
  static const char *n[] = {"none", "PS2", "DS3", "DS4", "DualSense"};
  return k >= 0 && k <= 4 ? n[k] : "?";
}
static const char *link_name(int l) {
  static const char *n[] = {"port", "USB", "Bluetooth"};
  return l >= 0 && l <= 2 ? n[l] : "?";
}

static void show(void) {
  mx_printf("k53:   gen %u\n", mx_pad_gen());
  struct mx_pad_info x;
  CHECK_EQ(mx_pad_info(MX_PAD_PORTS, &x), -1);
  for (int port = 0; port < MX_PAD_PORTS; port++) {
    struct mx_pad_info i;
    int r = mx_pad_info(port, &i);
    if (r < 0) {
      mx_printf("k53: mx_pad_info: -1 (kernel before 28)\n");
      t_fail++;
      return;
    }
    if (!r)
      continue;
    mx_printf("k53:   port %d: %s on %s %u, battery %u%s%s, light %02x%02x%02x, id %08x\n", port,
              kind_name(i.kind), link_name(i.link), i.hw, i.battery,
              i.flags & MX_PAD_INFO_CABLE ? ", cable" : "", i.flags & MX_PAD_INFO_CHARGING ? ", charging" : "",
              i.rgb[0], i.rgb[1], i.rgb[2], i.id);
  }
  struct mx_pad_paired p[8];
  int n = mx_pad_paired(p, 8);
  mx_printf("k53:   %d paired\n", n);
  for (int k = 0; k < n; k++)
    mx_printf("k53:     %08x %s %02x:%02x:%02x:%02x:%02x:%02x%s\n", p[k].id, kind_name(p[k].kind), p[k].addr[0],
              p[k].addr[1], p[k].addr[2], p[k].addr[3], p[k].addr[4], p[k].addr[5],
              p[k].connected ? ", connected" : "");
  struct mx_pad_pairing s;
  if (mx_pad_pair_state(&s) == 0)
    mx_printf("k53:   pairing: state %u (seq %u), id %08x, %s\n", s.state, s.seq, s.id, kind_name(s.kind));
}

static int is_shell(void) {
  struct mx_proc_info p[8];
  int n = mx_proc_list(p, 8);
  for (int i = 0; i < n && i < 8; i++)
    if (p[i].id == mx_proc_self())
      return (p[i].flags & MX_PROC_SHELL) != 0;
  return 0;
}

static volatile int done;
static u8 tstack[4096] MX_ALIGNED(16) MX_CORE_DATA;
static u32 secs = 20;
static void timer(void *arg) {
  (void)arg;
  mx_sleep_us(secs * 1000000u);
  done = 1;
  mx_notify(0, 0, "k53", "the time ran out"); /* an event that wakes the loop */
}

int main(int argc, char **argv) {
  if (argc > 1) {
    secs = 0;
    for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
      secs = secs * 10 + (u32)(*p - '0');
  }
  int shell = is_shell();
  const char *what = argc > 2 ? argv[2] : "";
  mx_printf("k53: shell %d\n", shell);
  /* pair, forget <id hex>, off <port>: the shell's calls (a test kernel
     lets a host's program make them) */
  if (!strcmp(what, "pair")) {
    CHECK_EQ(mx_pad_pair(30), 0);
    mx_printf("k53: pairing for 30 s: hold a pad's pairing buttons (DualSense: Create + PS, DS4: Share + PS)\n");
  } else if (!strcmp(what, "forget") && argc > 3) {
    u32 id = 0;
    for (const char *p = argv[3]; *p; p++)
      id = id << 4 | (u32)(*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
    mx_printf("k53: forget %08x: %d\n", id, mx_pad_forget(id));
  } else if (!strcmp(what, "off") && argc > 3) {
    mx_printf("k53: disconnect port %c: %d\n", argv[3][0], mx_pad_disconnect(argv[3][0] - '0'));
  } else if (!shell) {
    CHECK_EQ(mx_pad_pair(10), -1);
    CHECK_EQ(mx_pad_forget(1), -1);
    CHECK_EQ(mx_pad_disconnect(0), -1);
  }
  mx_pad_init();
  show();
  int port = mx_port_create("k53");
  CHECK(port > 0);
  CHECK_EQ(mx_proc_events(port), 0);
  CHECK(mx_thread_start(timer, 0, tstack, sizeof tstack, 60) > 0);
  mx_printf("k53: watching for %u s\n", secs);
  while (!done) {
    struct mx_event e;
    struct mx_msg m;
    if (mx_msg_receive(port, &e, sizeof e, &m) > 0 && e.type == MX_EVENT_PAD) {
      mx_printf("k53: MX_EVENT_PAD %d\n", e.status);
      show();
    }
  }
  show();
  return t_done("k53");
}
