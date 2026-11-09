/* Processes and messages (<mx/proc.h>, <mx/msg.h>): the kernel's, from
   version 13 (docs/kernel.md). libmx and the kernel's own library have none:
   -1. */
#include <mx/kernel.h>
#include <mx/msg.h>
#include <mx/proc.h>

#ifdef MX_KERNEL
static int has13(void) {
  return mx__kernel && mx__kernel->version >= 13 &&
         mx__kernel->size >= __builtin_offsetof(struct mxk_services, msg_reply) + sizeof(void *);
}
#define CALL(f, args) (has13() && mx__kernel->f ? mx__kernel->f args : -1)
#else
#define CALL(f, args) -1
#endif

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
