/* Step 2e-3 (docs/mx-os-apps-plan.md): messages between two processes. This
   one (libmxk without VM, its core at a run of its own) makes the port
   "k32" and serves it in a thread of its own while the other runs beside it
   (mx_loader_launch_beside): each request comes back with every byte + 1,
   after a word with the sender's process. A second thread sends to the
   other's port "vm33", which receives it and ends without a reply: -2.
   Then: the other's port went with its end. Started by mxlink:
     k32_port.elf host:vm33_msg.elf     (status 0 expected) */
#include "t.h"
#include <mx/loader.h>
#include <mx/msg.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/thread.h>

MX_CORE_HEAP(512 << 10);

static int port;
static volatile u32 served, bad_from, stop;
static int other = -1;
/* stacks: core (a VM program's too) */
static u8 stack[16 << 10] MX_ALIGNED(16) MX_CORE_DATA, stack2[8 << 10] MX_ALIGNED(16) MX_CORE_DATA;
static volatile int asked = 1, asked_done;

/* to the other's port, once it is there: it ends without a reply */
static void client(void *arg) {
  (void)arg;
  int p = -1;
  for (int i = 0; i < 2000 && p < 0; i++) {
    mx_sleep_us(1000);
    p = mx_port_find("vm33");
  }
  char rep[8];
  asked = p < 0 ? -100 : mx_msg_send(p, "hi", 2, rep, sizeof rep);
  asked_done = 1;
}

static void server(void *arg) {
  (void)arg;
  static u8 req[MX_MSG_MAX], rep[MX_MSG_MAX];
  for (;;) {
    struct mx_msg m;
    int id = mx_msg_receive(port, req, sizeof req, &m);
    if (id < 0)
      break; /* the port went (its delete at the end) */
    if (m.len == 4 && !memcmp(req, "stop", 4)) {
      mx_msg_reply(id, "ok", 2);
      continue;
    }
    if (other < 0)
      other = m.from;
    bad_from += m.from != other || m.from == mx_proc_self();
    u32 n = m.len > MX_MSG_MAX - 4 ? MX_MSG_MAX - 4 : m.len;
    memcpy(rep, &m.from, 4);
    for (u32 i = 0; i < n; i++)
      rep[4 + i] = (u8)(req[i] + 1);
    mx_msg_reply(id, rep, 4 + n);
    served++;
  }
  stop = 1;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    mx_printf("k32: usage: k32_port.elf <program> [args]\n");
    return t_done("k32");
  }
  int self = mx_proc_self();
  CHECK(self > 0);
  port = mx_port_create("k32");
  CHECK(port > 0);
  CHECK_EQ(mx_port_create("k32"), -1); /* taken */
  CHECK_EQ(mx_port_find("k32"), port);
  CHECK(mx_thread_start(server, 0, stack, sizeof stack, 60) >= 0);
  CHECK(mx_thread_start(client, 0, stack2, sizeof stack2, 60) >= 0);

  static char args[256];
  u32 n = 0;
  for (int i = 1; i < argc; i++) {
    u32 l = (u32)strlen(argv[i]) + 1;
    if (n + l > sizeof args)
      break;
    memcpy(args + n, argv[i], l);
    n += l;
  }
  mx_printf("k32: process %d, port %d; starting %s beside\n", self, port, argv[1]);
  int st = mx_loader_launch_beside(argc - 1, args, n, 0);
  /* the other's port went with it */
  int gone = mx_port_find("vm33");
  CHECK(mx_port_delete(port) == 0);
  for (int i = 0; i < 100 && !stop; i++)
    mx_thread_yield();
  for (int i = 0; i < 100 && !asked_done; i++)
    mx_thread_yield();
  mx_printf("k32: back, status %d; %u requests served (from process %d), port vm33 %d, "
            "its unanswered request %d\n", st, served, other, gone, asked);
  CHECK_EQ(st, 0);
  CHECK(served >= 90); /* vm33 sends 93 */
  CHECK_EQ(bad_from, 0);
  CHECK_EQ(gone, -1);
  CHECK_EQ(asked, -2);
  CHECK(stop);
  return t_done("k32");
}
