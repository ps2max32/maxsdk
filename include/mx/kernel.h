#ifndef MX_KERNEL_H
#define MX_KERNEL_H

/* The services of mx-kernel (tools/mxlink, resident below 1 MiB), for libmxk:
   docs/kernel.md. Programs do not use this header: they call the same
   functions in either library, libmx (no kernel) or libmxk (MX_MEMORY=vm).

   The loader passes a pointer to this table. Rules: functions are only added
   at the end, with a new version; libmxk reads a table of at least its own
   version, which it checks with size. */

#include <mx/debug.h>
#include <mx/sif.h>
#include <mx/types.h>

#define MX_KERNEL_MAGIC 0x4c4e524bu /* "KRNL" */
#define MX_KERNEL_VERSION 3

struct mxk_services {
  u32 magic;   /* MX_KERNEL_MAGIC */
  u32 version; /* MX_KERNEL_VERSION of the kernel */
  u32 size;    /* sizeof this table in the kernel */

  /* program: the end of this one, the start of the next with the IOP kept,
     the swap file all programs share (0: none) */
  void (*exit)(int status) __attribute__((noreturn));
  void (*launch)(int argc, const char *args, u32 args_len, const char *swap)
      __attribute__((noreturn));
  const char *(*swap)(void);

  /* SIF, as <mx/sif.h> (version 2): the kernel's one client of the IOP's SIF.
     Addresses are core: libmxk resolves VM. wait: cycles before a call gives
     up with -2, 0 never. */
  int (*sif_bind_tries)(struct mx_sif_client *c, u32 sid, int n);
  int (*sif_bound)(const struct mx_sif_client *c);
  int (*sif_call)(struct mx_sif_client *c, int fno, const void *send, u32 ssize, void *recv,
                  u32 rsize, u32 wait);
  void (*sif_call_async)(struct mx_sif_client *c, int fno, const void *send, u32 ssize,
                         void *recv, u32 rsize);
  int (*sif_wait)(struct mx_sif_client *c);
  int (*sif_serve)(u32 sid, mx_sif_server_fn fn, void *arg);
  int (*iop_read)(u32 iop, void *dst, u32 size);
  int (*iop_reset)(const char *arg);
  void (*sif_waits)(u32 out[4]);

  /* exceptions (version 3): the vector for what the program's VM cannot
     handle, the handlers set again with note read at a crash (text for the
     report), and the report of the last program */
  void (*exception_vector)(void);
  void (*exception_setup)(const char **note);
  int (*crash)(struct mx_crash *c);
};

/* The kernel's side of sif_call (src/base/sif.c). */
int mx__sif_call(struct mx_sif_client *c, int fno, const void *send, u32 ssize, void *recv,
                 u32 rsize, u32 wait);

/* The table, 0 without a kernel. */
extern const struct mxk_services *mx__kernel;

#endif
