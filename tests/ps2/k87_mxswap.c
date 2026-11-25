/* The swap file through mxswap.irx (kernel version 49, swap.iop): the
   program writes 4 MiB of its VM with data that compresses about as a
   game's does, mx_vm_clean puts those pages into mxswap's queue, and the
   program then watches mxswap's writer (MXSWAP_STAT, read only) until the
   queue is written or 20 s went by, printing its progress; then the pages
   are checked. Needs MXOS's swap file through the IOP. With an argument,
   the writer's block in KiB first (MXSWAP_BLOCK), to compare; with
   "stat" only mxswap's figures (the gets from the file timed on the IOP).
     k87_mxswap.elf [block KiB | stat]     (status 0 expected) */
#include "t.h"
#include <mx/kernel.h>
#include <mx/sif.h>
#include <mx/loader.h>
#include <mx/mem.h>
#include <mx/vm.h>
#include "../../common/include/mxswap.h"

MX_CORE_HEAP(256 << 10);

#define PAGE MX_VM_PAGE
#define BIG (4u << 20)
static u32 big[BIG / 4]; /* VM: the program's .bss */
static struct mx_sif_client cl;
static u32 sbuf[4] MX_IRQ_DATA MX_ALIGNED(64);
static u32 rbuf[16] MX_IRQ_DATA MX_ALIGNED(64);

/* about half of each page compresses: a counter in every other word */
static u32 E(u32 i) { return (i & 1) ? i * 2654435761u : i >> 3; }

static int stat(struct mxswap_stat *s) {
  int r = mx_sif_call(&cl, MXSWAP_STAT, MX_UNCACHED(sbuf), 16, MX_UNCACHED(rbuf), sizeof rbuf);
  const volatile u32 *w = MX_UNCACHED(rbuf);
  u32 *o = (u32 *)s;
  for (u32 i = 0; i < sizeof *s / 4; i++)
    o[i] = w[i];
  return r;
}

int main(int argc, char **argv) {
  CHECK(mx__kernel && mx__kernel->version >= 49);
  if (mx_sif_bind(&cl, MXSWAP_SID) < 0 || !cl.server) {
    mx_printf("k87: no mxswap on the IOP\n");
    CHECK(0);
    return t_done("k87");
  }
  if (argc > 1 && argv[1][0] == 's') {
    /* "stat": mxswap's figures so far (they add up over programs), the gets
       from the file timed on the IOP */
    struct mxswap_stat s;
    CHECK(stat(&s) >= 0);
    u32 nf = s.gets - s.from_queue;
    mx_printf("k87: %u puts, %u writes, %u errors; %u gets, %u from the queue, %u from the file\n", s.puts,
              s.writes, s.write_errors, s.gets, s.from_queue, nf);
    mx_printf("k87: %u puts told full; wait %u ms, seek %u ms, read %u ms in all\n", s.full,
              s.get_wait_us / 1000, s.get_seek_us / 1000, s.get_read_us / 1000);
    if (nf)
      mx_printf("k87: a get from the file on the IOP: wait %u us, seek %u us, read %u us\n",
                s.get_wait_us / nf, s.get_seek_us / nf, s.get_read_us / nf);
    return t_done("k87");
  }
  if (argc > 1) {
    u32 kib = 0;
    for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
      kib = kib * 10 + (u32)(*p - '0');
    volatile u32 *a = MX_UNCACHED(sbuf);
    a[0] = kib << 10, a[1] = a[2] = a[3] = 0;
    CHECK(mx_sif_call(&cl, MXSWAP_BLOCK, (void *)a, 16, MX_UNCACHED(rbuf), 16) >= 0);
    mx_printf("k87: the writer's block: %u KiB\n", ((volatile u32 *)MX_UNCACHED(rbuf))[0] >> 10);
  }
  struct mxswap_stat s;
  CHECK(stat(&s) >= 0);
  mx_printf("k87: before: %u queued (%u of %u bytes), %u puts, %u writes, %u errors\n", s.queued,
            s.queue_bytes, s.pool_bytes, s.puts, s.writes, s.write_errors);
  u32 puts0 = s.puts, writes0 = s.writes;
  for (u32 i = 0; i < BIG / 4; i++)
    big[i] = E(i);
  /* the VM's own time: mx_count wraps after 14.5 s */
  struct mx_vm_stats v0, v1;
  mx_vm_stats(&v0);
  int n = mx_vm_clean();
  mx_vm_stats(&v1);
  u32 ms = (v1.us_swap_out - v0.us_swap_out) / 1000, t0 = mx_count();
  mx_printf("k87: mx_vm_clean: %d pages in %u ms\n", n, ms);
  CHECK(n >= (int)(BIG / PAGE));
  /* the writer at work */
  u32 last = ~0u;
  for (int k = 0; k < 80; k++) {
    CHECK(stat(&s) >= 0);
    if (s.writes != last)
      mx_printf("k87: %u ms: %u queued, %u written, %u errors, %u puts told full, %u bytes\n",
                ms + (mx_count() - t0) / (MX_EE_HZ / 1000u), s.queued, s.writes - writes0, s.write_errors,
                s.full, s.queue_bytes);
    last = s.writes;
    if (!s.queued)
      break;
    mx_sleep_us(250000);
  }
  mx_printf("k87: %u puts, %u written, %u errors, %u still queued; all written after %u ms\n", s.puts - puts0,
            s.writes - writes0, s.write_errors, s.queued, ms + (mx_count() - t0) / (MX_EE_HZ / 1000u));
  CHECK_EQ(s.queued, 0);
  CHECK_EQ(s.write_errors, 0);
  u32 bad = 0;
  for (u32 i = 0; i < BIG / 4; i++)
    bad += big[i] != E(i);
  CHECK_EQ(bad, 0);
  return t_done("k87");
}
