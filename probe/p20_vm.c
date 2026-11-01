/* Probe: what virtual memory costs (<mx/vm.h>, docs/vm.md), in EE cycles:
   - a TLB refill of a resident page, a TLB modified exception (the first
     store to a clean page), a first touch (zero fill)
   - a page fault that reads the swap file (host:), with and without a
     write back of the page it evicts
   - reading 4 MiB of VM that stays resident, against core memory
   - VIF1 DMA of 64 KiB of pinned VM in pieces (REF chain) against one
     piece (normal mode) and core memory
   On PCSX2 run it with PCSX2_EE=interp (and the numbers are not timings). */

#include <mx/mx.h>
#include <mx/vm.h>

#define PAGE MX_VM_PAGE
#define MB (1024u * 1024u)

static volatile u32 sink;

static u32 *vmw(u32 off) { return (u32 *)((u8 *)mx_vm_base() + off); }

/* Read one word of each page in [a, b) (pages). */
static void touch(u32 a, u32 b) {
  for (u32 pg = a; pg < b; pg++)
    sink += *vmw(pg * PAGE);
}

static u32 per(u32 cycles, u32 n) { return n ? cycles / n : 0; }

static u8 core_buf[64 * 1024] MX_ALIGNED(64);

static u32 time_send(const void *p, u32 qwc) {
  u32 t0 = mx_count();
  mx_dma_send(MX_CH_VIF1, p, qwc);
  mx_dma_wait(MX_CH_VIF1);
  return mx_count() - t0;
}

int main(void) {
  static const struct mx_vm_config cfg = {.swap = "host:p20.swap", .frames = 8 * MB};
  if (mx_vm_init(&cfg) < 0) {
    MX_TEST_END(1);
    return 1;
  }
  struct mx_vm_stats s0, s1;

  /* first touch: 64 pages, zero filled in the exception */
  mx_vm_stats(&s0);
  u32 t0 = mx_count();
  touch(0, 64);
  u32 t = mx_count() - t0;
  mx_vm_stats(&s1);
  mx_printf("p20_vm: first touch (zero fill): %u cycles a page (%u zero fills)\n",
            per(t, s1.zero_fills - s0.zero_fills), s1.zero_fills - s0.zero_fills);

  /* refills: 64 resident pages in turn, more than the 17 TLB entries */
  touch(0, 64);
  mx_vm_stats(&s0);
  t0 = mx_count();
  for (int r = 0; r < 16; r++)
    touch(0, 64);
  t = mx_count() - t0;
  mx_vm_stats(&s1);
  u32 nref = s1.refills - s0.refills;
  mx_printf("p20_vm: TLB refill of a resident page: %u cycles (%u refills, 1024 reads)\n",
            per(t - 1024 * 2, nref), nref);

  /* modified: the first store to each of 64 pages loaded clean... in this
     run they are zero pages read only, so mapped clean */
  t0 = mx_count();
  for (u32 pg = 0; pg < 64; pg++)
    *vmw(pg * PAGE + 4) = pg;
  t = mx_count() - t0;
  struct mx_vm_stats s2;
  mx_vm_stats(&s2);
  mx_printf("p20_vm: first store to a clean page: %u cycles (dirty tracking %u)\n", per(t, 64),
            s2.dirty_tracking);

  /* swap: 3 * frames of pages written, then read back in order: each fault
     reads a page and writes back the dirty one it evicts */
  u32 nf = s2.frames, span = 3 * nf;
  for (u32 pg = 0; pg < span; pg++)
    *vmw(pg * PAGE) = pg;
  mx_vm_stats(&s0);
  t0 = mx_count();
  for (u32 pg = 0; pg < span; pg++)
    sink += *vmw(pg * PAGE);
  t = mx_count() - t0;
  mx_vm_stats(&s1);
  u32 nfault = s1.faults - s0.faults;
  mx_printf("p20_vm: page fault, swap in + write back: %u us (%u faults, %u in, %u out)\n",
            per(t, nfault) / (MX_EE_HZ / 1000000), nfault, s1.swap_ins - s0.swap_ins,
            s1.swap_outs - s0.swap_outs);
  /* again, only read: the evicted pages are clean, dropped */
  mx_vm_stats(&s0);
  t0 = mx_count();
  for (u32 pg = 0; pg < span; pg++)
    sink += *vmw(pg * PAGE);
  t = mx_count() - t0;
  mx_vm_stats(&s1);
  nfault = s1.faults - s0.faults;
  mx_printf("p20_vm: page fault, swap in, clean eviction: %u us (%u faults, %u out, %u drops)\n",
            per(t, nfault) / (MX_EE_HZ / 1000000), nfault, s1.swap_outs - s0.swap_outs,
            s1.drops - s0.drops);

  /* reading 4 MiB, resident (the frames hold 8 MiB): VM and core */
  u32 n4 = 4 * MB / 4;
  for (u32 i = 0; i < n4; i += 16)
    sink += *vmw(i * 4);
  t0 = mx_count();
  u32 sum = 0;
  for (u32 i = 0; i < n4; i++)
    sum += *vmw(i * 4);
  u32 tv = mx_count() - t0;
  const u32 *core = (const u32 *)0x00100000; /* the program and what follows: core */
  t0 = mx_count();
  for (u32 i = 0; i < n4; i++)
    sum += core[i];
  u32 tc = mx_count() - t0;
  sink += sum;
  mx_printf("p20_vm: reading 4 MiB: VM %u, core %u cycles (VM %u.%02u x)\n", tv, tc, tv / tc,
            tv % tc * 100 / tc);

  /* VIF1 DMA of 64 KiB of VIF NOPs: scattered VM (a REF chain), VM in one
     piece (normal mode), core */
  u32 at = 1000 * PAGE, bytes = 64 * 1024, qwc = bytes / 16; /* untouched so far */
  for (u32 pg = 1002; pg-- > 1000;) /* resident in reverse order */
    *vmw(pg * PAGE) = 0;
  for (u32 i = 0; i < bytes / 4; i++)
    vmw(at)[i] = 0;
  mx_vm_pin(vmw(at), bytes, 0);
  struct mx_vm_piece pc[8];
  u32 k = mx_vm_pieces(vmw(at), bytes, pc, 8);
  mx_dcache_writeback(vmw(at), bytes);
  u32 tsg = time_send(vmw(at), qwc);
  mx_vm_unpin(vmw(at), bytes);
  mx_vm_pin(vmw(at), bytes, MX_VM_PIN_DMA);
  mx_dcache_writeback(vmw(at), bytes);
  u32 tone = time_send(vmw(at), qwc);
  mx_vm_unpin(vmw(at), bytes);
  mx_dcache_writeback(core_buf, bytes);
  u32 tcore = time_send(core_buf, qwc);
  mx_printf("p20_vm: VIF1 64 KiB: VM in %u pieces %u, VM in one %u, core %u cycles\n", k, tsg,
            tone, tcore);

  mx_printf("p20_vm: ok\n");
  MX_TEST_END(0);
  return 0;
}
