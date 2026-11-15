/* The home button in the shell (kernel version 25): libmxk asks the kernel
   (proc_shell) and does not end the shell. It needs a test kernel that
   holds buttons for a program's line "@@PAD <hex> <ms>" (not in the
   kernel's sources).
     as the kernel's shell: k55_homeshell.elf    (status 0: "still here")
     not as the shell: it ends at the home button, without "still here" */
#include <mx/pad.h>
#include <mx/thread.h>
#include <mx/mx.h>

int main(void) {
  mx_pad_init();
  mx_printf("@@PAD 10000 1500\n");
  u32 seen = 0;
  for (int i = 0; i < 100; i++) {
    seen |= mx_pad_buttons(0) | mx_pad_buttons(1);
    mx_sleep_us(20000);
  }
  mx_printf("k55: still here, home seen %d\n", (seen & MX_PAD_HOME) != 0);
  mx_printf("@@PS2TEST_END@@ status=%d\n", (seen & MX_PAD_HOME) ? 0 : 2);
  return 0;
}
