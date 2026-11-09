#ifndef MX_PROC_H
#define MX_PROC_H

/* Processes under mx-kernel (kernel version 13, spawn and wait 14, events 15; docs/kernel.md,
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

/* Kernel version 14. Start a program in front, as mx_loader_launch_beside
   (args: argc words, each with its 0; swap: the swap file or 0), but come
   back at once: its process (> 0), -1 if it did not start (this one is in
   front again then). This program goes on beside it, and may start the next
   once this one ended. */
int mx_proc_spawn(int argc, const char *args, u32 args_len, const char *swap);
/* The end of a program this one started: 1 and its exit status in *status
   (0: not wanted) when it ended (the last one only), 0 while it runs (only
   with nohang; without, the thread waits for its end), -1 not such one. */
int mx_proc_wait(int id, int *status, int nohang);
/* End the program in front, as if it called mx_exit(status): 0, or -1 when
   id is not that one, is the caller, or cannot end without a restart. Its
   end follows soon after: mx_proc_wait for it. */
int mx_proc_kill(int id, int status);

/* Kernel version 15: the kernel's events to this process, as messages on
   one of its ports (<mx/msg.h>): from process 0, a struct mx_event each,
   no reply wanted. 0 to stop them. 0, or -1 for a port not its own. An
   event that finds the kernel's message slots full is dropped. */
enum {
  MX_EVENT_END = 1, /* a program it started ended: proc, its exit status */
};
struct mx_event {
  u32 type; /* MX_EVENT_* */
  s32 proc;
  s32 status;
  u32 pad;
};
int mx_proc_events(int port);

#ifdef __cplusplus
}
#endif

#endif
