/* The system folder (kernel version 47): system.root, shell.path,
   swap.path as the kernel sees them, the swap file's state and the boot's
   notifications. Prints them; status 0 when the kernel is 47 or later.
     k87_root.elf */
#include "t.h"
#include <mx/kernel.h>
#include <mx/setting.h>
#include <mx/notify.h>
#include <mx/dev.h>

int main(void) {
  CHECK(mx__kernel && mx__kernel->version >= 47);
  static const char *const keys[] = {"system.root", "shell.path", "swap.path", "swap.size",
                                     "storage.usb", "storage.ata", "storage.mx4sio"};
  for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
    char v[MX_SETTING_VALUE];
    int n = mx_setting_get(keys[i], v, sizeof v);
    mx_printf("k87: %s = %s\n", keys[i], n > 0 ? v : "(none)");
  }
  struct mx_swap_info s;
  if (mx_swap_info(&s) == 0)
    mx_printf("k87: swap: %s, %u MiB, state %u, device %s\n", s.path[0] ? s.path : "(none)", s.mib,
              s.state, s.device);
  static struct mx_notify n[32];
  int k = mx_notify_read(0, n, 32);
  for (int i = 0; i < k; i++)
    mx_printf("k87: notify %u at %u s%s: %s: %s\n", n[i].id, n[i].time, n[i].urgent ? " (urgent)" : "",
              n[i].title, n[i].text);
  return t_done("k87");
}
