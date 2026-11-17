/* The working directory (kernel version 40, <mx/sif.h>): each process has its
   own, as on Linux. A program starts in its ELF's directory (from the host:
   "host:"); mx_fio_chdir changes it, relative paths are in it, "." and ".."
   are taken out; a child started with mx_proc_spawn gets its parent's, one
   started with mx_proc_spawn_in the one given; argv[0] is the ELF's path in
   the starter's directory. Makes host:k75dir/ and a file in it.
     k75_cwd.elf     (status 0 expected) */
#include "t.h"
#include <mx/kernel.h>
#include <mx/proc.h>
#include <mx/sif.h>
#include <mx/string.h>

static int cwd_is(const char *want) {
  char d[MX_FIO_CWD_MAX];
  if (mx_fio_getcwd(d, sizeof d) < 0)
    return 0;
  if (strcmp(d, want)) {
    mx_printf("k75: in %s, not %s\n", d, want);
    return 0;
  }
  return 1;
}

/* a child: argv[2] the directory it should be in, argv[3] its argv[0] */
static int child(char **argv) {
  int ok = cwd_is(argv[2]) && !strcmp(argv[0], argv[3]);
  if (!ok)
    mx_printf("k75: child argv[0] %s, expected %s\n", argv[0], argv[3]);
  int fd = mx_fio_open("a.txt", MX_FIO_RDONLY); /* there in k75dir/ only */
  if (fd >= 0)
    mx_fio_close(fd);
  return ok ? (fd >= 0 ? 5 : 6) : 7;
}

static int spawn(const char *dir, const char *elf, const char *want, const char *argv0) {
  static char args[256];
  u32 n = 0;
  const char *v[4] = {elf, "child", want, argv0};
  for (int i = 0; i < 4; i++) {
    u32 l = (u32)strlen(v[i]) + 1;
    memcpy(args + n, v[i], l);
    n += l;
  }
  int id = dir ? mx_proc_spawn_in(dir, 4, args, n, 0) : mx_proc_spawn(4, args, n, 0), st = -1;
  if (id <= 0 || mx_proc_wait(id, &st, 0) != 1)
    return -1;
  return st;
}

int main(int argc, char **argv) {
  if (argc > 3 && !strcmp(argv[1], "child"))
    return child(argv);
  CHECK(mx__kernel && mx__kernel->version >= 40);
  /* its ELF's directory */
  char start[MX_FIO_CWD_MAX];
  CHECK(mx_fio_getcwd(start, sizeof start) > 0);
  u32 d = 0;
  for (u32 i = 0; argv[0][i]; i++)
    if (argv[0][i] == ':' || argv[0][i] == '/')
      d = i + 1;
  CHECK(d && !strncmp(start, argv[0], d) && start[d] == 0);
  const char *elf = argv[0] + d; /* its name */
  mx_printf("k75: started in %s as %s\n", start, argv[0]);

  mx_fio_mkdir("k75dir", 0777);
  int fd = mx_fio_open("k75dir/a.txt", MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  if (fd >= 0) {
    CHECK_EQ(mx_fio_write(fd, "k75\n", 4), 0);
    mx_fio_close(fd);
  }
  /* "." and ".." */
  fd = mx_fio_open("./k75dir/../k75dir/./a.txt", MX_FIO_RDONLY);
  CHECK(fd >= 0);
  if (fd >= 0)
    mx_fio_close(fd);

  CHECK_EQ(mx_fio_chdir("k75dir"), 0);
  char want[MX_FIO_CWD_MAX];
  mx_snprintf(want, sizeof want, "%sk75dir/", start);
  CHECK(cwd_is(want));
  fd = mx_fio_open("a.txt", MX_FIO_RDONLY);
  CHECK(fd >= 0);
  if (fd >= 0)
    mx_fio_close(fd);
  CHECK(mx_fio_chdir("no-such-dir") < 0);
  CHECK(cwd_is(want));

  /* a child gets this directory; its path ("../<elf>") is in this one */
  char rel[64], abs[MX_FIO_CWD_MAX];
  mx_snprintf(rel, sizeof rel, "../%s", elf);
  mx_snprintf(abs, sizeof abs, "%s%s", start, elf);
  CHECK_EQ(spawn(0, rel, want, abs), 5);
  CHECK(cwd_is(want));

  /* back up; a child given k75dir */
  CHECK_EQ(mx_fio_chdir(".."), 0);
  CHECK(cwd_is(start));
  CHECK_EQ(spawn("k75dir", elf, want, abs), 5);
  /* one given its own directory: no a.txt there */
  CHECK_EQ(spawn(start, elf, start, abs), 6);

  mx_fio_remove("k75dir/a.txt");
  mx_fio_rmdir("k75dir");
  return t_done("k75");
}
