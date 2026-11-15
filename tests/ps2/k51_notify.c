/* Devices, the swap file and notifications (kernel version 26, mx OS plan
   4e): prints the devices, the swap file's state and the notifications in
   the ring, then for a while every event (MX_EVENT_DEVICE, _NOTIFY, _PAD)
   with the new notifications and the device list again. Plug a USB drive or
   a controller in and out meanwhile.
     k51_notify.elf [seconds] [pads]  (default 60; pads: the controller ports
                                       open, so the kernel watches them; status 0) */
#include "t.h"
#include <mx/dev.h>
#include <mx/kernel.h>
#include <mx/msg.h>
#include <mx/notify.h>
#include <mx/pad.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/thread.h>

static void devices(void) {
  struct mx_dev_info d[8];
  int n = mx_dev_list(d, 8);
  mx_printf("k51: %d devices (gen %u)\n", n, mx_dev_gen());
  for (int i = 0; i < n && i < 8; i++)
    mx_printf("k51:   %-8s kind %u %s fs %u flags %x size %u MiB free %u MiB '%s'\n", d[i].name,
              d[i].kind, d[i].present ? "there" : "gone ", d[i].fs, d[i].flags,
              (u32)(d[i].size >> 20), (u32)(d[i].free >> 20), d[i].label);
}

static u32 seen, secs = 60;
static volatile int done;
static u8 tstack[8192] MX_ALIGNED(16) MX_CORE_BSS;
/* the time's end: a notification of its own, which ends the wait */
static void timer(void *arg) {
  (void)arg;
  for (u32 i = 0; i < secs * 10; i++) {
    mx_sleep_us(100000);
    if (i % 20 == 0)
      mx_printf("k51: pads seen by the program: %d %d\n", mx_pad_connected(0), mx_pad_connected(1));
  }
  mx_notify(0, 0, "k51 end", "the time ran out");
}

static void notes(void) {
  struct mx_notify n[8];
  int got;
  while ((got = mx_notify_read(seen, n, 8)) > 0)
    for (int i = 0; i < got; i++) {
      mx_printf("k51: note %u (seq %u, %us) kind %u%s action %u [%s] %s: %s\n", n[i].id, n[i].seq,
                n[i].time, n[i].kind, n[i].urgent ? " URGENT" : "", n[i].action, n[i].source,
                n[i].title, n[i].text);
      seen = n[i].seq;
      if (!strcmp(n[i].title, "k51 end"))
        done = 1;
    }
}

int main(int argc, char **argv) {
  CHECK(mx__kernel && mx__kernel->version >= 26);
  if (argc > 1) {
    secs = 0;
    for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
      secs = secs * 10 + (u32)(*p - '0');
  }
  if (argc > 2 && !strcmp(argv[2], "pads"))
    mx_printf("k51: controller ports open: %d\n", mx_pad_init());
  devices();
  struct mx_swap_info s;
  CHECK_EQ(mx_swap_info(&s), 0);
  mx_printf("k51: swap '%s' %u MiB, state %u, device '%s'\n", s.path, s.mib, s.state, s.device);
  notes();
  CHECK(mx_notify(0, 0, "k51", "a program's notification") > 0);
  notes();
  int port = mx_port_create("k51");
  CHECK(port > 0);
  CHECK_EQ(mx_proc_events(port), 0);
  mx_printf("k51: watching for %u s: plug a USB drive or a controller in and out\n", secs);
  CHECK(mx_thread_start(timer, 0, tstack, sizeof tstack, 60) > 0);
  u32 events = 0;
  while (!done) {
    struct mx_event e;
    struct mx_msg m;
    if (mx_msg_receive(port, &e, sizeof e, &m) > 0) {
      events++;
      mx_printf("k51: event type %u status %d\n", e.type, e.status);
      if (e.type == MX_EVENT_DEVICE)
        devices();
      notes();
    }
  }
  mx_printf("k51: %u events\n", events);
  return t_done("k51");
}
