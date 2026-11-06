#ifndef MX_LOADER_H
#define MX_LOADER_H

/* What a loader passes in a0 to a program's __start (crt/crt0.S): ps2link
   ({ pid; argc; argv[16]; payload }, the pid being the address of its
   thread id), or mxload, which puts MX_LOADER_MAGIC in place of the pid.
   mxload (loader/mxload.c) loads a program and then jumps to it in its own
   thread, as ps2link starts one. */

#include <mx/types.h>

#define MX_MAX_ARGS 16
#define MX_LOADER_MAGIC 0x444c584du /* "MXLD" */

struct mx_args {
  s32 argc;
  char *argv[MX_MAX_ARGS];
  char payload[256];
};

struct mx_loader_args {
  u32 pid; /* ps2link: the address of its thread id; mxload: MX_LOADER_MAGIC */
  struct mx_args args;
  /* mxlink (ps2max/tools/mxlink) only, after ps2link's block (programs of
     ps2sdk and older maxsdk ones do not read them): ret_magic is
     MX_LOADER_RET, and ret(status) returns to mxlink instead of the BIOS's
     Exit (which reboots the PS2). mx_exit calls it. */
  u32 ret_magic;
  void (*ret)(int status) __attribute__((noreturn));
  /* mxlink only, launch_magic being MX_LOADER_LAUNCH: end this program and
     start another with the IOP as it is (its modules and open devices stay).
     mx_loader_launch calls it. swap: the system's swap file, used in place of
     the program's own, or 0. */
  u32 launch_magic;
  void (*launch)(int argc, const char *args, u32 args_len, const char *swap)
      __attribute__((noreturn));
  const char *swap;
  /* mx-kernel (<mx/kernel.h>): the table of its services for libmxk, and
     kernel_magic MX_KERNEL_MAGIC. The fields above stay for libmx and older
     programs. */
  u32 kernel_magic;
  const struct mxk_services *kernel;
};

#define MX_LOADER_RET 0x4b4c584du /* "MXLK" */
#define MX_LOADER_LAUNCH 0x4e4c584du /* "MXLN" */

/* 1 if the program was started by mxload (argv[0] is then the path of its
   ELF), 0 if not. */
int mx_loader_mxload(void);

/* End this program and have mxlink start another, the IOP kept: args is
   argc strings one after the other, the first the path of its ELF; swap (or
   0) is the swap file all programs share, used in place of the one they ask
   for. Returns -1 if the program was not started by an mxlink that can do
   this. */
int mx_loader_launch(int argc, const char *args, u32 args_len, const char *swap);

#endif
