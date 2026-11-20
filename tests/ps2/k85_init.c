/* The boot script (kernel version 46, apps/mxos/init): run after the
   kernel's start with INIT.ELF and this RC.SH in its directory (the card's
   mc0:/MXOS/ for a kernel tried from host:) and k85_init.elf there too:

     set W world
     setting init.test "hello $W"
     run k85_init.elf child
     setting init.status $?
     if equal $? 7
       setting init.if yes
     else
       setting init.if no
     fi
     if not exists nothere.elf
       setting init.not yes
     fi

   Checks that init ran it (its settings are there, `run` gave the child's
   status 7) and that they hold for this boot only: not in SETTINGS.DAT.
     k85_init.elf          (status 0 expected; "child": ends with 7) */
#include "t.h"
#include <mx/kernel.h>
#include <mx/setting.h>
#include <mx/sif.h>
#include <mx/string.h>

static int is(const char *key, const char *want) {
  char v[MX_SETTING_VALUE];
  int n = mx_setting_get(key, v, sizeof v);
  mx_printf("k85: %s = %s\n", key, n >= 0 ? v : "(not set)");
  return n >= 0 && !strcmp(v, want);
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "child"))
    return 7;
  CHECK(mx__kernel && mx__kernel->version >= 46);
  CHECK(is("init.test", "hello world"));
  CHECK(is("init.status", "7"));
  CHECK(is("init.if", "yes"));
  CHECK(is("init.not", "yes"));
  /* this boot's only: SETTINGS.DAT has none of them */
  static char buf[4096];
  int fd = mx_fio_open("mc0:/MXOS/SETTINGS.DAT", MX_FIO_RDONLY), n = 0;
  if (fd >= 0) {
    n = mx_fio_read(fd, buf, sizeof buf - 1);
    mx_fio_close(fd);
  }
  buf[n > 0 ? n : 0] = 0;
  int found = 0;
  for (char *p = buf; *p; p++)
    found |= !strncmp(p, "init.", 5);
  mx_printf("k85: SETTINGS.DAT %d bytes, init.* in it: %d\n", n, found);
  CHECK(!found);
  return t_done("k85");
}
