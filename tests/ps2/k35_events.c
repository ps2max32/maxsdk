/* Step 2e-3 (docs/mx-os-apps-plan.md): the kernel's events (kernel version
   15). This program (libmxk without VM, its core at a run of its own) has
   its events sent to a port of its own, starts vm34_child (mx_proc_spawn)
   and waits on that port: the end comes as an event from process 0 with
   the exit status (5). Then the same for one it ends itself (kill, 9).
   Started by mxlink:
     k35_events.elf host:vm34_child.elf     (status 0 expected) */
#include "t.h"
#include <mx/loader.h>
#include <mx/msg.h>
#include <mx/proc.h>
#include <mx/string.h>

MX_CORE_HEAP(512 << 10);

static int spawn(const char *path, const char *arg) {
  static char args[256];
  u32 n = (u32)strlen(path) + 1;
  memcpy(args, path, n);
  memcpy(args + n, arg, strlen(arg) + 1);
  n += (u32)strlen(arg) + 1;
  return mx_proc_spawn(2, args, n, 0);
}

static void event(int port, int id, int status) {
  struct mx_event e;
  struct mx_msg m;
  memset(&e, 0, sizeof e);
  int r = mx_msg_receive(port, &e, sizeof e, &m);
  mx_printf("k35: event %d: type %u, process %d, status %d (from %d, %u bytes)\n", r, e.type,
            e.proc, e.status, m.from, m.len);
  CHECK(r > 0);
  CHECK_EQ(m.from, 0);
  CHECK_EQ(m.len, sizeof e);
  CHECK_EQ(e.type, MX_EVENT_END);
  CHECK_EQ(e.proc, id);
  CHECK_EQ(e.status, status);
  CHECK_EQ(mx_msg_reply(r, 0, 0), -1); /* events want none */
}

int main(int argc, char **argv) {
  if (argc < 2) {
    mx_printf("k35: usage: k35_events.elf <vm34_child.elf>\n");
    return t_done("k35");
  }
  int port = mx_port_create("k35");
  CHECK(port > 0);
  CHECK_EQ(mx_proc_events(port + 1), -1); /* not a port of its own */
  CHECK_EQ(mx_proc_events(port), 0);

  int id = spawn(argv[1], "exit5");
  CHECK(id > 0);
  event(port, id, 5);

  id = spawn(argv[1], "forever");
  CHECK(id > 0);
  CHECK_EQ(mx_proc_kill(id, 9), 0);
  event(port, id, 9);

  int st = -1;
  CHECK_EQ(mx_proc_wait(id, &st, 1), 1);
  CHECK_EQ(st, 9);
  CHECK_EQ(mx_proc_events(0), 0);
  return t_done("k35");
}
