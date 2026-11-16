/* The BIOS's memory after the kernel's first start (mx OS plan step 4a, part
   2). A kernel built with KBIOS_GONE fills it (0x1000-0x87FFF, all but the
   vectors' 4 KiB) with break instructions when it stops using the BIOS; this
   program reads it (mx_phys_read) and says what is no longer the fill: runs
   of words, with their first values. Nothing may write there: a run found is
   something that still takes that memory for the BIOS's. Under a kernel that
   keeps the BIOS (the memory is not filled) it only says so.
     k62_bios.elf     (status 0 expected) */
#include "t.h"
#include <mx/cpu.h>
#include <mx/loader.h>
#include <mx/proc.h>

MX_CORE_HEAP(256 << 10);

#define LO 0x00001000u
#define HI 0x00088000u
#define FILL 0x0000000Du

static u32 buf[4096 / 4] MX_DMA_LINES;

int main(void) {
  CHECK_EQ(mx_phys_read(LO, buf, sizeof buf), 0);
  if (buf[0] != FILL && buf[1] != FILL && buf[512] != FILL) {
    mx_printf("k62: the BIOS's memory is not filled (%08x %08x): this kernel keeps the BIOS; nothing checked\n",
              buf[0], buf[1]);
    return t_done("k62");
  }
  u32 runs = 0, words = 0, run_at = 0, run_n = 0, first[4];
  for (u32 a = LO; a < HI; a += sizeof buf) {
    CHECK_EQ(mx_phys_read(a, buf, sizeof buf), 0);
    for (u32 i = 0; i <= sizeof buf / 4; i++) {
      int bad = i < sizeof buf / 4 && buf[i] != FILL;
      u32 at = a + 4 * i;
      if (bad) {
        if (!run_n || run_at + 4 * run_n != at) {
          if (run_n && runs++ < 32)
            mx_printf("k62: %05x-%05x changed (%u words): %08x %08x %08x %08x\n", run_at,
                      run_at + 4 * run_n - 1, run_n, first[0], first[1], first[2], first[3]);
          run_at = at, run_n = 0;
          first[0] = first[1] = first[2] = first[3] = 0;
        }
        if (run_n < 4)
          first[run_n] = buf[i];
        run_n++, words++;
      }
    }
  }
  if (run_n && runs++ < 32)
    mx_printf("k62: %05x-%05x changed (%u words): %08x %08x %08x %08x\n", run_at, run_at + 4 * run_n - 1,
              run_n, first[0], first[1], first[2], first[3]);
  mx_printf("k62: %u KiB filled, %u words in %u runs changed\n", (HI - LO) >> 10, words, runs);
  CHECK_EQ(words, 0);
  return t_done("k62");
}
