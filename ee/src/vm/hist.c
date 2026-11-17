/* <mx/vm.h>'s mx_vm_refill_hist. Built once for all libraries: under
   mx-kernel the kernel's VM is asked (version 35); a program with the VM of
   its own (libmxkern) has vm.c's. */
#include <mx/kernel.h>
#include <mx/vm.h>

extern int mx__vm_refill_hist(struct mx_vm_hot *out, int max) __attribute__((weak));

int mx_vm_refill_hist(struct mx_vm_hot *out, int max) {
  if (mx__kernel && mx__kernel->version >= 35 &&
      mx__kernel->size >= __builtin_offsetof(struct mxk_services, vm_refill_hist) + sizeof(void *))
    return mx__kernel->vm_refill_hist(out, max);
  return mx__vm_refill_hist ? mx__vm_refill_hist(out, max) : -1;
}
