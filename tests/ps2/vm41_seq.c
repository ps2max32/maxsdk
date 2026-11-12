/* Programs one after the other on one kernel, as a host's runs on the PS2
   (mx OS plan 4b: a file of one program's VM, its swap file or its ELF, must
   not be closed or reused while the VM serves the next): this one starts
   each program its arguments name and waits for it; each must end with 0.
     vm41_seq.elf host:vm11_vmlink.elf host:vm12_elfpage.elf ...
       (status 0 expected) */

#include "t.h"
#include <mx/loader.h>
#include <mx/string.h>

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    u32 n = (u32)strlen(argv[i]) + 1;
    int st = mx_loader_launch(1, argv[i], n, 0);
    mx_printf("vm41: %s ended with %d\n", argv[i], st);
    CHECK_EQ(st, 0);
  }
  return t_done("vm41");
}
