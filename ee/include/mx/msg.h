#ifndef MX_MSG_H
#define MX_MSG_H

/* Messages between processes (kernel version 13; docs/kernel.md,
   docs/mx-os-apps-plan.md step 2e-3). A process makes a named port and
   receives on it; any process finds it by its name and sends: the sender
   waits for the reply. The kernel copies both, from one address space to the
   other, up to MX_MSG_MAX bytes each; MX_MSG_SLOTS messages are under way at
   once (a sender waits for a free one). Buffers may be anywhere in the
   caller's memory, VM too. libmxk only: libmx's calls return -1. */

#include <mx/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MX_MSG_MAX 256
#define MX_MSG_SLOTS 4
#define MX_PORT_NAME 16 /* with its 0 */

struct mx_msg {
  u32 len;  /* the request's length (more than was copied: cut off) */
  int from; /* the sender's process */
};

/* a port of this process: its id (> 0); -1 when the name is taken, too long
   or there is no room. It goes with the process's end. */
int mx_port_create(const char *name);
/* the port of that name, -1 if there is none */
int mx_port_find(const char *name);
/* the port gone: its waiting senders get -2 */
int mx_port_delete(int port);

/* send len bytes to port and wait for the reply, at most max bytes into
   reply: the reply's length; -1 no such port or len over MX_MSG_MAX, -2 the
   port went away before it replied */
int mx_msg_send(int port, const void *req, u32 len, void *reply, u32 max);
/* the next request on a port of this process, waited for: at most max bytes
   into buf, what it is in m; its id for mx_msg_reply (> 0), -1 not a port of
   this process, -2 the port went away */
int mx_msg_receive(int port, void *buf, u32 max, struct mx_msg *m);
/* the reply to a request received (at most the sender's max of it is
   copied): 0, or -1 for an id that is not waiting for a reply */
int mx_msg_reply(int msg, const void *reply, u32 len);

#ifdef __cplusplus
}
#endif

#endif
