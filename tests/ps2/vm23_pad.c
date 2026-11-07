/* tests/ps2/pad.c in VM, under mx-kernel: from table version 6 the pad is the
   kernel's (mx_pad_* through it), on an older kernel the program's own. */
#include <mx/kernel.h>
#define main pad_main
#include "pad.c"
#undef main

int main(void) {
  mx_printf("vm23_pad: kernel version %u, %s pad\n", mx__kernel ? mx__kernel->version : 0,
            mx__kernel && mx__kernel->version >= 6 ? "the kernel's" : "the program's own");
  return pad_main();
}
