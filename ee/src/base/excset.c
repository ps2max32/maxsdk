/* Where maxsdk sets a syscall handler or an exception handler (exception.c,
   the VM): the kernel's own syscall entry and exception table once it has
   them (mx OS plan step 2h-4, apps/mxos/ee/ksys.S), else the BIOS's
   SetSyscall, SetVTLBRefillHandler and SetVCommonHandler. Not in libmxk:
   a program sets neither. */

#include <mx/bios.h>

/* the kernel's (weak: none in a standalone ELF); 0 when it took the
   handler, -1 while it does not have its entry or table yet */
int mx__kernel_syscall_set(int number, void *handler) __attribute__((weak));
int mx__kernel_handler_set(int code, void (*handler)(void), void **old) __attribute__((weak));

void mx__set_syscall(int number, void *handler) {
  if (mx__kernel_syscall_set && mx__kernel_syscall_set(number, handler) == 0)
    return;
  mx_bios_set_syscall(number, handler);
}

/* code 1-3 the TLB's (refill, load, store), else a common one (4-7, 9-13);
   the handler before */
void *mx__set_handler(int code, void (*handler)(void)) {
  void *old;
  if (mx__kernel_handler_set && mx__kernel_handler_set(code, handler, &old) == 0)
    return old;
  return code >= 1 && code <= 3 ? mx_bios_set_tlb_handler(code, handler)
                                : mx_bios_set_common_handler(code, handler);
}
