/* A program's files stay open while its child runs and after the child ends
   (kernel version 50): the kernel closes what the ending program left open,
   not the files of a parent that waits for it (mxio knows each file's
   process). This one opens its own ELF, reads a part, starts itself as the
   child, which opens files of its own and ends leaving them open, then reads
   the same part again through the same descriptor and compares.
     k92_files.elf     (status 0 expected; on the PS2, with mxio) */
#include "t.h"
#include <mx/kernel.h>
#include <mx/proc.h>
#include <mx/string.h>

static u8 a[4096] MX_ALIGNED(64), b[4096] MX_ALIGNED(64);

int main(int argc, char **argv) {
  if (argc > 1) { /* the child: files of its own, left open */
    for (int i = 0; i < 3; i++)
      if (mx_fio_open(argv[0], MX_FIO_RDONLY) < 0)
        return 9;
    return 5;
  }
  CHECK(mx__kernel && mx__kernel->version >= 50);
  int fd = mx_fio_open(argv[0], MX_FIO_RDONLY);
  CHECK(fd >= 0);
  CHECK(mx_fio_lseek(fd, 1024, MX_FIO_SEEK_SET) == 1024 && mx_fio_read(fd, a, sizeof a) == (int)sizeof a);
  for (int round = 0; round < 3; round++) {
    static const char args[] = "k92_files.elf\0child";
    int es = -1, id = mx_proc_spawn(2, args, sizeof args, 0);
    CHECK(id > 0 && mx_proc_wait(id, &es, 0) == 1 && es == 5);
    memset(b, 0, sizeof b);
    int s = mx_fio_lseek(fd, 1024, MX_FIO_SEEK_SET), r = mx_fio_read(fd, b, sizeof b);
    mx_printf("k92: round %d: seek %d, read %d, %s\n", round, s, r, memcmp(a, b, sizeof a) ? "DIFFERENT" : "the same");
    CHECK(s == 1024 && r == (int)sizeof b && !memcmp(a, b, sizeof a));
  }
  CHECK(mx_fio_close(fd) == 0);
  return t_done("k92");
}
