/* Probe: the TLB for virtual memory (docs/vm.md).
   - Status (KSU, CU0, EXL) in main, Wired, and the BIOS's 48 entries (tlbr)
   - an entry written by hand (tlbwi) maps 0x08000000 to two 16 KiB frames
   - that a refill (no entry), an invalid entry (V = 0) and a store to a
     clean page (D = 0) reach the handlers of SetVTLBRefillHandler (codes
     1-3): the kernel's refill and common vectors both dispatch through one
     table indexed by the code (probe/kdump.c). The
     handlers write the TLB with tlbwi at seen.index only: PCSX2 keeps
     Random at 0, so a tlbwr there overwrites the scratchpad's entry 0, and
     its tlbp does not find entries (docs/vm.md)
   On PCSX2 run it with PCSX2_EE=interp: the recompiler raises no TLB misses.
   Measured: docs/vm.md.
   - an exception return started from C: a negative syscall whose handler
     (at EXL = 1) sets EPC and ERETs
   - the cost of a refill and of an invalid/modified round trip */

#include <mx/bios.h>
#include <mx/mx.h>

#define VA 0x08000000u
#define PM16K 0x6000u

static u8 frame[4][16384] MX_ALIGNED(16384);

/* What the handlers saw: a count per stub, and the last exception. */
struct seen {
  u32 n[8]; /* 0..2: codes 1..3 */
  u32 cause, epc, badvaddr, entryhi, index; /* index: where the handler writes */
  u32 lo0, lo1; /* the entry the handler writes on a miss */
};
volatile struct seen seen MX_ALIGNED(64);

MX_INLINE u32 cop0_wired(void) { u32 v; __asm__ volatile("mfc0 %0, $6" : "=r"(v)); return v; }

static void tlb_read(u32 i, u32 *pm, u32 *hi, u32 *lo0, u32 *lo1) {
  int was = mx_di();
  __asm__ volatile("mtc0 %4, $0\n\tsync.p\n\ttlbr\n\tsync.p\n\t"
                   "mfc0 %0, $5\n\tmfc0 %1, $10\n\tmfc0 %2, $2\n\tmfc0 %3, $3"
                   : "=r"(*pm), "=r"(*hi), "=r"(*lo0), "=r"(*lo1)
                   : "r"(i));
  mx_irq_restore(was);
}

static void tlb_write(u32 i, u32 pm, u32 hi, u32 lo0, u32 lo1) {
  int was = mx_di();
  __asm__ volatile("mtc0 %0, $0\n\tmtc0 %1, $5\n\tmtc0 %2, $10\n\tmtc0 %3, $2\n\tmtc0 %4, $3\n\t"
                   "sync.p\n\ttlbwi\n\tsync.p"
                   :
                   : "r"(i), "r"(pm), "r"(hi), "r"(lo0), "r"(lo1)
                   : "memory");
  mx_irq_restore(was);
}

/* Index of the entry matching hi, or -1. */
static int tlb_probe(u32 hi) {
  u32 idx;
  int was = mx_di();
  __asm__ volatile("mtc0 %1, $10\n\tsync.p\n\ttlbp\n\tsync.p\n\tmfc0 %0, $0"
                   : "=r"(idx)
                   : "r"(hi));
  mx_irq_restore(was);
  return (idx & 0x80000000u) ? -1 : (int)(idx & 63);
}

/* EntryLo of a 16 KiB frame: cached, global, valid, dirty (writable). */
static u32 lo_of(const void *f, int v, int d) {
  return (mx_paddr_of(f).v >> 12) << 6 | 3u << 3 | (u32)d << 2 | (u32)v << 1 | 1u;
}

/* Handler stubs: count, record the state, then write seen.lo0/lo1 for the
   faulting pair (EntryHi as the CPU set it) at seen.index. Only k0 and k1
   are free. */
#define STUB(name, slot)                                                                         \
  __asm__(".text\n\t.set noreorder\n\t.set noat\n\t.globl " #name "\n" #name ":\n\t"             \
          "lui $26, %hi(seen)\n\taddiu $26, $26, %lo(seen)\n\t"                                  \
          "lw $27, " #slot "*4($26)\n\taddiu $27, $27, 1\n\tsw $27, " #slot "*4($26)\n\t"        \
          "sltiu $27, $27, 100\n\tbnez $27, 3f\n\tnop\n\t"                                          \
          "la $27, storm\n\tmtc0 $27, $14\n\tsync.p\n\teret\n\tnop\n3:\n\t"                       \
          "mfc0 $27, $13\n\tsw $27, 32($26)\n\tmfc0 $27, $14\n\tsw $27, 36($26)\n\t"             \
          "mfc0 $27, $8\n\tsw $27, 40($26)\n\tmfc0 $27, $10\n\tsw $27, 44($26)\n\t"              \
          "lw $27, 48($26)\n\tmtc0 $27, $0\n\t"                                                  \
          "lw $27, 52($26)\n\tmtc0 $27, $2\n\tlw $27, 56($26)\n\tmtc0 $27, $3\n\t"               \
          "lui $27, 0x0000\n\tori $27, $27, 0x6000\n\tmtc0 $27, $5\n\tsync.p\n\t"                \
          "tlbwi\n\tsync.p\n\teret\n\tnop\n\t.set reorder\n\t.set at\n")
STUB(h_tlb1, 0);
STUB(h_tlb2, 1);
STUB(h_tlb3, 2);
void h_tlb1(void), h_tlb2(void), h_tlb3(void);

/* Return from a negative syscall (the kernel calls the handler at EXL = 1,
   on the caller's stack, below a 16-byte frame of its own): the handler
   drops that frame, sets EPC to its argument and ERETs, so the caller
   continues there. Returns 1 if it got there. */
__asm__(".text\n\t.set noreorder\n\t.globl k_resume\nk_resume:\n\t"
        "addiu $29, $29, 16\n\tmtc0 $4, $14\n\tsync.p\n\teret\n\tnop\n\t.set reorder\n");
void k_resume(void);

static int eret_from_c(void) {
  u32 ok;
  __asm__ volatile(".set noreorder\n\t"
                   "li %0, 0\n\t"
                   "la $4, 1f\n\t"
                   "li $3, -0x5B\n\t"
                   "syscall\n\t"
                   "nop\n\t"
                   "li %0, 2\n" /* not reached */
                   "1:\n\taddiu %0, %0, 1\n\t.set reorder"
                   : "=&r"(ok)
                   :
                   : "$2", "$3", "$4", "$26", "$27", "$31", "memory");
  return ok == 1 && !(mx_status() & 2);
}

static void clear_seen(void) {
  for (int i = 0; i < 8; i++)
    seen.n[i] = 0;
}

static void report(const char *what) {
  mx_printf("  %s: codes 1-3 %u %u %u, cause %08x (code %u) badv %08x entryhi %08x\n", what,
            seen.n[0], seen.n[1], seen.n[2], seen.cause,
            seen.cause >> 2 & 31, seen.badvaddr, seen.entryhi);
}

/* A handler called 100 times for one test (an exception storm): show what
   it saw and what the entry holds, then end. */
void storm(void) __attribute__((used, noreturn));
void storm(void) {
  u32 pm, hi, lo0, lo1;
  tlb_read(seen.index, &pm, &hi, &lo0, &lo1);
  mx_printf("STORM: codes 1-3 %u %u %u cause %08x epc %08x badv %08x entryhi %08x; wrote at %u "
            "lo %08x %08x; entry now mask %08x hi %08x lo %08x %08x\n",
            seen.n[0], seen.n[1], seen.n[2], seen.cause, seen.epc, seen.badvaddr, seen.entryhi,
            seen.index, seen.lo0, seen.lo1, pm, hi, lo0, lo1);
  MX_TEST_END(3);
  mx_exit(3);
}

/* Kernel memory, read by a syscall handler (kernel mode). Writing
   Status.KSU from user mode hung the PS2 (probe/kdump.c). */
static u32 k_read(u32 a) { return *(volatile u32 *)a; }

static u32 kread(u32 a) {
  register u32 a0 __asm__("$4") = a;
  register u32 v1 __asm__("$3") = 0x5A;
  register u32 v0 __asm__("$2");
  __asm__ volatile("syscall" : "=r"(v0), "+r"(v1) : "r"(a0) : "memory");
  return v0;
}

/* The kernel's handler table for exception codes 0..15: both the refill
   vector and the common vector dispatch through it (probe/kdump.c). */
static void print_table(const char *when) {
  mx_printf("  handler table (%s):", when);
  for (u32 i = 0; i < 16; i++)
    mx_printf(" %x", kread(0x80015340u + 4 * i));
  mx_printf("\n");
}

int main(int argc, char **argv) {
  int fail = 0;
  /* The invalid-entry tests (V = 0) hang the PS2 (2026-10-31): only with
     the argument inv (p19_tlb.elf inv). PCSX2 passes them. */
  int mod_only = !(argc >= 2 && argv[1][0] == 'i');
  u32 st = mx_status();
  mx_printf("p19_tlb: Status %08x KSU %u CU0 %u EXL %u ERL %u IE %u EIE %u; Wired %u\n", st,
            st >> 3 & 3, st >> 28 & 1, st >> 1 & 1, st >> 2 & 1, st & 1, st >> 16 & 1,
            cop0_wired());
  for (u32 i = 0; i < 48; i++) {
    u32 pm, hi, lo0, lo1;
    tlb_read(i, &pm, &hi, &lo0, &lo1);
    mx_printf("  tlb %2u: mask %08x hi %08x lo0 %08x lo1 %08x\n", i, pm, hi, lo0, lo1);
  }

  /* An entry by hand: 0x08000000 -> frame 0, 0x08004000 -> frame 1. */
  for (int f = 0; f < 4; f++)
    for (int i = 0; i < 16384; i += 4)
      *(u32 *)&frame[f][i] = (u32)f << 24 | (u32)i;
  mx_dcache_flush_all();
  u32 idx = 47;
  tlb_write(idx, PM16K, VA, lo_of(frame[0], 1, 1), lo_of(frame[1], 1, 1));
  volatile u32 *va = (volatile u32 *)VA;
  int ok = va[0] == 0 && va[1] == 4 && va[4096] == (1u << 24) && va[4097] == ((1u << 24) | 4);
  va[2] = 0x12345678;
  mx_dcache_flush_all();
  ok = ok && ((volatile u32 *)MX_UNCACHED(frame[0]))[2] == 0x12345678;
  mx_printf("  tlbwi map: %s; tlbp(VA) = %d\n", ok ? "ok" : "FAIL", tlb_probe(VA));
  fail |= !ok;

  /* The handlers. */
  mx_bios_set_syscall(0x5A, (void *)k_read);
  mx_bios_set_syscall(0x5B, (void *)k_resume);
  mx_bios_flush_cache(0);
  print_table("before");
  void (*tlb[3])(void) = {h_tlb1, h_tlb2, h_tlb3};
  for (int c = 1; c <= 3; c++)
    mx_bios_set_tlb_handler(c, tlb[c - 1]);
  print_table("after");
  mx_printf("  stubs %x %x %x\n", (u32)h_tlb1, (u32)h_tlb2, (u32)h_tlb3);

  /* Refill: the pair at VA + 32 KiB has no entry; the handler writes it at
     entry 46. */
  const u32 slot = 46;
  clear_seen();
  seen.index = slot;
  seen.lo0 = lo_of(frame[2], 1, 1), seen.lo1 = lo_of(frame[3], 1, 1);
  u32 v = ((volatile u32 *)(VA + 0x8000))[3];
  report("refill (load)");
  ok = v == ((2u << 24) | 12) && seen.n[1] == 1;
  mx_printf("    value %08x %s; tlbp finds it at %d (46 expected)\n", v, ok ? "ok" : "FAIL",
            tlb_probe(VA + 0x8000));
  fail |= !ok;
  tlb_write(slot, 0, 0xE0000000u + (slot << 13), 0, 0);

  clear_seen();
  ((volatile u32 *)(VA + 0xc000))[5] = 0xabcd;
  report("refill (store)");
  ok = seen.n[2] == 1 && ((volatile u32 *)(VA + 0xc000))[5] == 0xabcd;
  fail |= !ok;
  tlb_write(slot, 0, 0xE0000000u + (slot << 13), 0, 0);

  /* Invalid: entry 47 with lo0 V = 0; the handler rewrites entry 47. */
  if (mod_only)
    goto modified;
  tlb_write(idx, PM16K, VA, lo_of(frame[0], 0, 1), lo_of(frame[1], 1, 1));
  clear_seen();
  seen.index = idx;
  seen.lo0 = lo_of(frame[0], 1, 1), seen.lo1 = lo_of(frame[1], 1, 1);
  v = va[1];
  report("invalid (load)");
  fail |= v != 4 || seen.n[1] != 1;

  tlb_write(idx, PM16K, VA, lo_of(frame[0], 0, 1), lo_of(frame[1], 1, 1));
  clear_seen();
  va[6] = 6;
  report("invalid (store)");
  fail |= va[6] != 6 || seen.n[2] != 1;

modified:
  /* Modified: D = 0. Reported, not checked: PCSX2 ignores D (writes go
     through without an exception), the PS2 should raise code 1. */
  tlb_write(idx, PM16K, VA, lo_of(frame[0], 1, 0), lo_of(frame[1], 1, 1));
  clear_seen();
  seen.index = idx;
  seen.lo0 = lo_of(frame[0], 1, 1), seen.lo1 = lo_of(frame[1], 1, 1);
  v = va[1]; /* a load is fine */
  va[7] = 7;
  report("modified (store)");
  mx_printf("    store to a clean page: %s\n",
            seen.n[0] ? "TLB modified raised" : "no exception (D ignored)");
  fail |= va[7] != 7;

  /* Cost: a refill, then the entry cleared by hand, 64 times. */
  clear_seen();
  seen.index = slot;
  seen.lo0 = lo_of(frame[2], 1, 1), seen.lo1 = lo_of(frame[3], 1, 1);
  u32 t0 = mx_count();
  for (int r = 0; r < 64; r++) {
    (void)((volatile u32 *)(VA + 0x8000))[0];
    tlb_write(slot, 0, 0xE0000000u + (slot << 13), 0, 0);
  }
  u32 t1 = mx_count();
  mx_printf("  64 refill + clear: %u cycles each (handler calls %u)\n", (t1 - t0) / 64,
            seen.n[1]);
  fail |= seen.n[1] != 64;

  /* Exception return from C. */
  ok = eret_from_c();
  mx_printf("  eret from C: %s (Status %08x)\n", ok ? "ok" : "FAIL", mx_status());
  fail |= !ok;

  tlb_write(idx, 0, 0xE0000000u + (idx << 13), 0, 0);
  mx_exception_init();
  mx_printf("p19_tlb: %s\n", fail ? "FAIL" : "ok");
  MX_TEST_END(fail);
  return fail;
}
