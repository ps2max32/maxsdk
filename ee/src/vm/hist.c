/* <mx/vm.h>'s calls that came after the kernel's first table of VM calls:
   mx_vm_refill_hist (version 35), mx_vm_map_file_flags (36), mx_vm_clean (43). Built once for
   all libraries: under mx-kernel the kernel's VM is asked; a program with
   the VM of its own (libmxkern) has vm.c's. */
#include <mx/kernel.h>
#include <mx/vm.h>

extern int mx__vm_refill_hist(struct mx_vm_hot *out, int max) __attribute__((weak));

int mx_vm_refill_hist(struct mx_vm_hot *out, int max) {
  if (mx__kernel && mx__kernel->version >= 35 &&
      mx__kernel->size >= __builtin_offsetof(struct mxk_services, vm_refill_hist) + sizeof(void *))
    return mx__kernel->vm_refill_hist(out, max);
  return mx__vm_refill_hist ? mx__vm_refill_hist(out, max) : -1;
}

extern const void *mx__vm_map_file_flags(const char *path, u32 *size, u32 flags) __attribute__((weak));

const void *mx_vm_map_file_flags(const char *path, u32 *size, u32 flags) {
  if (!flags)
    return mx_vm_map_file(path, size);
  if (mx__kernel && mx__kernel->version >= 36 &&
      mx__kernel->size >= __builtin_offsetof(struct mxk_services, vm_map_file_flags) + sizeof(void *))
    return mx__kernel->vm_map_file_flags(path, size, flags);
  return mx__vm_map_file_flags ? mx__vm_map_file_flags(path, size, flags) : 0;
}

extern int mx__vm_clean(void) __attribute__((weak));

int mx_vm_clean(void) {
  if (mx__kernel && mx__kernel->version >= 43 &&
      mx__kernel->size >= __builtin_offsetof(struct mxk_services, vm_clean) + sizeof(void *))
    return mx__kernel->vm_clean();
  return mx__vm_clean ? mx__vm_clean() : -1;
}
