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

void *mx__heap_take(u32 keep, u32 align, u32 *bytes);
/* Shown by the crash report after a fatal VM fault. */
extern const char *mx__exception_note;
#endif

#endif
