/* A crash in a program linked into VM: under mx-kernel the kernel reports it
   and keeps the report (mx_crash_last). Ends with status 255 from the report.
     vm21_crash.elf        break instruction, in code in VM
     vm21_crash.elf note   a store outside the VM in use: the VM's note */

#include <mx/mx.h>
#include <mx/string.h>
#include <mx/vm.h>

__attribute__((noinline)) static int level2(int note) {
  if (note)
    *(volatile u32 *)(uintptr_t)(MX_VM_LIMIT - 64) = 1;
  else
    __builtin_trap();
  return 2;
}

__attribute__((noinline)) static int level1(int note) { return level2(note) + 1; }

int main(int argc, char **argv) {
  int note = argc > 1 && strcmp(argv[1], "note") == 0;
  mx_printf("vm21: %s, expect the exception report and status 255\n", note ? "bad store" : "break");
  int r = level1(note);
  mx_printf("vm21: no exception (%d)\n", r);
  MX_TEST_END(1);
  return 1;
}
