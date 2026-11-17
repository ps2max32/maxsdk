/* A TLB entry with one half invalid (V = 0), under mx-kernel's own vectors.
   docs/vm.md said that an access to the invalid half hangs the PS2 (4 runs
   under the BIOS, probe/p19_tlb.c), so the VM maps a page with both halves
   or not at all. A region of an odd number of large pages wants an entry
   with one half (mx OS plan step 6).

   Result (a SCPH-90006, 2026-11-17, kernel 37): it works as the manuals
   say (TX79 5.5.8, 6.1). The valid half reads; a read through the invalid
   half is a TLB exception of a load at that address, which the kernel
   reports, and the program ends with 255. The PS2 goes on.

   Three hangs on the way were this test's own, and are a rule for any
   program: never tlbr outside the kernel. It puts the entry's ASID into
   EntryHi, and a program's core is mapped under its own ASID (not global),
   so the instruction after it is not mapped any more; the kernel's tlbr
   runs from its global entries, and PCSX2 ignores the ASID.

   An entry by hand at an address that is no one's: 4 KiB pages of the
   program's core, written to TLB entry 47 and parked after the read.
     k70_tlbhalf.elf          both halves valid, a read through each
     k70_tlbhalf.elf steps    the same and the entry with its odd half
                              invalid, a step at a time, each said first
     k70_tlbhalf.elf half     starts itself twice: "good" reads through the
                              valid half, "bad" through the invalid one,
                              which must end in the kernel's crash report
     k70_tlbhalf.elf odd      the read through the invalid half alone
                              (status 255 expected)
   (status 0 expected for the first three) */
#include "t.h"
#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/thread.h>

MX_CORE_HEAP(256 << 10);

#define VA 0x7F000000u /* no one's: above the windows, below kseg0 */
#define ENTRY 47
#define MAGIC 0x600D600Du

static u32 page[2048] MX_ALIGNED(8192) MX_CORE_DATA = {MAGIC, [1024] = MAGIC + 1};

/* core: nothing here may fault while the entry is not the VM's.
   No tlbr: it puts the entry's ASID into EntryHi, and a program's core is
   mapped under its own ASID (not global), so the instruction after it is
   not mapped any more: on the PS2 that hung (PCSX2 ignores the ASID). The
   kernel's own tlbr runs from its global entries. So the entry is not
   saved: after the read it is parked (no address of anyone's, both halves
   invalid, as the VM's tlb_clear), and the VM writes it again when the page
   it held is next used.
   odd < 0: no read. both: the odd half valid too (the page's second 4 KiB). */
__attribute__((noinline)) static MX_CORE_FN u32 probe(int odd, int both) {
  u32 phys = mx_paddr_of(page).v;
  int was = mx_di();
  u32 hi, v;
  __asm__ volatile("mfc0 %0, $10" : "=r"(hi));
  u32 asid = hi & 0xFF;
  __asm__ volatile("mtc0 %0, $0\n\tmtc0 $0, $5\n\tmtc0 %1, $10\n\tmtc0 %2, $2\n\tmtc0 %3, $3\n\t"
                   "sync.p\n\ttlbwi\n\tsync.p"
                   :
                   : "r"(ENTRY), "r"(VA | asid), "r"(phys >> 12 << 6 | 0x1Eu),
                     "r"(both ? (phys + 0x1000u) >> 12 << 6 | 0x1Eu : 0u));
  v = odd < 0 ? 0 : *(volatile u32 *)(uintptr_t)(VA + (odd ? 0x1000u : 0));
  __asm__ volatile("mtc0 %0, $0\n\tmtc0 $0, $5\n\tmtc0 %1, $10\n\tmtc0 $0, $2\n\tmtc0 $0, $3\n\t"
                   "sync.p\n\ttlbwi\n\tsync.p\n\tmtc0 %2, $10\n\tsync.p"
                   :
                   : "r"(ENTRY), "r"((0xE0000000u + ((48u + ENTRY) << 13)) | asid), "r"(hi));
  mx_irq_restore(was);
  return v;
}

static int child(const char *self, const char *role, int *st) {
  static char args[256];
  u32 n = (u32)strlen(self) + 1, r = (u32)strlen(role) + 1;
  memcpy(args, self, n);
  memcpy(args + n, role, r);
  int id = mx_proc_spawn(2, args, n + r, 0);
  if (id <= 0)
    return -1;
  return mx_proc_wait(id, st, 0);
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "good")) {
    u32 v = probe(0, 0);
    mx_printf("k70 good: read %08x through the even half\n", v);
    return v == MAGIC ? 0 : 7;
  }
  if (argc > 1 && !strcmp(argv[1], "bad")) {
    mx_printf("k70 bad: reading through the invalid half\n");
    u32 v = probe(1, 0);
    mx_printf("k70 bad: it read %08x: no exception\n", v);
    return 8;
  }
  /* the experiment a step at a time, each said first (the line reaches the
     host before the step): "write" the entry with one half and put back,
     nothing read; "even" a read through the valid half; "odd" through the
     invalid one */
  if (argc > 1 && (!strcmp(argv[1], "write") || !strcmp(argv[1], "even") || !strcmp(argv[1], "odd"))) {
    int odd = argv[1][0] == 'w' ? -1 : argv[1][0] == 'o';
    mx_printf("k70 %s: the entry with its odd half invalid%s\n", argv[1],
              odd < 0 ? ", written and put back" : odd ? ", a read through the invalid half" : ", a read through the valid half");
    mx_debug_flush();
    mx_sleep_us(300000);
    u32 v = probe(odd, 0);
    mx_printf("k70 %s: done, read %08x\n", argv[1], v);
    CHECK(odd < 0 || (!odd && v == MAGIC));
    return t_done("k70");
  }
  if (argc > 1 && !strcmp(argv[1], "steps")) {
    /* each step said before it is done (for a PS2 that hangs: the last
       line tells where); the read through the invalid half is not among
       them: that is "odd", which must end in the kernel's crash report */
    static const struct {
      const char *what;
      int odd, both;
      u32 want;
    } st[5] = {{"two valid pages written and parked, nothing read", -1, 1, 0},
               {"two valid pages, a read through the even half", 0, 1, MAGIC},
               {"two valid pages, a read through the odd half", 1, 1, MAGIC + 1},
               {"the odd half invalid, written and parked, nothing read", -1, 0, 0},
               {"the odd half invalid, a read through the even half", 0, 0, MAGIC}};
    for (int w = 0; w < 5; w++) {
      mx_printf("k70 step %d: %s\n", w, st[w].what);
      mx_debug_flush();
      mx_sleep_us(300000);
      u32 r = probe(st[w].odd, st[w].both);
      mx_printf("k70 step %d: done: %08x\n", w, r);
      CHECK_EQ(r, st[w].want);
      mx_debug_flush();
      mx_sleep_us(300000);
    }
    return t_done("k70");
  }
  if (argc < 2 || strcmp(argv[1], "half")) {
    /* the control: both halves valid */
    u32 a = probe(0, 1), b = probe(1, 1);
    mx_printf("k70: both halves valid: read %08x and %08x\n", a, b);
    CHECK_EQ(a, MAGIC);
    CHECK_EQ(b, MAGIC + 1);
    return t_done("k70");
  }
  int st = -1;
  CHECK_EQ(child(argv[0], "good", &st), 1);
  CHECK_EQ(st, 0);
  st = -1;
  CHECK_EQ(child(argv[0], "bad", &st), 1);
  CHECK_EQ(st, 255);
  struct mx_crash c;
  memset(&c, 0, sizeof c);
  CHECK_EQ(mx_crash_last(&c), 1);
  mx_printf("k70: the crash: code %u (%s), address %08x, epc %08x\n", c.code, c.what ? c.what : "?",
            c.badvaddr, c.epc);
  CHECK_EQ(c.badvaddr, VA + 0x1000u);
  CHECK_EQ(c.code, 2); /* TLB, a load */
  return t_done("k70");
}
