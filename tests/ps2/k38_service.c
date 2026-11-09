/* Step 2e-4 (docs/mx-os-apps-plan.md): a service (kernel version 18). The
   kernel starts this program first (mc0:/MXOS/SERVICES.CFG; on PCSX2
   host:services.txt); it becomes the service (mx_proc_service) and the
   program in front starts (host:mxload.txt's: vm34_child exit2). It gets
   that one's end as an event, starts the next in front itself (spawn), ends
   it (kill), and as the service prints the test's end marker itself. On
   PCSX2 (the runner's MXSERVICE and MXLOAD):
     services.txt: host:k38_service.elf host:vm34_child.elf
     mxload.txt:   host:vm34_child.elf exit2      (status 0 expected) */
#include "t.h"
#include <mx/loader.h>
#include <mx/msg.h>
#include <mx/proc.h>
#include <mx/string.h>

MX_CORE_HEAP(512 << 10);

static int port;

static int end_of(int id) {
  struct mx_event e;
  struct mx_msg m;
  for (;;) {
    memset(&e, 0, sizeof e);
    if (mx_msg_receive(port, &e, sizeof e, &m) < 0)
      return -1000;
    if (m.from == 0 && e.type == MX_EVENT_END && e.proc == id)
      return e.status;
    mx_printf("k38: another message: from %d, type %u, process %d\n", m.from, e.type, e.proc);
  }
}

static int spawn(const char *path, const char *arg) {
  static char args[256];
  u32 n = (u32)strlen(path) + 1;
  memcpy(args, path, n);
  memcpy(args + n, arg, strlen(arg) + 1);
  n += (u32)strlen(arg) + 1;
  return mx_proc_spawn(2, args, n, 0);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    mx_printf("k38: usage: k38_service.elf <vm34_child.elf>\n");
    return t_done("k38");
  }
  port = mx_port_create("k38");
  CHECK(port > 0);
  CHECK_EQ(mx_proc_events(port), 0);

  int front = mx_proc_service();
  mx_printf("k38: a service now; in front: process %d\n", front);
  CHECK(front > 0);
  struct mx_proc_info p[4];
  int n = mx_proc_list(p, 4), beside = 0;
  for (int i = 0; i < n && i < 4; i++)
    beside += p[i].id == mx_proc_self() && p[i].state == MX_PROC_BESIDE;
  CHECK_EQ(beside, 1);
  CHECK_EQ(mx_proc_service(), -1); /* once */
  CHECK_EQ(end_of(front), 2);

  /* nothing in front now (no shell on PCSX2): this one starts the next */
  int id = spawn(argv[1], "forever");
  mx_printf("k38: started process %d\n", id);
  CHECK(id > 0);
  CHECK_EQ(spawn(argv[1], "exit1"), -1); /* one runs */
  CHECK_EQ(mx_proc_kill(id, 9), 0);
  CHECK_EQ(end_of(id), 9);

  id = spawn(argv[1], "exit6");
  CHECK(id > 0);
  int st = -1;
  CHECK_EQ(mx_proc_wait(id, &st, 0), 1);
  CHECK_EQ(st, 6);
  CHECK_EQ(end_of(id), 6);
  CHECK_EQ(spawn("host:no-such.elf", "x"), -1);

  /* the kernel prints no marker while a service is beside: this one does */
  int failed = t_done("k38");
  mx_printf("@@PS2TEST_END@@ status=%d\n", failed);
  for (;;)
    mx_msg_receive(port, &st, sizeof st, 0);
}
