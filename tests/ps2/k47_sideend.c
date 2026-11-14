/* mx OS plan step 4c: a process beside the one in front ends alone. This
   program (libmxk without VM, its core at a run of its own) starts the
   other in front (spawn) and then ends, by returning (exit) or by a crash
   (crash): the other goes on and its end is the run's. Started by mxlink:
     k47_sideend.elf exit host:vm32_side.elf     (status 0 expected)
     k47_sideend.elf crash host:vm32_side.elf    (status 0 expected)
   Before step 4c this one's end ended the other: the run ended with 3, or
   with 255. */
#include "t.h"
#include <mx/loader.h>
#include <mx/proc.h>
#include <mx/string.h>

MX_CORE_HEAP(512 << 10);

int main(int argc, char **argv) {
  if (argc < 3) {
    mx_printf("k47: usage: k47_sideend.elf exit|crash <program> [args]\n");
    return t_done("k47");
  }
  static char args[256];
  u32 n = 0;
  for (int i = 2; i < argc; i++) {
    u32 l = (u32)strlen(argv[i]) + 1;
    if (n + l > sizeof args)
      break;
    memcpy(args + n, argv[i], l);
    n += l;
  }
  int id = mx_proc_spawn(argc - 2, args, n, 0);
  mx_printf("k47: %s started as process %d; this one (%d) ends now: %s\n", argv[2], id,
            mx_proc_self(), argv[1]);
  if (id <= 0)
    return 1;
  if (!strcmp(argv[1], "crash"))
    *(volatile u32 *)4 = 1; /* address 4: nothing there */
  return 3;
}
