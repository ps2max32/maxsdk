/* The kernel's proc_shell (version 25) as the shell sees it: at its start
   and a second later (libmxk's mx_pad_buttons ends any program but the
   shell at the home button).
     as the kernel's shell: k54_isshell.elf     (status 0: 1 both times) */
#include <mx/kernel.h>
#include <mx/thread.h>
#include "t.h"

int main(void) {
  int a = mx__kernel->proc_shell();
  mx_sleep_us(1000000);
  int b = mx__kernel->proc_shell();
  mx_printf("k54: proc_shell at the start %d, a second later %d\n", a, b);
  CHECK_EQ(a, 1);
  CHECK_EQ(b, 1);
  return t_done("k54");
}
