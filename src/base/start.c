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
/* mxload's arguments, copied: its memory is the heap's now */
static struct mx_args mxload_args;
static int by_mxload;
/* mxlink's way back (<mx/loader.h>), 0 for the BIOS's Exit */
static void (*loader_ret)(int status) __attribute__((noreturn));
static void (*loader_launch)(int argc, const char *args, u32 args_len, const char *swap)
    __attribute__((noreturn));
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
      k->version < MX_KERNEL_VERSION || k->size < sizeof *k)
    return;
  mx__kernel = k;
  loader_ret = k->exit;
  loader_launch = k->launch;
  mx__loader_swap = k->swap();
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

void mx__test_end(int status) {
  report();
  mx_printf("@@PS2TEST_END@@ status=%d\n", status);
}

void mx__vm_shutdown(void);
void mx__vm_swap_close(void);

void mx_exit(int status) {
  report();
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

int mx_loader_launch(int argc, const char *args, u32 args_len, const char *swap) {
  if (!loader_launch)
    return -1;
  mx_debug_flush();
  mx__vm_shutdown();
  mx__vm_swap_close();
  loader_launch(argc, args, args_len, swap);
}

void mx__fio_base(const char *argv0);
void mx__main(void) __attribute__((noreturn));
void mx__main(void) {
  heap_end = mx_bios_setup_heap(_end, -1);
  mx_bios_flush_cache(0);

  struct mx_args *a = &mx__kernel_args;
  struct mx_loader_args *l = mx__loader_args;
  if (loader_args_ok(l) && l->ret_magic == MX_LOADER_RET && l->ret &&
      !((u32)(uintptr_t)l->ret & 3) && (u32)(uintptr_t)l->ret < 0x02000000u)
    loader_ret = l->ret;
  if (loader_ret && l->launch_magic == MX_LOADER_LAUNCH && l->launch &&
      !((u32)(uintptr_t)l->launch & 3) && (u32)(uintptr_t)l->launch < 0x02000000u)
    loader_launch = l->launch, mx__loader_swap = l->swap;
  find_kernel(l);
  if (loader_args_ok(l) && l->pid == MX_LOADER_MAGIC) {
    by_mxload = 1;
    a = copy_mxload_args(&l->args);
  } else if (a->argc == 0 && loader_args_ok(l) && l->args.argc != 0)
    a = &l->args;

  if (a->argc > 0 && a->argv[0])
    mx__fio_base(a->argv[0]);
  mx_debug_init();
  mx_exception_init();
  mx_vif_reset(0);
  mx_vif_reset(1);
  mx_ei();
  /* VM before the constructors, so their malloc is in it already */
  if (mx__vm_image()) {
    /* linked into VM (ld/maxsdk-vm.ld): VM on, then the program's VM part
       from its ELF */
    static const struct mx_vm_config none;
    if (!by_mxload) {
      mx_printf("maxsdk: this program is linked into VM: start it with mxlink (the PS2's loader, "
                "tools/mxlink) or mxload.elf\n");
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
