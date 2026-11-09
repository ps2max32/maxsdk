/* Processes, messages and the log (<mx/proc.h>, <mx/msg.h>, <mx/log.h>): the kernel's, from
   version 13 and 14 (docs/kernel.md). libmx and the kernel's own library have none:
   -1. */
#include <mx/debug.h>
#include <mx/kernel.h>
#include <mx/log.h>
#include <mx/msg.h>
#include <mx/proc.h>

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

int mx_proc_spawn(int argc, const char *args, u32 args_len, const char *swap) {
  mx_debug_flush();
  return CALLV(14, proc_spawn, (argc, args, args_len, swap));
}
int mx_proc_wait(int id, int *status, int nohang) {
  return CALLV(14, proc_wait, (id, status, nohang));
}
int mx_proc_kill(int id, int status) { return CALLV(14, proc_kill, (id, status)); }
int mx_proc_events(int port) { return CALLV(15, proc_events, (port)); }
int mx_log_read(u32 *pos, void *buf, u32 max, int *proc, int wait) {
  return CALLV(16, log_read, (pos, buf, max, proc, wait));
}
