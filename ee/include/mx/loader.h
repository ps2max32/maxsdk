#ifndef MX_LOADER_H
#define MX_LOADER_H

/* What a loader passes in a0 to a program's __start (ee/crt/crt0.S): ps2link
   ({ pid; argc; argv[16]; payload }, the pid being the address of its
   thread id), or mxlink, which puts MX_LOADER_MAGIC in place of the pid for
   a program linked into VM. */

#include <mx/types.h>

#define MX_MAX_ARGS 16
#define MX_LOADER_MAGIC 0x444c584du /* "MXLD" */

struct mx_args {
  s32 argc;
  char *argv[MX_MAX_ARGS];
  char payload[256];
};

struct mx_loader_args {
  u32 pid; /* ps2link: the address of its thread id; mxlink: MX_LOADER_MAGIC */
  struct mx_args args;
  /* mxlink (ps2max/apps/mxos) only, after ps2link's block (programs of
     ps2sdk and older maxsdk ones do not read them): ret_magic is
     MX_LOADER_RET, and ret(status) returns to mxlink instead of the BIOS's
     Exit (which reboots the PS2). mx_exit calls it. */
  u32 ret_magic;
  void (*ret)(int status) __attribute__((noreturn));
  /* mxlink only, launch_magic being MX_LOADER_LAUNCH: end this program and
     start another with the IOP as it is (its modules and open devices stay).
     mx_loader_launch calls it. swap: the system's swap file, used in place of
     the program's own, or 0. From kernel version 9 a program whose core is at
     a run of its own and without VM stays: the call returns the other's exit
     status when it ends (docs/mx-os-plan.md, step 2c). */
  u32 launch_magic;
  int (*launch)(int argc, const char *args, u32 args_len, const char *swap);
  const char *swap;
  /* mx-kernel (<mx/kernel.h>): the table of its services for libmxk, and
     kernel_magic MX_KERNEL_MAGIC. The fields above stay for libmx and older
     programs. */
  u32 kernel_magic;
  const struct mxk_services *kernel;
};

#define MX_LOADER_RET 0x4b4c584du /* "MXLK" */
#define MX_LOADER_LAUNCH 0x4e4c584du /* "MXLN" */

/* 1 if the program has MX_LOADER_MAGIC from mxlink (argv[0] is then the path
   of its ELF), 0 if not. */
int mx_loader_mxload(void);

/* Have mxlink start another program, the IOP kept: args is argc strings
   one after the other, the first the path of its ELF; swap (or 0) is the
   swap file all programs share, used in place of the one they ask for.
   This program ends, or, under a kernel of version 9 with its core at a run
   of its own (MX_CORE_HEAP) and without VM, stays: the call returns the other
   program's exit status (0-255) when it has ended; GS memory, the display,
   the pad's ports and open files are to be set up again then. Returns -1 if
   the program was not started by an mxlink that can do this, or the other
   could not be started (this one staying). */
int mx_loader_launch(int argc, const char *args, u32 args_len, const char *swap);

/* As mx_loader_launch for a program with its core at a run of its own and
   without VM, under a kernel of version 12: the calling thread waits for the
   other's end as there, and this program's other threads go on running
   beside it, in their own address space (docs/mx-os-apps-plan.md, step
   2e-2). While the other runs they may not end the program (mx_exit), nor
   use the pad. Returns -1 if the kernel cannot. */
int mx_loader_launch_beside(int argc, const char *args, u32 args_len, const char *swap);

/* A program without VM asks for its core at a physical run of its own (a
   kernel of version 9), with this core heap after its end (bytes): what
   mx_sbrk, malloc and the heaps (<mx/mem.h>) have, the rest of memory being
   for others. Without it such a program has all memory, at its own
   addresses. At file scope, once: MX_CORE_HEAP(512 << 10); */
struct mx__core_heap_note {
  u32 namesz, descsz, type;
  char name[4];
  u32 bytes;
};
#define MX_CORE_HEAP(n)                                                                    \
  __attribute__((section(".note.mxk.heap"), used, aligned(4))) static const struct          \
      mx__core_heap_note mx__core_heap_note = {4, 4, 2, "MXK", (n)}

#endif
