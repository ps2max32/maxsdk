/* VM step 11: a program linked into VM (ee/ld/maxsdk-vm.ld): its own code and
   data at MX_VM_BASE, maxsdk in core, started by mxload. Run as
     run.sh mxload.elf 30 host:vm11_vmlink.elf
   (PCSX2, with the EE interpreter: host:mxload.txt holding
   "host:vm11_vmlink.elf"). 16 frames (512 KiB) and a swap file, so the
   program's own pages, code too, go out and come back. Under a minute; at
   the end the screen is bright green with 900D0000 in slot 0, or bright
   red with BAD000nn.

   Slot 0 and the colour: the step. Slot 1: the round (step 4), slot 2: a
   value checked. Slots 4-6: swap ins, swap outs, page faults. Heartbeat in
   slots 30 and 31.
     1 started by mxload, VM on; main, .data and .bss in VM, a MX_CORE_FN
       function and a MX_CORE_DATA variable in core
     2 .data, .rodata and a 256 KiB .bss as in the file
     3 core calls VM: a core function calling a VM function pointer
     4 code pages: four functions, each on a page of its own at the same
       I-cache index, called in 6 rounds with 20 heap pages written in
       between (more than the frames: theirs are reused for each other's
       code). About 300 swap transfers, 20 s on the PS2
     5 a thread whose function is in VM (its stack in core) */

#include "t.h"
#include <mx/loader.h>
#include <mx/thread.h>
#include <mx/vm.h>

#define PAGE MX_VM_PAGE
#define HEAP_PAGES 20

const struct mx_vm_config mx_vm_config = {.swap = "host:vm11swap.bin", .frames = 16 * PAGE};

static u32 data_word = 0x12345678u;
static const u32 table[64] = {
    1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987, 1597,
};
static u8 big[256 * 1024];
static u32 core_word MX_CORE_DATA = 0xC0DE0001u;
static u8 tstack[16384] MX_CORE_DATA MX_ALIGNED(16);

#define FN(n, v)                                                                                 \
  __attribute__((noinline, aligned(32768))) static u32 f##n(u32 x) { return x * 3 + (v); }
FN(0, 0x1000)
FN(1, 0x2000)
FN(2, 0x3000)
FN(3, 0x4000)
static u32 (*const fns[4])(u32) = {f0, f1, f2, f3};

static u32 in_vm(const void *p) {
  return (uintptr_t)p >= MX_VM_BASE && (uintptr_t)p < MX_VM_LIMIT;
}

static void stats(void) {
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_dbg_word(4, s.swap_ins);
  mx_dbg_word(5, s.swap_outs);
  mx_dbg_word(6, s.faults);
}

static void step(u32 n, const char *what) {
  stats();
  mx_dbg_step(n);
  mx_printf("vm11: %u %s\n", n, what);
}

MX_CORE_FN __attribute__((noinline)) static u32 core_call(u32 (*fn)(u32), u32 x) {
  return fn(x) + core_word;
}

static volatile u32 thread_result;
static void thread_fn(void *arg) { thread_result = f1((u32)(uintptr_t)arg) + f2(1); }

int main(int argc, char **argv) {
  (void)argc, (void)argv;
  mx_dbg_screen();
  mx_dbg_heartbeat(1);

  step(1, "linked into VM");
  CHECK_EQ(mx_loader_mxload(), 1);
  CHECK_EQ(mx_vm_active(), 1);
  CHECK(in_vm((const void *)main));
  CHECK(in_vm(&data_word));
  CHECK(in_vm(big));
  CHECK(in_vm(table));
  CHECK(!in_vm((const void *)core_call));
  CHECK(!in_vm(&core_word));
  CHECK(!in_vm(tstack));
  mx_printf("vm11: main %p, data %p, bss %p, core_call %p\n", (void *)main, (void *)&data_word,
            (void *)big, (void *)core_call);

  step(2, "data, rodata, bss");
  mx_dbg_word(2, data_word);
  CHECK_EQ(data_word, 0x12345678u);
  u32 sum = 0;
  for (u32 i = 0; i < 64; i++)
    sum += table[i];
  CHECK_EQ(sum, 4179u);
  u32 nonzero = 0;
  for (u32 i = 0; i < sizeof big; i++)
    nonzero += big[i] != 0;
  CHECK_EQ(nonzero, 0);
  data_word++;

  step(3, "core calls VM");
  u32 r = core_call(f0, 5);
  mx_dbg_word(2, r);
  CHECK_EQ(r, 15 + 0x1000 + 0xC0DE0001u);

  step(4, "code pages");
  u8 *heap = mx_sbrk(HEAP_PAGES * PAGE);
  CHECK(heap != (void *)-1);
  CHECK(in_vm(heap));
  CHECK((uintptr_t)heap >= (uintptr_t)(big + sizeof big));
  u32 bad = 0;
  for (u32 n = 0; n < 6 && heap != (void *)-1; n++) {
    mx_dbg_word(1, n);
    for (u32 k = 0; k < HEAP_PAGES; k++)
      ((volatile u32 *)(heap + k * PAGE))[n] = n * HEAP_PAGES + k;
    for (u32 j = 0; j < 4; j++) {
      u32 i = (j + n) & 3;
      u32 v = fns[i](n);
      if (v != n * 3 + 0x1000 * (i + 1)) {
        mx_printf("vm11: round %u: f%u gave %08x\n", n, i, v);
        bad++;
      }
    }
    stats();
  }
  CHECK_EQ(bad, 0);
  CHECK_EQ(data_word, 0x12345679u);
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  CHECK(s.swap_ins > 0);

  step(5, "a thread in VM");
  mx_thread_set_prio(mx_thread_self(), 64);
  int t = mx_thread_start(thread_fn, (void *)7, tstack, sizeof tstack, 64);
  CHECK(t >= 0);
  if (t >= 0)
    mx_thread_join(t);
  mx_dbg_word(2, thread_result);
  CHECK_EQ(thread_result, 21 + 0x2000 + 3 + 0x3000);

  stats();
  mx_vm_stats(&s);
  mx_printf("vm11: %u swap ins, %u outs, %u drops, %u faults, %u refills\n", s.swap_ins,
            s.swap_outs, s.drops, s.faults, s.refills);
  mx_dbg_heartbeat(0);
  return t_done("vm11");
}
