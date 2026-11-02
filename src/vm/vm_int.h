/* Internals of the VM (vm.c, heap.c, tlb.S). */

#ifndef MX_VM_INT_H
#define MX_VM_INT_H

/* The registers at a TLB exception (tlb.S saves them, the offsets are
   tlb.S's): the GPRs, 128 bits each (0, k0 and k1 not kept), HI and LO
   (both pipelines), SA, then COP0. A page fault that needs the swap file
   copies this onto the faulting thread's stack and continues there. */
#define VMX_GPR(i) ((i) * 16)
#define VMX_HI 512
#define VMX_LO 528
#define VMX_SA 544
#define VMX_STATUS 548
#define VMX_CAUSE 552
#define VMX_EPC 556
#define VMX_BADVADDR 560
#define VMX_FRAME 564 /* where the copy went (slow path) */
#define VMX_SIZE 576

/* What mx__vm_exception (C) tells tlb.S to do. */
#define VMX_RETURN 0 /* handled: back to the instruction */
#define VMX_SLOW 1   /* continue in mx__vm_slow on the thread's stack */
#define VMX_FATAL 2  /* the crash report */

/* What tlb.S's fast refill reads and writes (vm.c keeps the rest): a
   refill of a VM page that is mapped and has no TLB entry is handled
   there, with three registers saved, before the full save and C. */
#define VMF_LO 0         /* u32 *: EntryLo0 of each page (vm.c's lo) */
#define VMF_PAGE_SLOT 4  /* u8 *: TLB entry of each page, 0: none */
#define VMF_NPAGES 8     /* pages the fast refill takes (0: none: a trace) */
#define VMF_NEXT 12      /* the TLB entry to take next */
#define VMF_REFILLS 16   /* all refills */
#define VMF_REFILLS_I 20 /* of them, instruction fetches */
#define VMF_STAMP 24     /* u32 *: each page's last refill (refills then), for eviction */
#define VMF_SLOT_PAGE 32 /* u16[48]: each TLB entry's page + 1, 0: none */
#define VMF_SAVE 128     /* three GPRs, 128 bits each */
#define VMF_SIZE 176
#define VMF_BASE_PAGE 0x1000 /* MX_VM_BASE >> 15 */
#define VMF_TLB_FIRST 24     /* the VM's TLB entries: 24..47 */

/* The syscall that resumes a slow fault (a negative one: its handler runs
   at EXL = 1 on the caller's stack). */
#define VM_RESUME_SYSCALL 0x5B

#ifndef __ASSEMBLER__
#include <mx/types.h>

struct mx__vm_x {
  u64 gpr[32][2];
  u64 hi[2], lo[2];
  u32 sa, status, cause, epc, badvaddr;
  struct mx__vm_x *frame;
  u32 pad[2];
};

_Static_assert(__builtin_offsetof(struct mx__vm_x, hi) == VMX_HI, "tlb.S offsets");
_Static_assert(__builtin_offsetof(struct mx__vm_x, sa) == VMX_SA, "tlb.S offsets");
_Static_assert(__builtin_offsetof(struct mx__vm_x, badvaddr) == VMX_BADVADDR, "tlb.S offsets");
_Static_assert(__builtin_offsetof(struct mx__vm_x, frame) == VMX_FRAME, "tlb.S offsets");
_Static_assert(sizeof(struct mx__vm_x) == VMX_SIZE, "tlb.S offsets");

struct mx__vm_fast {
  u32 *lo;
  u8 *page_slot;
  u32 npages, next_slot, refills, refills_i;
  u32 *stamp;
  u32 pad;
  u16 slot_page[48];
  u64 save[6];
};
_Static_assert(__builtin_offsetof(struct mx__vm_fast, next_slot) == VMF_NEXT, "tlb.S offsets");
_Static_assert(__builtin_offsetof(struct mx__vm_fast, refills_i) == VMF_REFILLS_I, "tlb.S offsets");
_Static_assert(__builtin_offsetof(struct mx__vm_fast, stamp) == VMF_STAMP, "tlb.S offsets");
_Static_assert(__builtin_offsetof(struct mx__vm_fast, slot_page) == VMF_SLOT_PAGE, "tlb.S offsets");
_Static_assert(__builtin_offsetof(struct mx__vm_fast, save) == VMF_SAVE, "tlb.S offsets");
_Static_assert(sizeof(struct mx__vm_fast) == VMF_SIZE, "tlb.S offsets");
extern struct mx__vm_fast mx__vm_fast;

void *mx__heap_take(u32 keep, u32 align, u32 *bytes);
void mx__heap_untake(void);
/* Shown by the crash report after a fatal VM fault. */
extern const char *mx__exception_note;
#endif

#endif
