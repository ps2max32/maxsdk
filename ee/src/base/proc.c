/* Processes, messages and the log (<mx/proc.h>, <mx/msg.h>, <mx/log.h>): the kernel's, from
   version 13 and 14 (docs/kernel.md). libmx and the kernel's own library have none:
   -1. */
#include <mx/debug.h>
#include <mx/dev.h>
#include <mx/notify.h>
#include <mx/iop.h>
#include <mx/kernel.h>
#include <mx/log.h>
#include <mx/msg.h>
#include <mx/proc.h>
#include <mx/setting.h>
#include <mx/sif.h>

#ifdef MX_KERNEL
#define HAS(v, f)                                                                          \
  (mx__kernel && mx__kernel->version >= (v) &&                                             \
   mx__kernel->size >= __builtin_offsetof(struct mxk_services, f) + sizeof(void *) &&      \
   mx__kernel->f)
#define CALLV(v, f, args) (HAS(v, f) ? mx__kernel->f args : -1)
#else
#define CALLV(v, f, args) -1
#endif
#define CALL(f, args) CALLV(13, f, args)

int mx_proc_self(void) { return CALL(proc_self, ()); }
int mx_proc_list(struct mx_proc_info *p, int max) { return CALL(proc_list, (p, max)); }
int mx_proc_memory(struct mx_proc_mem *m, int max) { return CALLV(38, proc_memory, (m, max)); }
int mx_port_create(const char *name) { return CALL(port_create, (name)); }
int mx_port_find(const char *name) { return CALL(port_find, (name)); }
int mx_port_delete(int port) { return CALL(port_delete, (port)); }
int mx_msg_send(int port, const void *req, u32 len, void *reply, u32 max) {
  return CALL(msg_send, (port, req, len, reply, max));
}
int mx_msg_receive(int port, void *buf, u32 max, struct mx_msg *m) {
  return CALL(msg_receive, (port, buf, max, m));
}
int mx_msg_reply(int msg, const void *reply, u32 len) { return CALL(msg_reply, (msg, reply, len)); }

/* the frame pipeline's, if the program has it: its stream drawn before
   another process is in front (src/gs/frame.c) */
extern void mx__frame_yield(void) __attribute__((weak));
/* the sound is the process in front's too (mx OS plan step 5): its voices
   and stream silenced at a handover (src/base/snd.c, with sound) */
extern void mx__snd_quiet(void) __attribute__((weak));
static void yield(void) {
  if (mx__frame_yield)
    mx__frame_yield();
  if (mx__snd_quiet)
    mx__snd_quiet();
}
int mx_proc_spawn(int argc, const char *args, u32 args_len, const char *swap) {
  mx_debug_flush();
  yield();
  return CALLV(14, proc_spawn, (argc, args, args_len, swap));
}
int mx_proc_spawn_in(const char *dir, int argc, const char *args, u32 args_len, const char *swap) {
  mx_debug_flush();
  yield();
  return CALLV(40, proc_spawn_in, (dir, argc, args, args_len, swap));
}
#ifdef MX_KERNEL
/* kernel version 40: the working directory is the kernel's, for each process
   (libmx's own: src/base/sif.c) */
int mx_fio_chdir(const char *path) { return CALLV(40, fio_chdir, (path)); }
int mx_fio_getcwd(char *buf, u32 room) { return CALLV(40, fio_getcwd, (buf, room)); }
#endif
int mx_proc_wait(int id, int *status, int nohang) {
  return CALLV(14, proc_wait, (id, status, nohang));
}
int mx_proc_kill(int id, int status) { return CALLV(14, proc_kill, (id, status)); }
int mx_proc_front(int id) {
  yield();
  return CALLV(24, proc_front, (id));
}
int mx_proc_events(int port) { return CALLV(15, proc_events, (port)); }
int mx_log_read(u32 *pos, void *buf, u32 max, int *proc, int wait) {
  return CALLV(16, log_read, (pos, buf, max, proc, wait));
}
int mx_iop_channel(u32 cid, int port) { return CALLV(17, iop_channel, (cid, port)); }
int mx_proc_service(void) {
  mx_debug_flush();
  return CALLV(18, proc_service, ());
}

int mx_phys_read(u32 phys, void *to, u32 n) { return CALLV(21, phys_read, (phys, to, n)); }
int mx_phys_write(u32 phys, const void *from, u32 n) {
  return CALLV(21, phys_write, (phys, from, n));
}

/* kernel version 26: devices, the swap file, notifications (<mx/dev.h>,
   <mx/notify.h>) */
int mx_dev_list(struct mx_dev_info *d, int max) { return CALLV(26, dev_list, (d, max)); }
u32 mx_dev_gen(void) {
#ifdef MX_KERNEL
  if (HAS(26, dev_gen))
    return mx__kernel->dev_gen();
#endif
  return 0;
}
int mx_dev_eject(const char *name) { return CALLV(26, dev_eject, (name)); }
int mx_swap_set(const char *path, u32 mib) { return CALLV(26, swap_set, (path, mib)); }
int mx_swap_info(struct mx_swap_info *s) { return CALLV(26, swap_info, (s)); }
int mx_notify(u32 kind, int urgent, const char *title, const char *text) {
  return CALLV(26, notify, (kind, urgent, title, text));
}
int mx_notify_read(u32 after_seq, struct mx_notify *n, int max) {
  return CALLV(26, notify_read, (after_seq, n, max));
}
u32 mx_notify_seq(void) {
#ifdef MX_KERNEL
  if (HAS(26, notify_seq))
    return mx__kernel->notify_seq();
#endif
  return 0;
}

int mx_setting_get(const char *key, char *value, u32 size) {
  return CALLV(27, setting_get, (key, value, size));
}
int mx_setting_set(const char *key, const char *value) {
  return CALLV(27, setting_set, (key, value));
}
int mx_setting_list(u32 after_seq, struct mx_setting *s, int max) {
  int n = CALLV(27, setting_list, (after_seq, s, max));
  return n < 0 ? 0 : n;
}
u32 mx_setting_seq(void) {
#ifdef MX_KERNEL
  if (HAS(27, setting_seq))
    return mx__kernel->setting_seq();
#endif
  return 0;
}
