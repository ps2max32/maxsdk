#ifndef MX_KERNEL_H
#define MX_KERNEL_H

/* The services of mx-kernel (tools/mxlink, resident below 1 MiB), for libmxk:
   docs/kernel.md. Programs do not use this header: they call the same
   functions in either library, libmx (no kernel) or libmxk (MX_MEMORY=vm).

   The loader passes a pointer to this table. Rules: functions are only added
   at the end, with a new version; libmxk reads a table of at least its own
   version, which it checks with size. */

#include <mx/types.h>

#define MX_KERNEL_MAGIC 0x4c4e524bu /* "KRNL" */
#define MX_KERNEL_VERSION 1

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
};

/* The table, 0 without a kernel. */
extern const struct mxk_services *mx__kernel;

#endif
