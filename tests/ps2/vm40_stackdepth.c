/* How much stack a page fault takes (mx OS plan, phase 4): a thread with a
   painted stack in core touches VM, first pages that are resident (no
   fault), then new pages (the zero fill), then more pages than there are
   frames, written and read back (swap outs and ins in its thread, the VM's
   slow path). After each the stack's lowest overwritten word: the depth
   the thread's own code and the fault path need. Started by mxlink:
     vm40_stackdepth.elf     (status 0 expected) */

#include "t.h"
#include <mx/thread.h>
#include <mx/vm.h>

#define PAGE MX_VM_PAGE
#define NP 48
#define STACK (64u << 10)
#define PAINT 0x5AC0FFEEu

const struct mx_vm_config mx_vm_config = {.swap = "host:vm40swap.bin", .frames = 16 * PAGE};

static u8 pages[NP][PAGE];
static u32 stack[STACK / 4] MX_ALIGNED(16) MX_CORE_DATA;
static volatile u32 phase_depth[4];
static volatile u32 bad;

/* the bottom 256 bytes are mx_thread_start's (its start block): not painted
   or counted; from there up to just below the caller's frame */
#define LOW_WORDS 64
static void paint(void) {
  u32 sp;
  __asm__ volatile("move %0, $sp" : "=r"(sp));
  u32 top = (sp - 64 - (u32)(uintptr_t)stack) / 4;
  if (top > STACK / 4)
    top = 0; /* not on this stack (the main thread's first paint) */
  if (!top)
    top = STACK / 4 - 1024;
  for (u32 i = LOW_WORDS; i < top; i++)
    stack[i] = PAINT;
}
/* the deepest use: bytes from the top down to the lowest overwritten word */
static u32 depth(void) {
  u32 i = LOW_WORDS;
  while (i < STACK / 4 && stack[i] == PAINT)
    i++;
  return STACK - i * 4;
}

static void touch(u32 from, u32 n, u32 g, int check) {
  for (u32 k = from; k < from + n; k++)
    for (u32 i = 0; i < PAGE; i += 64) {
      u8 v = (u8)(k * 7 + i / 64 + g);
      if (check)
        bad += pages[k][i] != v;
      else
        pages[k][i] = v;
    }
}

static void body(void *arg) {
  (void)arg;
  /* 0: resident pages (made resident by the main thread) */
  touch(0, 4, 0, 1);
  phase_depth[0] = depth();
  paint();
  /* 1: new pages, zero filled while frames are free */
  touch(4, 8, 0, 0);
  phase_depth[1] = depth();
  paint();
  /* 2: more pages than frames: swap outs */
  touch(12, NP - 12, 0, 0);
  phase_depth[2] = depth();
  paint();
  /* 3: all read back: swap ins */
  touch(4, NP - 4, 0, 1);
  phase_depth[3] = depth();
}

int main(void) {
  CHECK(mx_vm_active());
  touch(0, 4, 0, 0);
  paint();
  int id = mx_thread_start(body, 0, stack, sizeof stack, mx_thread_prio(mx_thread_self()));
  CHECK(id > 0);
  if (id > 0)
    mx_thread_join(id);
  struct mx_vm_stats st;
  mx_vm_stats(&st);
  mx_printf("vm40: stack depth: resident %u, zero fill %u, swap outs %u, swap ins %u bytes\n",
            phase_depth[0], phase_depth[1], phase_depth[2], phase_depth[3]);
  mx_printf("vm40: %u swap outs, %u ins, %u faults; %u bytes wrong\n", st.swap_outs, st.swap_ins,
            st.faults, bad);
  CHECK_EQ(bad, 0);
  CHECK(st.swap_ins > 0);
  return t_done("vm40");
}
