/* C side of the startup: heap, cache, arguments, debug output, main, exit. */

#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/loader.h>
#include <mx/mx.h>
#include <mx/vm.h>
#include <mx/vu.h>

struct mx_args mx__kernel_args;
struct mx_loader_args *mx__loader_args;
/* mxload's arguments, copied: its memory is the heap's now */
static struct mx_args mxload_args;
static int by_mxload;

int mx_loader_mxload(void) { return by_mxload; }

/* a0 is garbage when no loader passed one (the BIOS, PCSX2 -elf) */
static int loader_args_ok(const struct mx_loader_args *l) {
  u32 p = (u32)(uintptr_t)l;
  return p && !(p & 3) && p < 0x02000000u - sizeof *l;
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

void mx_exit(int status) {
  report();
  mx_debug_flush();
  mx_bios_exit(status);
}

void mx__main(void) __attribute__((noreturn));
void mx__main(void) {
  heap_end = mx_bios_setup_heap(_end, -1);
  mx_bios_flush_cache(0);

  struct mx_args *a = &mx__kernel_args;
  struct mx_loader_args *l = mx__loader_args;
  if (loader_args_ok(l) && l->pid == MX_LOADER_MAGIC) {
    by_mxload = 1;
    a = copy_mxload_args(&l->args);
  } else if (a->argc == 0 && loader_args_ok(l) && l->args.argc != 0)
    a = &l->args;

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
      mx_printf("maxsdk: this program is linked into VM: start it with mxload.elf\n");
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
