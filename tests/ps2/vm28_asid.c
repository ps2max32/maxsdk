/* Step 2b (docs/mx-os-plan.md): a libmxk program runs under an ASID of its
   own, and the VM's entries are its: G = 0, its ASID in EntryHi. Started
   twice in a row, the second gets another ASID. Linked into VM, started by
   mxlink. */
#include "t.h"
#include <mx/vm.h>
#include <mx/mem.h>

static u32 entry_hi(void) {
  u32 v;
  __asm__ volatile("mfc0 %0, $10" : "=r"(v));
  return v;
}

/* entry i, with EntryHi (the CPU's ASID) as it was */
static void tlb_read(u32 i, u32 *hi, u32 *lo0, u32 *lo1) {
  u32 keep = entry_hi();
  int was = mx_di();
  __asm__ volatile("mtc0 %3, $0\n\tsync.p\n\ttlbr\n\tsync.p\n\t"
                   "mfc0 %0, $10\n\tmfc0 %1, $2\n\tmfc0 %2, $3\n\tmtc0 %4, $10\n\tsync.p"
                   : "=r"(*hi), "=r"(*lo0), "=r"(*lo1)
                   : "r"(i), "r"(keep));
  mx_irq_restore(was);
}

int main(void) {
  u32 asid = entry_hi() & 0xFFu;
  mx_printf("vm28: ASID %u\n", asid);
  CHECK(asid != 0);

  /* VM pages touched: their entries in the TLB */
  u32 n = 64 * 1024;
  volatile u32 *p = mx_sbrk(n * 4);
  CHECK(p != 0);
  CHECK(mx_is_vm((const void *)p));
  for (u32 i = 0; i < n; i += 1024)
    p[i] = i ^ 0x5A5A5A5Au;
  u32 bad = 0;
  for (u32 i = 0; i < n; i += 1024)
    bad += p[i] != (i ^ 0x5A5A5A5Au);
  CHECK_EQ(bad, 0);

  u32 vm_entries = 0, global = 0, other = 0;
  for (u32 i = 0; i < 48; i++) {
    u32 hi, lo0, lo1;
    tlb_read(i, &hi, &lo0, &lo1);
    if (hi < MX_VM_BASE || hi >= MX_VM_LIMIT || !((lo0 | lo1) & 2))
      continue;
    vm_entries++;
    global += lo0 & lo1 & 1;
    other += (hi & 0xFFu) != asid;
  }
  mx_printf("vm28: %u VM entries, %u global, %u of another ASID\n", vm_entries, global, other);
  CHECK(vm_entries > 0);
  CHECK_EQ(global, 0);
  CHECK_EQ(other, 0);
  /* the ASID stayed through all of it */
  CHECK_EQ(entry_hi() & 0xFFu, asid);
  return t_done("vm28");
}
