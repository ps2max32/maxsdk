/* mx_pad_rumble through the kernel (version 11): for the given seconds
   (default 30) each port's pad rumbles while Cross (the big motor) or
   Square (the small one) is held. At the end every pad's big motor is
   turned on and the program ends without turning it off: the kernel stops
   it at the program's end. */

#include <mx/kernel.h>
#include <mx/mx.h>
#include <mx/pad.h>
#include <mx/thread.h>

#define PORTS 4

int main(int argc, char **argv) {
  int secs = 0;
  for (const char *c = argc > 1 ? argv[1] : "30"; *c >= '0' && *c <= '9'; c++)
    secs = secs * 10 + (*c - '0');
  mx_printf("vm31_rumble: kernel version %u, %d s: Cross big motor, Square small motor\n",
            mx__kernel ? mx__kernel->version : 0, secs);
  mx_pad_init();
  u32 was[PORTS] = {0};
  for (int t = 0; t < secs * 50; t++) {
    mx_sleep_us(20000);
    for (int p = 0; p < PORTS; p++) {
      if (!mx_pad_connected(p))
        continue;
      u32 b = mx_pad_buttons(p) & (MX_PAD_CROSS | MX_PAD_SQUARE);
      if (b == was[p])
        continue;
      int r = mx_pad_rumble(p, b & MX_PAD_SQUARE ? 1 : 0, b & MX_PAD_CROSS ? 200 : 0);
      mx_printf("vm31_rumble: port %d: small %u big %u: %d\n", p, b & MX_PAD_SQUARE ? 1 : 0,
                b & MX_PAD_CROSS ? 200 : 0, r);
      was[p] = b;
    }
  }
  for (int p = 0; p < PORTS; p++)
    if (mx_pad_connected(p))
      mx_printf("vm31_rumble: port %d: big motor on at the end: %d\n", p, mx_pad_rumble(p, 0, 120));
  mx_sleep_us(500000);
  mx_printf("vm31_rumble: end, the motors left on (the kernel stops them)\n");
  MX_TEST_END(0);
  return 0;
}
