/* VM step 5: TLB entries written by hand (tlbwi from user mode, as the VM
   does), no TLB exceptions: an access only ever hits an entry. About 10 s;
   at the end the screen is bright green with 900D0000 in slot 0, or bright
   red with BAD000nn.

   Slot 0 and the colour: the step. Slot 1: the entry being checked or
   written, slot 2: words wrong in the last check. Slots 8-15: entries
   31..38 EntryHi as the BIOS left them, 16-23: 39..46. Heartbeat in
   slots 30 and 31.
     1 read the BIOS's entries (printed): 13-21 main memory cached,
       31-38 UCAB, 39-47 unused (V = 0)
     2 entry 47: 0x0800_0000 (32 KiB, both 16 KiB halves) -> frame A,
       cached, writable; written there, read at A
     3 entry 47 -> frame B: 0x0800_0000 now reads B
     4 entry 47 -> frame A uncached (C = 2): written there, read uncached
     5 entries 39..47: 9 pages at 0x0800_0000.. -> 9 frames, each checked
     6 3 s of timer interrupts, thread switches and host: I/O: the entries
       are still there and still right
     7 the BIOS's entries 39..47 back */

#include "t.h"
#include <mx/sif.h>

#define PAGE 32768u
#define HALF 16384u
#define PM16K 0x6000u
#define VA 0x08000000u
#define LO_G 1u
#define LO_V 2u
#define LO_D 4u
#define C_CACHED (3u << 3)
#define C_UNCACHED (2u << 3)

static u8 frames[10 * PAGE] __attribute__((aligned(PAGE)));

static void tlb_write(u32 i, u32 pm, u32 hi, u32 lo0, u32 lo1) {
  int was = mx_di();
  __asm__ volatile("mtc0 %0, $0\n\tmtc0 %1, $5\n\tmtc0 %2, $10\n\tmtc0 %3, $2\n\tmtc0 %4, $3\n\t"
                   "sync.p\n\ttlbwi\n\tsync.p"
                   :
                   : "r"(i), "r"(pm), "r"(hi), "r"(lo0), "r"(lo1)
                   : "memory");
  mx_irq_restore(was);
}

static void tlb_read(u32 i, u32 *pm, u32 *hi, u32 *lo0, u32 *lo1) {
  int was = mx_di();
  __asm__ volatile("mtc0 %4, $0\n\tsync.p\n\ttlbr\n\tsync.p\n\t"
                   "mfc0 %0, $5\n\tmfc0 %1, $10\n\tmfc0 %2, $2\n\tmfc0 %3, $3"
                   : "=r"(*pm), "=r"(*hi), "=r"(*lo0), "=r"(*lo1)
                   : "r"(i));
  mx_irq_restore(was);
}

static struct {
  u32 pm, hi, lo0, lo1;
} bios[48];

static u32 lo_of(const void *p, u32 c) {
  return mx_paddr_of(p).v >> 12 << 6 | c | LO_D | LO_V | LO_G;
}

/* entry i: VA + k * PAGE -> frame f */
static void map(u32 i, u32 k, u32 f, u32 c) {
  u32 lo = lo_of(frames + f * PAGE, c);
  tlb_write(i, PM16K, VA + k * PAGE, lo, lo + (HALF >> 12 << 6));
}

static u32 word(u32 seed, u32 i) { return i * 2654435761u ^ seed * 40503u; }

static void put(volatile u32 *p, u32 seed) {
  for (u32 i = 0; i < PAGE / 4; i++)
    p[i] = word(seed, i);
}

static u32 wrong(const volatile u32 *p, u32 seed) {
  u32 w = 0;
  for (u32 i = 0; i < PAGE / 4; i++)
    w += p[i] != word(seed, i);
  mx_dbg_word(2, w);
  return w;
}

static void step(u32 n, const char *what) {
  mx_dbg_step(n);
  mx_printf("vm5: %u %s\n", n, what);
}

static u8 tstack[16384] MX_ALIGNED(16);
static volatile u32 other_runs, stop;
static void other(void *arg) {
  (void)arg;
  while (!stop) {
    other_runs++;
    mx_thread_yield();
  }
}

static volatile u32 ticks;
static void tick(void *arg) {
  (void)arg;
  ticks++;
}

int main(void) {
  mx_dbg_screen();
  mx_dbg_heartbeat(1);

  step(1, "the BIOS's entries");
  for (u32 i = 0; i < 48; i++) {
    tlb_read(i, &bios[i].pm, &bios[i].hi, &bios[i].lo0, &bios[i].lo1);
    mx_printf("vm5: tlb %2u: pm %08x hi %08x lo %08x %08x\n", i, bios[i].pm, bios[i].hi,
              bios[i].lo0, bios[i].lo1);
    if (i >= 31 && i < 47)
      mx_dbg_word(8 + (i - 31), bios[i].hi);
  }
  for (u32 i = 39; i < 48; i++)
    CHECK_EQ((bios[i].lo0 | bios[i].lo1) & LO_V, 0);

  step(2, "0x08000000 -> frame A");
  mx_dbg_word(1, 47);
  map(47, 0, 0, C_CACHED);
  put((volatile u32 *)VA, 1);
  CHECK_EQ(wrong((volatile u32 *)frames, 1), 0);
  CHECK_EQ(wrong((volatile u32 *)VA, 1), 0);

  step(3, "-> frame B");
  put((volatile u32 *)(frames + PAGE), 2);
  map(47, 0, 1, C_CACHED);
  CHECK_EQ(wrong((volatile u32 *)VA, 2), 0);

  step(4, "-> frame A uncached");
  mx_dcache_flush_all();
  map(47, 0, 0, C_UNCACHED);
  put((volatile u32 *)VA, 3);
  CHECK_EQ(wrong((volatile u32 *)MX_UNCACHED(frames), 3), 0);

  step(5, "9 entries");
  /* entry 47 lets go of 0x0800_0000 first: two entries matching one
     address are undefined (MIPS: a TLB shutdown) */
  tlb_write(47, bios[47].pm, bios[47].hi, bios[47].lo0, bios[47].lo1);
  for (u32 k = 0; k < 9; k++) {
    mx_dbg_word(1, 39 + k);
    map(39 + k, k, k + 1, C_CACHED);
  }
  for (u32 k = 0; k < 9; k++)
    put((volatile u32 *)(VA + k * PAGE), 10 + k);
  for (u32 k = 0; k < 9; k++) {
    mx_dbg_word(1, 39 + k);
    CHECK_EQ(wrong((volatile u32 *)(frames + (k + 1) * PAGE), 10 + k), 0);
  }

  step(6, "3 s of interrupts, threads and I/O");
  mx_thread_set_prio(mx_thread_self(), 64);
  int t = mx_thread_start(other, 0, tstack, sizeof tstack, 64);
  CHECK(t >= 0);
  mx_timer_periodic(1, 57600, tick, 0);
  int fd = mx_fio_open("host:vm5.bin", MX_FIO_RDONLY | MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  u32 v0 = mx_gs.vsync, io = 0;
  while (mx_gs.vsync - v0 < 3 * mx_gs.hz) {
    if (fd >= 0) {
      mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
      CHECK_EQ(mx_fio_write(fd, frames + PAGE, PAGE), 0);
      io++;
    }
    mx_thread_yield();
  }
  mx_timer_periodic(1, 0, 0, 0);
  stop = 1;
  mx_thread_join(t);
  if (fd >= 0)
    mx_fio_close(fd);
  mx_printf("vm5: %u ticks, %u switches to the other thread, %u writes\n", ticks, other_runs, io);
  CHECK(ticks > 10);
  CHECK(other_runs > 10);
  for (u32 k = 0; k < 9; k++) {
    u32 pm, hi, lo0, lo1;
    tlb_read(39 + k, &pm, &hi, &lo0, &lo1);
    u32 lo = lo_of(frames + (k + 1) * PAGE, C_CACHED);
    CHECK_EQ(pm, PM16K);
    CHECK_EQ(hi & ~0xFFu, VA + k * PAGE);
    CHECK_EQ(lo0, lo);
    CHECK_EQ(lo1, lo + (HALF >> 12 << 6));
    mx_dbg_word(1, 39 + k);
    CHECK_EQ(wrong((volatile u32 *)(VA + k * PAGE), 10 + k), 0);
  }

  step(7, "the BIOS's entries back");
  for (u32 i = 39; i < 48; i++)
    tlb_write(i, bios[i].pm, bios[i].hi, bios[i].lo0, bios[i].lo1);
  for (u32 i = 39; i < 48; i++) {
    u32 pm, hi, lo0, lo1;
    tlb_read(i, &pm, &hi, &lo0, &lo1);
    CHECK_EQ(hi, bios[i].hi);
    CHECK_EQ(lo0 | lo1, bios[i].lo0 | bios[i].lo1);
  }
  mx_dbg_heartbeat(0);
  return t_done("vm5");
}
