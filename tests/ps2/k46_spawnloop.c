/* Spawn after spawn (the k42 hang on the PS2: now and then the kernel did not
   start the other after closing this one's pads). This program starts
   vm34_child (exit7) in front again and again and waits for each; a thread
   of its own beside says every second how far it is, so a hang shows whether
   this program still runs. Started by mxlink:
     k46_spawnloop.elf host:vm34_child.elf [times]     (status 0 expected) */
#include "t.h"
#include <mx/loader.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/thread.h>
#include <mx/hw/regs.h>

MX_CORE_HEAP(256 << 10);

static volatile int round_now, stage;
static u8 stack[8192] MX_CORE_BSS __attribute__((aligned(16)));

static void watch(void *arg) {
  (void)arg;
  for (u32 s = 1;; s++) {
    mx_sleep_us(1000000);
    /* SIF0 (the IOP's answers): still receiving, or stopped */
    mx_printf("k46: %us, round %d, stage %d; SIF0 CHCR %08x QWC %x MADR %08x TADR %08x "
              "D_STAT %08x SMFLG %08x\n", s, round_now, stage, MX_R32(MX_DMA_SIF0_CHCR),
              MX_R32(MX_DMA_SIF0_QWC), MX_R32(MX_DMA_SIF0_MADR), MX_R32(MX_DMA_SIF0_TADR),
              MX_R32(MX_D_STAT), MX_R32(0x1000F230u));
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    mx_printf("k46: usage: k46_spawnloop.elf <vm34_child.elf> [times]\n");
    return t_done("k46");
  }
  int times = 0;
  for (const char *c = argc > 2 ? argv[2] : "30"; *c >= '0' && *c <= '9'; c++)
    times = times * 10 + (*c - '0');
  CHECK(mx_thread_start(watch, 0, stack, sizeof stack, mx_thread_prio(mx_thread_self()) - 1) >= 0);
  static char args[256];
  u32 n = (u32)strlen(argv[1]) + 1;
  memcpy(args, argv[1], n);
  memcpy(args + n, "exit7", 6);
  n += 6;
  for (int i = 1; i <= times; i++) {
    round_now = i, stage = 1;
    int id = mx_proc_spawn(2, args, n, 0);
    stage = 2;
    CHECK(id > 0);
    int st = -1;
    CHECK_EQ(mx_proc_wait(id, &st, 0), 1);
    stage = 3;
    CHECK_EQ(st, 7);
    if (st != 7 || id <= 0)
      break;
  }
  mx_printf("k46: %d rounds\n", round_now);
  return t_done("k46");
}
