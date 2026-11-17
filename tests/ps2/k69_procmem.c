/* Who holds main memory (kernel version 37, <mx/proc.h>'s mx_proc_memory; mx
   OS plan step 6: the count of what each process keeps while another is in
   front). This program starts itself as a child, which is in front, takes
   512 KiB of pinned memory, writes 1 MiB of VM and sleeps a second; the
   parent, beside it, reads the table meanwhile and prints it: the kernel,
   itself (not in front, with its core), the child (in front, with at least
   those frames), and whatever else runs (the host link's service, a shell).
   The numbers are the step's starting point: what is not in front should
   keep little.
     k69_procmem.elf     (status 0 expected) */
#include "t.h"
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/mem.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/thread.h>
#include <mx/vm.h>

MX_CORE_HEAP(256 << 10);

static volatile u8 big[1 << 20]; /* VM: the program's .bss */

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "child")) {
    volatile u8 *p = mx_dma_alloc(512 << 10);
    if (!p)
      return 6;
    for (u32 i = 0; i < sizeof big; i += 4096)
      big[i] = (u8)i;
    for (u32 i = 0; i < (512u << 10); i += 4096)
      p[i] = (u8)i;
    mx_sleep_us(1000000);
    return 5;
  }
  CHECK(mx__kernel && mx__kernel->version >= 37);
  static char args[256];
  u32 n = (u32)strlen(argv[0]) + 1;
  memcpy(args, argv[0], n);
  memcpy(args + n, "child", 6);
  int id = mx_proc_spawn(2, args, n + 6, 0), self = mx_proc_self();
  CHECK(id > 0);
  mx_sleep_us(500000); /* the child has its memory */

  static struct mx_proc_mem m[9];
  static struct mx_proc_info info[8];
  int np = mx_proc_memory(m, 9), ni = mx_proc_list(info, 8);
  struct mx_vm_stats v;
  mx_vm_stats(&v);
  mx_printf("k69: %u frames, %u free, %u pinned; %d entries\n", v.frames, v.free_frames, v.pinned_frames, np);
  CHECK(np >= 3 && np <= 9);
  u32 held = 0, back = 0, seen = 0;
  for (int i = 0; i < np && i < 9; i++) {
    const char *name = "the kernel";
    for (int k = 0; k < ni && k < 8; k++)
      if (m[i].id && info[k].id == m[i].id)
        name = info[k].name;
    mx_printf("k69: %-28s %s core %3u pinned %3u paged %3u\n", name,
              !m[i].id ? "       " : m[i].state == MX_PROC_FRONT ? "front  " : "behind ", m[i].core, m[i].pinned,
              m[i].paged);
    if (m[i].id)
      held += m[i].core + m[i].pinned;
    if (m[i].id && m[i].state != MX_PROC_FRONT)
      back += m[i].core + m[i].pinned;
    if (m[i].id == id) {
      seen |= 1;
      CHECK_EQ(m[i].state, MX_PROC_FRONT);
      CHECK(m[i].core >= 8);
      CHECK(m[i].pinned >= 16);
      CHECK(m[i].paged >= 32);
    }
    if (m[i].id == self) {
      seen |= 2;
      CHECK(m[i].state != MX_PROC_FRONT);
      CHECK(m[i].core >= 8);
    }
  }
  CHECK_EQ(m[0].id, 0);
  CHECK_EQ(m[0].core, 32);
  CHECK_EQ(seen, 3);
  CHECK_EQ(held + m[0].pinned, v.pinned_frames);
  mx_printf("k69: the processes not in front hold %u frames (%u KiB) resident, the kernel %u in the pool\n",
            back, back * 32, m[0].pinned);
  int st = -1;
  CHECK_EQ(mx_proc_wait(id, &st, 0), 1);
  CHECK_EQ(st, 5);
  return t_done("k69");
}
