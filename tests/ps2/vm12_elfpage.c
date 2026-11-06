/* VM step 12: a program linked into VM whose pages come from its ELF when
   first touched (ee/src/vm/image.c, the pager's file_in), not at the start.
   Run as
     run.sh mxload.elf 60 host:vm12_elfpage.elf
   (PCSX2, with the EE interpreter: host:mxload.txt holding
   "host:vm12_elfpage.elf"). 16 frames (512 KiB) and a swap file. Under a
   minute; at the end the screen is bright green with 900D0000 in slot 0,
   or bright red with BAD000nn.

   Slot 0 and the colour: the step. Slot 1: the round, slot 2: a value
   checked. Slots 4-7: pages read from the ELF, swap ins, swap outs,
   drops. Heartbeat in slots 30 and 31.
     1 at main few pages read: not the 128 KiB table, not the 64 KiB array
     2 the table (.rodata, 4 pages) and the array (.data, 2 pages) right,
       read from the ELF
     3 20 heap pages written (more than the frames), then the table and the
       array again: read from the ELF again (the PS2 drops them unwritten;
       PCSX2, which ignores D, writes every page out and reads it back)
     4 half the array written, 20 heap pages again, the array again: the
       written page from the swap file, the other one from the ELF (PS2) */

#include "t.h"
#include <mx/loader.h>
#include <mx/vm.h>

#define PAGE MX_VM_PAGE
#define HEAP_PAGES 20

const struct mx_vm_config mx_vm_config = {.swap = "host:vm12swap.bin", .frames = 16 * PAGE};

/* word i of the tables: E(i), filled in by the compiler */
#define E(i) ((u32)(i) * 2654435761u ^ 0x5a5a5a5au)
#define X8(b) E(b), E(b + 1), E(b + 2), E(b + 3), E(b + 4), E(b + 5), E(b + 6), E(b + 7)
#define X64(b) X8(b), X8(b + 8), X8(b + 16), X8(b + 24), X8(b + 32), X8(b + 40), X8(b + 48), X8(b + 56)
#define X512(b)                                                                                    \
  X64(b), X64(b + 64), X64(b + 128), X64(b + 192), X64(b + 256), X64(b + 320), X64(b + 384),       \
      X64(b + 448)
#define X4K(b)                                                                                     \
  X512(b), X512(b + 512), X512(b + 1024), X512(b + 1536), X512(b + 2048), X512(b + 2560),          \
      X512(b + 3072), X512(b + 3584)
#define X32K(b)                                                                                    \
  X4K(b), X4K(b + 4096), X4K(b + 8192), X4K(b + 12288), X4K(b + 16384), X4K(b + 20480),            \
      X4K(b + 24576), X4K(b + 28672)

#define NT 32768 /* 128 KiB */
#define NA 16384 /* 64 KiB */
static const u32 table[NT] __attribute__((aligned(32768))) = {X32K(0)};
static u32 array[NA] __attribute__((aligned(32768))) = {X4K(0), X4K(4096), X4K(8192), X4K(12288)};

static void stats(void) {
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_dbg_word(4, s.file_ins);
  mx_dbg_word(5, s.swap_ins);
  mx_dbg_word(6, s.swap_outs);
  mx_dbg_word(7, s.drops);
}

static void step(u32 n, const char *what) {
  stats();
  mx_dbg_step(n);
  mx_printf("vm12: %u %s\n", n, what);
}

static u32 wrong(const u32 *p, u32 n, u32 from, u32 to, u32 (*want)(u32)) {
  u32 w = 0;
  for (u32 i = from; i < to && i < n; i++)
    w += p[i] != want(i);
  return w;
}

static u32 orig(u32 i) { return E(i); }
static u32 written(u32 i) { return i < NA / 2 ? ~E(i) : E(i); }

static void thrash(u8 *heap, u32 round) {
  for (u32 k = 0; k < HEAP_PAGES; k++) {
    mx_dbg_word(1, round << 8 | k);
    ((volatile u32 *)(heap + k * PAGE))[round] = round * HEAP_PAGES + k;
    stats();
  }
}

int main(int argc, char **argv) {
  (void)argc, (void)argv;
  mx_dbg_screen();
  mx_dbg_heartbeat(1);
  struct mx_vm_stats s0, s1, s2;

  step(1, "few pages at main");
  mx_vm_stats(&s0);
  mx_printf("vm12: %u pages read from the ELF at main\n", s0.file_ins);
  CHECK_EQ(mx_loader_mxload(), 1);
  CHECK(s0.file_ins <= 4);

  step(2, "the table and the array from the ELF");
  CHECK_EQ(wrong(table, NT, 0, NT, orig), 0);
  CHECK_EQ(wrong(array, NA, 0, NA, orig), 0);
  mx_vm_stats(&s1);
  mx_dbg_word(2, s1.file_ins - s0.file_ins);
  CHECK(s1.file_ins - s0.file_ins >= 6);

  step(3, "evicted, and read again");
  u8 *heap = mx_sbrk(HEAP_PAGES * PAGE);
  CHECK(heap != (void *)-1);
  if (heap == (void *)-1)
    return t_done("vm12");
  thrash(heap, 1);
  CHECK_EQ(wrong(table, NT, 0, NT, orig), 0);
  CHECK_EQ(wrong(array, NA, 0, NA, orig), 0);
  mx_vm_stats(&s2);
  mx_printf("vm12: %u from the ELF, %u swap ins, %u outs, %u drops\n", s2.file_ins, s2.swap_ins,
            s2.swap_outs, s2.drops);
  if (s2.dirty_tracking)
    CHECK(s2.file_ins - s1.file_ins >= 6);
  else
    CHECK(s2.swap_ins - s1.swap_ins >= 6);

  step(4, "half the array written");
  for (u32 i = 0; i < NA / 2; i++)
    array[i] = ~E(i);
  thrash(heap, 2);
  CHECK_EQ(wrong(array, NA, 0, NA, written), 0);
  CHECK_EQ(wrong(table, NT, 0, NT, orig), 0);

  stats();
  mx_vm_stats(&s2);
  mx_printf("vm12: %u from the ELF, %u swap ins, %u outs, %u drops, %u faults\n", s2.file_ins,
            s2.swap_ins, s2.swap_outs, s2.drops, s2.faults);
  mx_dbg_heartbeat(0);
  return t_done("vm12");
}
