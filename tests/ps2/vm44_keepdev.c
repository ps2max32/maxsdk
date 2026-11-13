/* The VM's files on a driver stay open over another program's end (mx OS
   plan step 3). vm43_keep has its files on host:; a file on a card or a USB
   stick is held by the file server as a handle of the IOP's file manager,
   and the kernel closed every number of that manager at a program's end:
   the server's files were closed behind its back. The shell (its ELF on
   the card) could not read its code after a game, and the next game's swap
   file (on USB) was the first file that game opened.
   A file of the card is mapped (read-only, paged from the file). Then the
   program starts itself with "child" (which opens the same file and its own
   ELF, reads, leaves both open, and ends), pushes the mapped pages out and
   reads them again: from the file.
     vm44_keepdev.elf [file]     the file: mc0:/MXOS/MXKERNEL.ELF
   Status 0 expected. Without the file (PCSX2: no card) nothing is tested:
   it says so, with status 0. */

#include "t.h"
#include <mx/loader.h>
#include <mx/sif.h>
#include <mx/string.h>
#include <mx/vm.h>

#define PAGE MX_VM_PAGE
#define NP 48
#define MAXF 8 /* pages of the file that are checked */

const struct mx_vm_config mx_vm_config = {.swap = "host:vm44.swp", .frames = 16 * PAGE};

static u8 pages[NP][PAGE];
static u8 buf[PAGE] MX_ALIGNED(64);

static u32 sum(const u8 *p, u32 n) {
  u32 s = 0;
  for (u32 i = 0; i < n; i++)
    s = s * 31 + p[i];
  return s;
}

static int child(const char *self, const char *file) {
  int a = mx_fio_open(file, MX_FIO_RDONLY);
  int na = a < 0 ? -1 : mx_fio_read(a, buf, 4096);
  int b = mx_fio_open(self, MX_FIO_RDONLY);
  int nb = b < 0 ? -1 : mx_fio_read(b, buf, 4096);
  mx_printf("vm44: the child: %s is %d, %d bytes read; %s is %d, %d bytes read; left open\n",
            file, a, na, self, b, nb);
  return na == 4096 && nb == 4096 ? 0 : 3;
}

int main(int argc, char **argv) {
  if (argc > 2 && !strcmp(argv[1], "child"))
    return child(argv[0], argv[2]);
  const char *file = argc > 1 ? argv[1] : "mc0:/MXOS/MXKERNEL.ELF";
  CHECK(mx_vm_active());

  /* what the file holds, read the plain way */
  static u32 want[MAXF];
  int fd = mx_fio_open(file, MX_FIO_RDONLY);
  if (fd < 0) {
    mx_printf("vm44: no file %s (%d): nothing tested\n", file, fd);
    return t_done("vm44");
  }
  int size = mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
  mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
  CHECK(size >= (int)PAGE);
  u32 np = (u32)size / PAGE > MAXF ? MAXF : (u32)size / PAGE;
  for (u32 k = 0; k < np; k++) {
    CHECK_EQ(mx_fio_read(fd, buf, PAGE), PAGE);
    want[k] = sum(buf, PAGE);
  }
  mx_fio_close(fd);

  u32 msize = 0;
  const u8 *m = mx_vm_map_file(file, &msize);
  CHECK(m != 0);
  CHECK_EQ(msize, size);
  if (!m)
    return t_done("vm44");
  u32 bad = 0;
  for (u32 k = 0; k < np; k++)
    bad += sum(m + k * PAGE, PAGE) != want[k];
  mx_printf("vm44: %s mapped: %u bytes, %u of %u pages wrong\n", file, msize, bad, np);
  CHECK_EQ(bad, 0);

  static char args[240];
  u32 l = (u32)strlen(argv[0]) + 1, fl = (u32)strlen(file) + 1;
  CHECK(l + 6 + fl <= sizeof args);
  memcpy(args, argv[0], l);
  memcpy(args + l, "child", 6);
  memcpy(args + l + 6, file, fl);
  int st = mx_loader_launch(3, args, l + 6 + fl, 0);
  mx_printf("vm44: the child ended with %d\n", st);
  CHECK_EQ(st, 0);

  /* more pages than frames: the mapped ones leave, and so do these */
  struct mx_vm_stats a, b;
  for (u32 k = 0; k < NP; k++)
    for (u32 i = 0; i < PAGE; i += 64)
      pages[k][i] = (u8)(k * 7 + i / 64);
  mx_vm_stats(&a);
  bad = 0;
  for (u32 k = 0; k < np; k++)
    bad += sum(m + k * PAGE, PAGE) != want[k];
  u32 pbad = 0;
  for (u32 k = 0; k < NP; k++)
    for (u32 i = 0; i < PAGE; i += 64)
      pbad += pages[k][i] != (u8)(k * 7 + i / 64);
  mx_vm_stats(&b);
  mx_printf("vm44: after it %u of %u mapped pages wrong, %u bytes of %u pages; %u from files, "
            "%u swap ins\n",
            bad, np, pbad, NP, b.file_ins - a.file_ins, b.swap_ins - a.swap_ins);
  CHECK_EQ(bad, 0);
  CHECK_EQ(pbad, 0);
  CHECK(b.file_ins - a.file_ins >= np);
  CHECK(b.swap_ins > a.swap_ins);
  mx_vm_unmap_file(m);
  return t_done("vm44");
}
