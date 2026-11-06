#ifndef MX_KERNEL_H
#define MX_KERNEL_H

/* The services of mx-kernel (tools/mxlink, resident below 1 MiB), for libmxk:
   docs/kernel.md. Programs do not use this header: they call the same
   functions in either library, libmx (no kernel) or libmxk (MX_MEMORY=vm).

   The loader passes a pointer to this table. Rules: functions are only added
   at the end, with a new version; libmxk reads a table of at least its own
   version, which it checks with size. */

#include <mx/debug.h>
#include <mx/mem.h>
#include <mx/sif.h>
#include <mx/types.h>
#include <mx/vm.h>

#define MX_KERNEL_MAGIC 0x4c4e524bu /* "KRNL" */
#define MX_KERNEL_VERSION 4

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
};

/* The kernel's side of sif_call (ee/src/base/sif.c). */
int mx__sif_call(struct mx_sif_client *c, int fno, const void *send, u32 ssize, void *recv,
                 u32 rsize, u32 wait);

/* The kernel's side: the program's memory, for the VM (heap.c, image.c), and
   the file lock given up in a crash (sif.c). */
void mx__heap_range(void *lo, void *hi);
void mx__vm_image_range(void *lo, void *hi);
void mx__fio_crash(void);

/* The table, 0 without a kernel. */
extern const struct mxk_services *mx__kernel;

#endif
