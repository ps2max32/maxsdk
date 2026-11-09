/* Started by k33_spawn (step 2e-3): "exitN" ends after 200 ms with status
   N; "forever" sleeps until ended (mx_proc_kill). Linked into VM. */
#include <mx/mx.h>
#include <mx/debug.h>
#include <mx/proc.h>
#include <mx/thread.h>

int main(int argc, char **argv) {
  const char *a = argc > 1 ? argv[1] : "exit0";
  mx_printf("vm34: process %d, %s\n", mx_proc_self(), a);
  if (a[0] == 'f')
    for (;;)
      mx_sleep_us(10000);
  mx_sleep_us(200000);
  int n = 0;
  for (const char *p = a + 4; *p >= '0' && *p <= '9'; p++)
    n = n * 10 + (*p - '0');
  return n;
}
