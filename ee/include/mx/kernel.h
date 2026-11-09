#ifndef MX_KERNEL_H
#define MX_KERNEL_H

/* The services of mx-kernel (apps/mxos, resident below 1 MiB), for libmxk:
   docs/kernel.md. Programs do not use this header: they call the same
   functions in either library, libmx (no kernel) or libmxk (MX_MEMORY=vm).

   The loader passes a pointer to this table. Rules: functions are only added
   at the end, with a new version; libmxk reads a table of at least its own
   version, which it checks with size. */

#include <mx/debug.h>
#include <mx/mem.h>
#include <mx/sif.h>
#include <mx/bios.h>
#include <mx/types.h>
#include <mx/video.h>
#include <mx/proc.h>
#include <mx/msg.h>
#include <mx/vm.h>

#define MX_KERNEL_MAGIC 0x4c4e524bu /* "KRNL" */
#define MX_KERNEL_VERSION 18
/* the PT_NOTE of a libmxk program (src/base/kstubs.c): name "MXK", this type,
   the version, the address of its mx__core_off */
#define MXK_NOTE_TYPE 1
/* a second note of that name: the core heap the program asks for (u32
   bytes), MX_CORE_HEAP in <mx/loader.h> */
#define MXK_NOTE_HEAP 2
/* The oldest table libmxk runs on: what came later it uses only when the
   table's version has it (the pad from version 6), else its own code. */
#define MX_KERNEL_MIN 5

/* The kernel's side of what libmxk has no code for: virtual memory, heaps and
   the file server's client, with the same arguments (version 4). libmxk's
   stubs (ee/src/base/kstubs.c) and the table come from this list. */
#define MXK_CALLS(X)                                                                         \
  X(int, mx_vm_init, (const struct mx_vm_config *c), (c))                                    \
  X(int, mx_vm_active, (void), ())                                                           \
  X(u32, mx_vm_size, (void), ())                                                             \
  X(const void *, mx_vm_map_file, (const char *path, u32 *size), (path, size))               \
  X(void, mx_vm_unmap_file, (const void *p), (p))                                            \
  X(int, mx_vm_pin, (const void *p, u32 bytes, u32 flags), (p, bytes, flags))                \
  X(void, mx_vm_unpin, (const void *p, u32 bytes), (p, bytes))                               \
  X(void, mx_vm_overwrite_done, (const void *p, u32 bytes, u32 done), (p, bytes, done))      \
  X(void, mx_vm_unpin_after, (const void *p, u32 bytes, mx_event e), (p, bytes, e))          \
  X(u32, mx_vm_unpin_done, (void), ())                                                       \
  X(u32, mx_vm_pieces, (const void *p, u32 bytes, struct mx_vm_piece *pc, u32 max),          \
    (p, bytes, pc, max))                                                                     \
  X(struct mx_vm_piece, mx_vm_first_piece, (const void *p, u32 bytes), (p, bytes))           \
  X(void, mx_vm_stats, (struct mx_vm_stats * s), (s))                                        \
  X(void, mx_vm_trace_mark, (u32 tag), (tag))                                                \
  X(void, mx_vm_trace_restart, (void), ())                                                   \
  X(int, mx_vm_trace_save, (const char *path), (path))                                       \
  X(void *, mx_sbrk, (s32 incr), (incr))                                                     \
  X(struct mx_heap *, mx_heap_create, (const char *name, int kind, u32 max_bytes),           \
    (name, kind, max_bytes))                                                                 \
  X(void *, mx_heap_alloc, (struct mx_heap * h, u32 bytes, u32 align), (h, bytes, align))    \
  X(void, mx_heap_free, (struct mx_heap * h, void *p), (h, p))                               \
  X(void, mx_heap_reset, (struct mx_heap * h), (h))                                          \
  X(void, mx_heap_stats, (const struct mx_heap *h, u32 *used, u32 *held), (h, used, held))   \
  X(u32, mx_mem_run, (const void *p, u32 bytes), (p, bytes))                                 \
  X(int, mx_mem_lock_try, (const void *p, u32 bytes), (p, bytes))                            \
  X(void, mx_mem_prefetch, (const void *p, u32 bytes), (p, bytes))                           \
  X(void, mx_mem_cold, (const void *p, u32 bytes), (p, bytes))                               \
  X(int, mx_mem_resident, (const void *p, u32 bytes), (p, bytes))                            \
  X(void, mx_mem_pressure, (struct mx_mem_pressure * p), (p))                                \
  X(mx_paddr, mx__vm_paddr, (const void *p, u32 bytes), (p, bytes))                          \
  X(void, mx__vm_shutdown, (void), ())                                                       \
  X(void, mx__vm_swap_close, (void), ())                                                     \
  X(void, mx__vm_report, (void), ())                                                         \
  X(int, mx__vm_load_image, (const char *path), (path))                                      \
  X(int, mx_fio_open, (const char *path, int flags), (path, flags))                          \
  X(int, mx_fio_write, (int fd, const void *data, u32 n), (fd, data, n))                     \
  X(int, mx_fio_read, (int fd, void *data, u32 n), (fd, data, n))                            \
  X(int, mx_fio_lseek, (int fd, s32 offset, int whence), (fd, offset, whence))               \
  X(int, mx_fio_close, (int fd), (fd))                                                       \
  X(int, mx_fio_dopen, (const char *path), (path))                                           \
  X(int, mx_fio_dread, (int fd, char *name, u32 room, u32 *size), (fd, name, room, size))    \
  X(int, mx_fio_dclose, (int fd), (fd))                                                      \
  X(int, mx_fio_remove, (const char *path), (path))                                          \
  X(int, mx_fio_mkdir, (const char *path, int mode), (path, mode))                           \
  X(int, mx_fio_rmdir, (const char *path), (path))

void mx__vm_shutdown(void);
void mx__vm_swap_close(void);
void mx__vm_report(void);
int mx__vm_load_image(const char *path);

/* A pad's state (the kernel's pad, version 6): the buttons held (MX_PAD_*),
   the sticks as mx_pad_sticks gives them, analog 1 when they are real. */
struct mx_pad_state {
  u32 buttons;
  u8 sticks[4];
  u32 analog;
};
/* ee/src/base/pad.c's own reading of the pad driver, without the quit
   combination: libmx's, and the kernel's. read: 1 with a pad connected;
   sticks: read them (and ask for analog mode) too. */
int mx__pad_init(void);
void mx__pad_forget(void);
int mx__pad_read(int port, struct mx_pad_state *s, int sticks);
/* the motors of a SIO2 pad (a DualShock): small 0/1, big 0..255, sent when
   the pad takes them (from the next mx__pad_read); 0, or -1 with no pad */
int mx__pad_act(int port, u32 small, u32 big);

/* A program's interrupt slots (ee/src/irq/irq.c), for the kernel at its end:
   each slot's BIOS handler id (0: none) at id_offset (version 8). */
struct mxk_irq_tables {
  const void *intc, *dmac;
  u32 nintc, ndmac, slot_size, id_offset;
  /* version 9: the functions the slots are registered with (AddIntcHandler,
     AddDmacHandler), so the kernel can take them away while the program
     waits for one it started and add them again (docs/mx-os-plan.md, 2c) */
  const void *intc_entry, *dmac_entry;
};

struct mxk_services {
  u32 magic;   /* MX_KERNEL_MAGIC */
  u32 version; /* MX_KERNEL_VERSION of the kernel */
  u32 size;    /* sizeof this table in the kernel */

  /* program: the end of this one, the start of the next with the IOP kept,
     the swap file all programs share (0: none) */
  void (*exit)(int status) __attribute__((noreturn));
  /* returns the other program's status when the caller stays (version 9) */
  int (*launch)(int argc, const char *args, u32 args_len, const char *swap);
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

  /* version 4: the program's core memory after its end, its VM part (0, 0:
     none), the first thing it calls; the default heaps; a VM store that must
     not be, and the tty */
  void (*program)(void *heap_lo, void *heap_hi, void *vm_lo, void *vm_hi);
  void (*heaps)(struct mx_heap *h[3]);
  void (*mx__core_fault)(const void *p, const char *what);
  void (*mx__paddr_fault)(const void *p, u32 bytes) __attribute__((noreturn));
  int (*mx_tty_write)(const char *s, u32 n);
#define MXK_FIELD(ret, name, params, args) ret(*name) params;
  MXK_CALLS(MXK_FIELD)
#undef MXK_FIELD

  /* version 5: the end of a program's threads, none of them in a file call */
  void (*fio_ending)(void (*stop)(void));

  /* version 6: the pad is the kernel's (mx_pad_init, mx__pad_read with the
     kernel's pad settings applied) */
  int (*pad_init)(void);
  int (*pad_read)(int port, struct mx_pad_state *s, int sticks);

  /* version 7: the display and the home menu (docs/mx-os-plan.md, step 1).
     gs_display: the display registers (PMODE, DISPFB1, DISPLAY1, DISPFB2,
     DISPLAY2, BGCOLOR: MX_DISP_*) whose bit is set in mask, which the kernel
     writes and keeps (also from interrupt handlers); gs_mode: the video mode
     after SetGsCrt. home_wanted: the GS memory pages the home menu wants
     (0: none asked for), at a frame boundary; home_run: the menu, in the
     caller's thread, with the GS idle and pages page .. page + pages - 1 of
     GS memory lent to it (page ~0u: none, the menu is not shown). Returns
     1 when the program is to end, 0 to go on. */
  void (*gs_display)(const u64 *regs, u32 mask);
  void (*gs_mode)(const struct mx_video_mode *m);
  u32 (*home_wanted)(void);
  int (*home_run)(u32 page, u32 pages);

  /* version 8: a program ends without a restart (docs/mx-os-plan.md, 2a):
     the kernel is told where its interrupt slots are */
  void (*irq_tables)(const struct mxk_irq_tables *t);
  /* version 10: the kernel's threads, semaphores and interrupt handlers
     (docs/mx-os-plan.md, step 2d): maxsdk's mx_bios_* calls for them go
     there (src/base/ksys.c) */
  const struct mx_kthreads *kthreads;
  /* version 11: a pad's rumble (mx_pad_rumble): 0, or < 0 for a port with no
     pad that rumbles there */
  int (*pad_rumble)(int port, u32 small, u32 big);
  /* version 12: mx_loader_launch_beside, the caller's other threads going on
     beside the other program (docs/mx-os-apps-plan.md, step 2e-2) */
  int (*launch_beside)(int argc, const char *args, u32 args_len, const char *swap);
  /* version 13: processes and messages, <mx/proc.h>, <mx/msg.h>
     (docs/mx-os-apps-plan.md, step 2e-3) */
  int (*proc_self)(void);
  int (*proc_list)(struct mx_proc_info *p, int max);
  int (*port_create)(const char *name);
  int (*port_find)(const char *name);
  int (*port_delete)(int port);
  int (*msg_send)(int port, const void *req, u32 len, void *reply, u32 max);
  int (*msg_receive)(int port, void *buf, u32 max, struct mx_msg *m);
  int (*msg_reply)(int msg, const void *reply, u32 len);
  /* version 14: <mx/proc.h>'s spawn, wait and kill (step 2e-3) */
  int (*proc_spawn)(int argc, const char *args, u32 args_len, const char *swap);
  int (*proc_wait)(int id, int *status, int nohang);
  int (*proc_kill)(int id, int status);
  /* mx_sleep_us: timer 0 is the kernel's, one for all processes */
  void (*sleep_us)(u32 us);
  /* version 15: the kernel's events to a port of the process (<mx/proc.h>) */
  int (*proc_events)(int port);
  /* version 16: the kernel's log (<mx/log.h>) */
  int (*log_read)(u32 *pos, void *buf, u32 max, int *proc, int wait);
  /* version 17: an IOP module's SIF commands to a port (<mx/iop.h>) */
  int (*iop_channel)(u32 cid, int port);
  /* version 18: the caller a service beside every program in front */
  int (*proc_service)(void);
};
/* the size of a table of version MX_KERNEL_MIN */
#define MXK_MIN_SIZE __builtin_offsetof(struct mxk_services, pad_init)

/* The home menu at the frame boundary (ee/src/gs/home.c): called by
   mx_frame_begin_n. */
void mx__home_poll(void);

/* libmxk's start: the program's interrupt slots to the kernel (irq.c). */
void mx__irq_tell_kernel(void);
/* The kernel at a program's end (2a): what the program left in the SIF
   layer, a client whose semaphore is gone, and the VM's vblank function. */
int mx__sif_calls_finish(void);
/* the running process's ASID (src/vm/vm.c; docs/mx-os-plan.md, step 2b) */
extern u32 mx__asid;
void mx__asid_set(u32 asid);
/* the VM's frames: this physical memory (a program's core at a run of its own) */
void mx__vm_frames(u32 phys_lo, u32 phys_hi);
void mx__sif_program_end(void);
struct mx_sif_client;
void mx__sif_client_check(struct mx_sif_client *c);
void mx__irq_vbon_unhook(void (*fn)(void));
void mx__vm_unhook(void);
/* the kernel's mx_sleep_us (src/thread/sleep.c): its semaphores and handler
   at its start, the slots of a program's threads freed at its end */
void mx__sleep_init(void);
void mx__sleep_reap(void);

/* The kernel's side of sif_call (ee/src/base/sif.c). */
int mx__sif_call(struct mx_sif_client *c, int fno, const void *send, u32 ssize, void *recv,
                 u32 rsize, u32 wait);

/* The kernel's side: the program's memory, for the VM (heap.c, image.c), and
   the file lock given up in a crash (sif.c). */
void mx__heap_range(void *lo, void *hi);
void mx__vm_image_range(void *lo, void *hi);
void mx__fio_crash(void);
extern u32 mx__sif_wait_limit, mx__sif_timeouts;
void mx__fio_ending(void (*stop)(void));

/* The table, 0 without a kernel. */
extern const struct mxk_services *mx__kernel;

#endif
