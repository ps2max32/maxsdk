#ifndef MX_PROC_H
#define MX_PROC_H

/* Processes under mx-kernel (kernel version 13; docs/kernel.md,
   docs/mx-os-apps-plan.md step 2e-3): a program started by the kernel is a
   process, with an address space of its own. libmxk only: libmx's calls
   return -1. */

#include <mx/types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  MX_PROC_FRONT = 1,   /* the program the kernel started last */
  MX_PROC_BESIDE = 2,  /* goes on beside it (mx_loader_launch_beside) */
  MX_PROC_WAITING = 3, /* waits for it to end (mx_loader_launch) */
};

struct mx_proc_info {
  u8 id;        /* the process (1-255) */
  u8 state;     /* MX_PROC_* */
  u16 pad;
  u32 core_lo;  /* its core: physical, 0 when at the kernel's default place */
  u32 core_hi;
  char name[48]; /* the path it was started from, cut to fit */
};

/* this program's process (1-255); -1 without a kernel that knows */
int mx_proc_self(void);
/* the processes, at most max of them in p: how many there are */
int mx_proc_list(struct mx_proc_info *p, int max);

#ifdef __cplusplus
}
#endif

#endif
