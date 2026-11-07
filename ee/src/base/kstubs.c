/* libmxk: the services that are the kernel's, through its table
   (<mx/kernel.h>, docs/kernel.md). */

#include <mx/debug.h>
#include <mx/kernel.h>

#define MXK_STUB(ret, name, params, args) \
  ret name params { return mx__kernel->name args; }
MXK_CALLS(MXK_STUB)

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
