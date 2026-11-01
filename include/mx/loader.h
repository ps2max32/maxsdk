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
};

/* 1 if the program was started by mxload (argv[0] is then the path of its
   ELF), 0 if not. */
int mx_loader_mxload(void);

#endif
