/* libmxk: the services that are the kernel's, through its table
   (<mx/kernel.h>, docs/kernel.md). */

#include <mx/debug.h>
#include <mx/kernel.h>
#include <mx/mx.h>
#include <mx/sifdma.h>

#define MXK_STUB(ret, name, params, args) \
  ret name params { return mx__kernel->name args; }
MXK_CALLS(MXK_STUB)

/* The note that tells the kernel, before the program runs, that it is on
   libmxk and which kernel version it was built for: a kernel of version 9
   may then put its core at a physical run of its own and writes the offset
   into mx__core_off (docs/mx-os-plan.md, step 2b). A PT_NOTE (the linker scripts in ee/ld). */
struct mxk_note {
  u32 namesz, descsz, type;
  char name[4];
  u32 version;
  u32 *core_off;
};
__attribute__((section(".note.mxk"), used, aligned(4))) static const struct mxk_note mxk_note = {
    4, 8, MXK_NOTE_TYPE, "MXK", MX_KERNEL_VERSION, &mx__core_off};

/* <mx/mem.h>'s default heaps: const for programs, set here at the start */
struct mx_heap *heap_pinned __asm__("mx_heap_pinned");
struct mx_heap *heap_stream __asm__("mx_heap_stream");
struct mx_heap *heap_paged __asm__("mx_heap_paged");

extern char __mx_vm_start[] __attribute__((weak));
extern char __mx_vm_end[] __attribute__((weak));

int mx__vm_image(void) { return (uintptr_t)__mx_vm_end > (uintptr_t)__mx_vm_start; }

void mx__kstart(void *heap_lo, void *heap_hi) {
  struct mx_heap *h[3];
  mx__kernel->program(heap_lo, heap_hi, __mx_vm_start, __mx_vm_end);
  mx__kernel->heaps(h);
  heap_pinned = h[0], heap_stream = h[1], heap_paged = h[2];
  mx__irq_tell_kernel();
}

/* debug output comes before the kernel is known, and without one */
int mx_tty_write(const char *s, u32 n) { return mx__kernel ? mx__kernel->mx_tty_write(s, n) : -1; }

/* tests: the fault is theirs to see */
void (*mx__core_fault_hook)(const void *p, const char *what);
void mx__core_fault(const void *p, const char *what) {
  if (mx__core_fault_hook)
    mx__core_fault_hook(p, what);
  else
    mx__kernel->mx__core_fault(p, what);
}

void mx__fio_ending(void (*stop)(void)) {
  if (mx__kernel)
    mx__kernel->fio_ending(stop);
  else
    stop();
}

void mx__paddr_fault(const void *p, u32 bytes) { mx__kernel->mx__paddr_fault(p, bytes); }

void mx_exception_init(void) { mx__kernel->exception_setup(0); }
int mx_crash_last(struct mx_crash *c) { return mx__kernel->crash(c); }

/* SIF DMA is the kernel's (mx OS plan 2h-3): sif.c's calls in libmxk go
   to the kernel's services before they reach these, which were the BIOS's
   SifSetDma, SifSetDChain, SifGetReg, ... (src/base/sifdma.c, in libmx). A
   program that gets here anyway ends, said so, instead of calling the BIOS. */
static void __attribute__((noreturn)) sif_not_here(const char *what) {
  mx_printf("maxsdk: %s from a program: SIF DMA is the kernel's\n", what);
  mx_exit(255);
}
u32 mx__sif_dma_set(const void *transfers, int n, int intr) { sif_not_here("SifSetDma"); }
int mx__sif_dma_stat(u32 id) { sif_not_here("SifDmaStat"); }
void mx__sif_dchain(int intr) { sif_not_here("SifSetDChain"); }
void mx__sif_stop_dma(void) { sif_not_here("SifStopDma"); }
u32 mx__sif_get_reg(u32 reg) { sif_not_here("SifGetReg"); }
u32 mx__sif_set_reg(u32 reg, u32 v) { sif_not_here("SifSetReg"); }
