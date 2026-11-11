/* C side of the startup: heap, cache, arguments, debug output, main, exit. */

#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/mx.h>
#include <mx/vm.h>
#include <mx/vu.h>

struct mx_args mx__kernel_args;
struct mx_loader_args *mx__loader_args;
/* the loader's arguments, copied */
static struct mx_args mxload_args;
static int by_mxload;
/* mxlink's way back (<mx/loader.h>), 0 for the BIOS's Exit */
static void (*loader_ret)(int status) __attribute__((noreturn));
static int (*loader_launch)(int argc, const char *args, u32 args_len, const char *swap);
const char *mx__loader_swap; /* the swap file mxlink gave, or 0 */

const struct mxk_services *mx__kernel;

int mx_loader_mxload(void) { return by_mxload; }

/* a0 is garbage when no loader passed one (the BIOS, PCSX2 -elf) */
static int loader_args_ok(const struct mx_loader_args *l) {
  u32 p = (u32)(uintptr_t)l;
  return p && !(p & 3) && p < 0x02000000u - sizeof *l;
}

/* libmxk (MX_KERNEL): the kernel's table, if a kernel started this program */
static void find_kernel(const struct mx_loader_args *l) {
#ifdef MX_KERNEL
  if (!loader_args_ok(l) || l->kernel_magic != MX_KERNEL_MAGIC)
    return;
  const struct mxk_services *k = l->kernel;
  u32 p = (u32)(uintptr_t)k;
  if (p & 3 || p >= 0x00100000u || k->magic != MX_KERNEL_MAGIC ||
      k->version < MX_KERNEL_MIN || k->size < MXK_MIN_SIZE)
    return;
  mx__kernel = k;
  loader_ret = k->exit;
  loader_launch = k->launch;
  /* version 10: its threads, semaphores and interrupt handlers (ksys.c) */
  if (k->version >= 10 && k->kthreads)
    mx__kthreads = k->kthreads;
#else
  (void)l;
#endif
}

static struct mx_args *copy_mxload_args(const struct mx_args *from) {
  struct mx_args *a = &mxload_args;
  *a = *from;
  if (a->argc < 0 || a->argc > MX_MAX_ARGS)
    a->argc = 0;
  for (int i = 0; i < a->argc; i++) {
    u32 off = (u32)(from->argv[i] - from->payload);
    a->argv[i] = off < sizeof a->payload ? a->payload + off : a->payload + sizeof a->payload - 1;
  }
  a->payload[sizeof a->payload - 1] = 0;
  return a;
}

extern char _end[];
extern char __stack[], __stack_size[]; /* the main stack (crt0 sets it) */
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

int main(int argc, char **argv);
int mx__vm_image(void);
int mx__vm_load_image(const char *path);
void mx__libc_exit(int status) __attribute__((weak, noreturn));

static void *heap_end;

void *mx_heap_end(void) { return heap_end; }
void *mx_heap_start(void) { return _end; }

void mx__vm_report(void);
void mx__sif_report(void);

/* with VM on: what the pager and the IOP's waits cost, once */
static void report(void) {
  static int done;
  if (mx_vm_active() && !done) {
    done = 1;
    mx__vm_report();
    mx__sif_report();
  }
}

/* defined by the kernel (mxlink): its own fatal ends print no marker either */
extern const int mx__kernel_self __attribute__((weak));

/* The end marker. mxlink prints it when the program is back: here a host
   would stop at it and go away while the program still ends, with its files on
   host:. Without a loader to go back to (PCSX2 with -elf, ps2link) the program
   prints it, once: at MX_TEST_END, else at mx_exit. */
static int marker_done;
static void end_marker(int status) {
  if (!loader_ret && !&mx__kernel_self && !marker_done) {
    marker_done = 1;
    mx_printf("@@PS2TEST_END@@ status=%d\n", status);
  }
}

void mx__test_end(int status) {
  report();
  end_marker(status);
}

void mx__vm_shutdown(void);
void mx__vm_swap_close(void);

/* the program's other threads (thread.c, when it has any) */
void mx__threads_stop(void) __attribute__((weak));
/* its software timers (irq/timer.c, when it had any) */
void mx__timer_stop(void) __attribute__((weak));
void mx__fio_ending(void (*stop)(void));

void mx_exit(int status) {
  /* Before the VM goes: what follows blocks in file calls, and a thread of the
     program that ran then would touch VM with its TLB gone, and hang. */
  if (mx__threads_stop)
    mx__fio_ending(mx__threads_stop);
  if (mx__timer_stop)
    mx__timer_stop();
  report();
  end_marker(status);
  mx_debug_flush();
  /* the BIOS's TLB back for ps2link (with VM on) */
  mx__vm_shutdown();
  /* not after a crash: it may have come in the file server's lock */
  if (status != 255)
    mx__vm_swap_close();
  /* back to mxlink without a reboot, if it started us */
  if (loader_ret)
    loader_ret(status);
  mx_bios_exit(status);
}

int mx_loader_launch_beside(int argc, const char *args, u32 args_len, const char *swap) {
  if (!mx__kernel || mx__kernel->version < 12 ||
      mx__kernel->size < __builtin_offsetof(struct mxk_services, launch_beside) + sizeof(void *) ||
      !mx__kernel->launch_beside)
    return -1;
  mx_debug_flush();
  return mx__kernel->launch_beside(argc, args, args_len, swap);
}

int mx_loader_launch(int argc, const char *args, u32 args_len, const char *swap) {
  if (!loader_launch)
    return -1;
  /* they may be in VM, which ends here */
  static char args_copy[256], swap_copy[64];
  if (args_len > sizeof args_copy)
    args_len = sizeof args_copy;
  for (u32 i = 0; i < args_len; i++)
    args_copy[i] = args[i];
  u32 n = 0;
  for (; swap && swap[n] && n < sizeof swap_copy - 1; n++)
    swap_copy[n] = swap[n];
  swap_copy[n] = 0;
  mx_debug_flush();
  mx__vm_shutdown();
  mx__vm_swap_close();
  return loader_launch(argc, args_copy, args_len, swap ? swap_copy : 0);
}

void mx__fio_base(const char *argv0);
void mx__kstart(void *heap_lo, void *heap_hi);
void mx__main(void) __attribute__((noreturn));
void mx__main(void) {
  struct mx_args *a = &mx__kernel_args;
  struct mx_loader_args *l = mx__loader_args;
  if (loader_args_ok(l) && l->ret_magic == MX_LOADER_RET && l->ret &&
      !((u32)(uintptr_t)l->ret & 3) && (u32)(uintptr_t)l->ret < 0x02000000u)
    loader_ret = l->ret;
  if (loader_ret && l->launch_magic == MX_LOADER_LAUNCH && l->launch &&
      !((u32)(uintptr_t)l->launch & 3) && (u32)(uintptr_t)l->launch < 0x02000000u)
    loader_launch = l->launch, mx__loader_swap = l->swap;
  find_kernel(l);

  /* The heap ends at the main stack. That is in the program (.stack, before
     _end) unless it was linked with __mx_top: the heap is then the rest of
     memory. On the kernel's threads without the BIOS (mx OS plan 2g: its
     arguments come with the loader's block below); on the BIOS's, its
     SetupThread (argc and argv into mx__kernel_args, the thread's gp, stack
     and end) and SetupHeap (-1: the heap up to that stack), as crt0 did */
  if (mx__kthreads) {
    uintptr_t st = (uintptr_t)__stack, end = (uintptr_t)_end; /* not as arrays: folded */
    heap_end = st < end ? (void *)MX_CORE_HI : (void *)st;
  } else {
    extern char _gp[];
    mx_bios_setup_thread(_gp, __stack, (int)(uintptr_t)__stack_size, &mx__kernel_args,
                         mx_bios_exit_thread);
    heap_end = mx_bios_setup_heap(_end, -1);
    if ((char *)heap_end < _end)
      heap_end = (void *)MX_CORE_HI;
  }
  mx_dcache_flush_all();
#ifdef MX_KERNEL
  /* no way to go on: its memory, files and VM are the kernel's */
  if (!mx__kernel) {
    mx_debug_sinks(MX_SINK_SIO);
    mx_printf("maxsdk: this program needs mx-kernel: start it with mxlink (apps/mxos)\n");
    mx_bios_exit(255);
  }
  mx__kstart(_end, heap_end);
#endif
  if (loader_args_ok(l) && l->pid == MX_LOADER_MAGIC) {
    by_mxload = 1;
    a = copy_mxload_args(&l->args);
  } else if ((a->argc == 0 || mx__kthreads) && loader_args_ok(l) && l->args.argc != 0)
    /* on the kernel's threads SetupThread answers for the BIOS thread they
       all run on, the kernel's own: its arguments are not the program's.
       Copied: the kernel's are the next program's once this one starts
       another and goes on (mx_proc_spawn) */
    a = copy_mxload_args(&l->args);

#ifndef MX_KERNEL
  if (a->argc > 0 && a->argv[0])
    mx__fio_base(a->argv[0]);
#endif
  mx_debug_init();
  mx_exception_init();
  /* a stack larger than .stack runs into the heap: --defsym=__stack_size
     instead of __mx_stack_size (ee/ld/maxsdk.ld) */
  uintptr_t st = (uintptr_t)__stack, end = (uintptr_t)_end;
  if (st < end && st + (uintptr_t)__stack_size > end) {
    mx_printf("maxsdk: the main stack (%u KiB) is larger than the space for it: link with "
              "--defsym=__mx_stack_size=<bytes>, not __stack_size\n",
              (u32)(uintptr_t)__stack_size >> 10);
    mx_exit(255);
  }
  mx_vif_reset(0);
  mx_vif_reset(1);
  mx_ei();
  /* VM before the constructors, so their malloc is in it already */
  if (mx__vm_image()) {
    /* linked into VM (ee/ld/maxsdk-vm.ld): VM on, then the program's VM part
       from its ELF */
    static const struct mx_vm_config none;
    if (!by_mxload) {
      mx_printf("maxsdk: this program is linked into VM: start it with mxlink (the PS2's loader, "
                "apps/mxos)\n");
      mx_exit(255);
    }
    if (mx_vm_init(&mx_vm_config ? &mx_vm_config : &none) < 0 ||
        mx__vm_load_image(a->argv[0]) < 0) {
      mx_printf("maxsdk: the program's VM part could not be set up\n");
      mx_exit(255);
    }
  } else if (&mx_vm_config && mx_vm_init(&mx_vm_config) < 0)
    mx_printf("maxsdk: mx_vm_config is set, but VM could not be turned on\n");

  for (void (**f)(void) = __init_array_start; f < __init_array_end; f++)
    (*f)();

  int status = main(a->argc, a->argv);
  if (mx__libc_exit)
    mx__libc_exit(status); /* libmxc: atexit handlers, static destructors */
  mx_exit(status);
}
