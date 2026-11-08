/* The kernel's pads as a program sees them (mx_pad_*), SIO2 and USB
   (mxpad.irx) together: for the given seconds (default 30) each port's
   buttons and sticks whenever they change. A USB pad takes the first port
   no SIO2 pad holds; plugging a pad into a SIO2 port moves it up. */

#include <mx/kernel.h>
#include <mx/mx.h>
#include <mx/pad.h>
#include <mx/thread.h>

#define PORTS 4

static const char *const names[16] = {"select", "L3",   "R3",       "start",  "up",    "right",
                                      "down",   "left", "L2",       "R2",     "L1",    "R1",
                                      "triangle", "circle", "cross", "square"};

static int far(u8 a, u8 b) { return (a > b ? a - b : b - a) >= 16; }

int main(int argc, char **argv) {
  int secs = 0;
  for (const char *c = argc > 1 ? argv[1] : "30"; *c >= '0' && *c <= '9'; c++)
    secs = secs * 10 + (*c - '0');
  mx_printf("vm27_pads: kernel version %u, %d s; ports 0-%d\n", mx__kernel ? mx__kernel->version : 0, secs,
            PORTS - 1);
  if (mx_pad_init() < 0)
    mx_printf("vm27_pads: no SIO2 pad driver\n");
  int was[PORTS] = {0};
  u32 last_b[PORTS] = {0};
  u8 last_st[PORTS][4] = {{0}};
  for (int t = 0; t < secs * 50; t++) {
    mx_sleep_us(20000);
    for (int p = 0; p < PORTS; p++) {
      int c = mx_pad_connected(p);
      u32 b = c ? mx_pad_buttons(p) : 0;
      u8 st[4];
      int analog = c && mx_pad_sticks(p, st);
      if (!analog)
        st[0] = st[1] = st[2] = st[3] = 128;
      if (c == was[p] && b == last_b[p] && !far(st[0], last_st[p][0]) && !far(st[1], last_st[p][1]) &&
          !far(st[2], last_st[p][2]) && !far(st[3], last_st[p][3]))
        continue;
      if (c != was[p])
        mx_printf("vm27_pads: port %d: %s\n", p, c ? "a pad" : "no pad");
      if (c) {
        mx_printf("vm27_pads: port %d: L %3u,%3u R %3u,%3u%s |", p, st[2], st[3], st[0], st[1],
                  analog ? "" : " (digital)");
        for (int i = 0; i < 16; i++)
          if (b & (1u << i))
            mx_printf(" %s", names[i]);
        mx_printf("\n");
      }
      was[p] = c, last_b[p] = b;
      for (int i = 0; i < 4; i++)
        last_st[p][i] = st[i];
    }
  }
  mx_printf("vm27_pads: end\n");
  MX_TEST_END(0);
  return 0;
}
