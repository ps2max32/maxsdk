#ifndef MX_PROC_H
#define MX_PROC_H

/* Processes under mx-kernel (kernel version 13, spawn and wait 14, events 15, services 18; docs/kernel.md,
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
/* kernel version 26: mx_proc_info.flags, its roles (from the card's
   configuration: the shell's path, SERVICES.CFG) */
enum {
  MX_PROC_SHELL = 1,   /* the kernel's shell */
  MX_PROC_SERVICE = 2, /* a service (mx_proc_service) */
};

struct mx_proc_info {
  u8 id;        /* the process (1-255) */
  u8 state;     /* MX_PROC_* */
  u16 flags;    /* MX_PROC_SHELL, MX_PROC_SERVICE (kernel version 26; 0 before) */
  u32 core_lo;  /* its core: physical, 0 when at the kernel's default place */
  u32 core_hi;
  char name[48]; /* the path it was started from, cut to fit */
};

/* Who holds main memory (kernel version 37): for the kernel (id 0, the
   first) and each process, in frames of 32 KiB. core: its core (the
   kernel's: its own MiB, which is not in the pool). pinned: what else it
   holds resident (pinned heaps, files mapped resident, its VM's tables;
   the kernel's: all pinned frames that are no process's). paged: frames
   that hold pages of its now, which the pager may take. A process that is
   not in front keeps core and pinned (mx OS plan step 6: they are to leave
   too). */
struct mx_proc_mem {
  u8 id, state; /* as mx_proc_info */
  u16 flags;
  u32 core, pinned, paged;
};
int mx_proc_memory(struct mx_proc_mem *m, int max);

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
   end follows soon after: mx_proc_wait for it. Since kernel version 25 also
   one that waits or is paused: it is brought in front and ended there, the
   caller paused meanwhile (it comes back when that one has ended). */
int mx_proc_kill(int id, int status);
/* Kernel version 24 (mx OS plan step 4d): process id (one that waits or runs
   beside, not the service) in front. The program in front until now is
   paused: its threads stop, its interrupt handlers are taken out, and it is
   in front again when the new one ends or another mx_proc_front brings it
   back, with its memory, kernel state and VU0 registers. 0, or -1 (id not there, or no
   program in front that can wait). Called by the program in front itself,
   it comes back when it is in front again. */
int mx_proc_front(int id);

/* Kernel version 15: the kernel's events to this process, as messages on
   one of its ports (<mx/msg.h>): from process 0, a struct mx_event each,
   no reply wanted. 0 to stop them. 0, or -1 for a port not its own. An
   event that finds the kernel's message slots full is dropped. */
enum {
  MX_EVENT_END = 1, /* a program it started ended: proc, its exit status */
  /* kernel version 25: the home button was pressed in process proc, which
     is now paused; this process (the shell) is in front. mx_proc_front(proc)
     brings it back, mx_proc_kill(proc, status) ends it. */
  MX_EVENT_HOME = 2,
  /* kernel version 26, hints (read the state again; while a process is not
     in front they are coalesced): a device came or went (<mx/dev.h>;
     status: mx_dev_gen), a notification was added or changed
     (<mx/notify.h>; status: mx_notify_seq) */
  MX_EVENT_DEVICE = 3,
  MX_EVENT_NOTIFY = 4,
  /* a controller came or went, or its battery runs low (status: a counter
     of such changes) */
  MX_EVENT_PAD = 5,
  /* kernel version 27: a setting changed (<mx/setting.h>; status:
     mx_setting_seq; mx_setting_list(seq seen before) says which) */
  MX_EVENT_SETTING = 6,
  /* kernel version 30: the network changed: the link, the address
     (<mx/net.h>'s mx_net_status; status: its gen) */
  MX_EVENT_NET = 7,
};
struct mx_event {
  u32 type; /* MX_EVENT_* */
  s32 proc;
  s32 status;
  u32 pad;
};
int mx_proc_events(int port);

/* Kernel version 18: this program (its core at a run of its own, without
   VM: MX_CORE_HEAP) becomes a service: it stays beside every program in
   front from now on, and the one that comes next in front (the shell; on
   PCSX2 host:mxload.txt's) starts now. The kernel starts a service at the
   boot from mc0:/MXOS/SERVICES.CFG. A service's mx_proc_spawn starts the
   program in front when none runs (-1 while one does: mx_proc_kill it
   first), and at a program's end the next comes (the shell) unless it
   starts one. The kernel prints no test end marker while a service is
   beside: that is the service's. 0 or the started program's process; -1
   when this program cannot. */
int mx_proc_service(void);
/* Kernel version 21: the EE's physical memory (0 up to its size, 32 MiB), as
   the kernel sees it, read into or written from this program's core memory
   (not VM). Writing is for a service only (the host link's writemem): the
   caches are written back and emptied after it. 0, or -1. */
int mx_phys_read(u32 phys, void *to, u32 n);
int mx_phys_write(u32 phys, const void *from, u32 n);

#ifdef __cplusplus
}
#endif

#endif
