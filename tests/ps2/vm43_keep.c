/* The VM's files stay open over another program's end (mx OS plan step 3):
   this one's swap file and its ELF, from which its pages still come once
   the program it started has ended. On the PS2 the file server closed every
   file at a program's end: the shell could not read its own code after a
   game, and the next game's swap file was another file.
   16 frames; 48 pages written (most of them go to the swap file) and 8 pages
   of constants left in the ELF. Then it starts itself with "child" (which
   opens, reads and leaves open a file, and ends), and after it reads all of
   them: from the swap file and from the ELF.
     vm43_keep.elf     (status 0 expected) */

#include "t.h"
#include <mx/loader.h>
#include <mx/sif.h>
#include <mx/string.h>
#include <mx/vm.h>

#define PAGE MX_VM_PAGE
#define NP 48
#define NC 8

const struct mx_vm_config mx_vm_config = {.swap = "host:vm43.swp", .frames = 16 * PAGE};

static u8 pages[NP][PAGE];
static const u8 consts[NC * PAGE] = {[0 ... NC * PAGE - 1] = 0x5A};

static int child(const char *self) {
  static u8 buf[4096] __attribute__((aligned(64)));
  int fd = mx_fio_open(self, MX_FIO_RDONLY);
  int n = fd < 0 ? -1 : mx_fio_read(fd, buf, sizeof buf);
  mx_printf("vm43: the child: %s is %d, %d bytes read; left open\n", self, fd, n);
  for (u32 k = 0; k < 4; k++)
    pages[k][0] = 1;
  return n == (int)sizeof buf ? 0 : 3;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "child"))
    return child(argv[0]);
  CHECK(mx_vm_active());
  for (u32 k = 0; k < NP; k++)
    for (u32 i = 0; i < PAGE; i += 64)
      pages[k][i] = (u8)(k * 7 + i / 64);
  struct mx_vm_stats a, b;
  mx_vm_stats(&a);
  CHECK(a.swap_outs > 0);

  static char args[160];
  u32 l = (u32)strlen(argv[0]) + 1;
  CHECK(l + 6 <= sizeof args);
  memcpy(args, argv[0], l);
  memcpy(args + l, "child", 6);
  int st = mx_loader_launch(2, args, l + 6, 0);
  mx_printf("vm43: the child ended with %d\n", st);
  CHECK_EQ(st, 0);

  u32 bad = 0, cbad = 0;
  for (u32 k = 0; k < NP; k++)
    for (u32 i = 0; i < PAGE; i += 64)
      bad += pages[k][i] != (u8)(k * 7 + i / 64);
  for (u32 i = 0; i < sizeof consts; i += 64)
    cbad += ((const volatile u8 *)consts)[i] != 0x5A;
  mx_vm_stats(&b);
  mx_printf("vm43: after it %u bytes of %u pages wrong, %u of %u constant pages; %u swap ins, "
            "%u from files\n",
            bad, NP, cbad, NC, b.swap_ins - a.swap_ins, b.file_ins - a.file_ins);
  CHECK_EQ(bad, 0);
  CHECK_EQ(cbad, 0);
  CHECK(b.swap_ins > a.swap_ins);
  CHECK(b.file_ins - a.file_ins >= NC);
  return t_done("vm43");
}
