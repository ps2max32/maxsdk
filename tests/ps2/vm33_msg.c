/* Step 2e-3 (docs/mx-os-apps-plan.md): messages between two processes, the
   one in front (linked into VM) and k32_port beside it: the process list,
   the port found by its name, requests of every length from VM and from
   core, the replies checked. Its own port "vm33": it receives k32's request
   and ends without a reply (k32 gets -2), and the port goes with its end
   (k32_port checks). */
#include "t.h"
#include <mx/msg.h>
#include <mx/proc.h>
#include <mx/string.h>

static u8 vm_req[MX_MSG_MAX + 16], vm_rep[MX_MSG_MAX];
static u8 core_req[MX_MSG_MAX] MX_CORE_DATA, core_rep[MX_MSG_MAX] MX_CORE_DATA;

static int has(const char *s, const char *w) {
  for (u32 n = (u32)strlen(w); *s; s++)
    if (!memcmp(s, w, n))
      return 1;
  return 0;
}

static u32 check(int port, const u8 *req, u8 *rep, u32 len, int self) {
  memset(rep, 0xEE, MX_MSG_MAX);
  int r = mx_msg_send(port, req, len, rep, MX_MSG_MAX);
  u32 want = len > MX_MSG_MAX - 4 ? MX_MSG_MAX : len + 4, bad = r != (int)want;
  int from;
  memcpy(&from, rep, 4);
  bad += from != self;
  for (u32 i = 0; i + 4 < want; i++)
    bad += rep[4 + i] != (u8)(req[i] + 1);
  return bad;
}

int main(void) {
  int self = mx_proc_self();
  CHECK(self > 0);
  struct mx_proc_info p[4];
  int n = mx_proc_list(p, 4);
  mx_printf("vm33: process %d; %d processes:\n", self, n);
  int front = 0, beside = 0;
  for (int i = 0; i < n && i < 4; i++) {
    mx_printf("vm33:   %u %s, core %08x-%08x, %s\n", p[i].id,
              p[i].state == MX_PROC_FRONT    ? "front"
              : p[i].state == MX_PROC_BESIDE ? "beside"
                                             : "waiting",
              p[i].core_lo, p[i].core_hi, p[i].name);
    front += p[i].state == MX_PROC_FRONT && p[i].id == self && has(p[i].name, "vm33");
    beside += p[i].state == MX_PROC_BESIDE && p[i].id != self && has(p[i].name, "k32");
  }
  CHECK_EQ(n, 2);
  CHECK_EQ(front, 1);
  CHECK_EQ(beside, 1);

  int port = mx_port_find("k32");
  CHECK(port > 0);
  CHECK_EQ(mx_port_find("nope"), -1);
  CHECK_EQ(mx_port_create("k32"), -1);
  CHECK_EQ(mx_msg_receive(port, vm_req, sizeof vm_req, 0), -1); /* not its port */
  CHECK_EQ(mx_msg_send(port, vm_req, MX_MSG_MAX + 1, vm_rep, sizeof vm_rep), -1);
  int mine = mx_port_create("vm33"); /* left to its end */
  CHECK(mine > 0);

  u32 bad = 0, sent = 0;
  for (u32 round = 0; round < 2; round++)
    for (u32 len = 0; len <= MX_MSG_MAX; len += len < 16 ? 1 : 37) {
      for (u32 i = 0; i < len; i++)
        vm_req[i] = (u8)(i * 7 + len + round), core_req[i] = (u8)(i * 13 + len);
      bad += check(port, vm_req, vm_rep, len, self);
      bad += check(port, core_req, core_rep, len, self);
      sent += 2;
    }
  /* a reply cut to the sender's room */
  memset(vm_rep, 0, sizeof vm_rep);
  int r = mx_msg_send(port, vm_req, 100, vm_rep, 10);
  mx_printf("vm33: %u requests, %u bytes wrong; a reply into 10 bytes: %d\n", sent, bad, r);
  CHECK_EQ(bad, 0);
  CHECK_EQ(r, 10);
  CHECK_EQ(vm_rep[10], 0);
  /* k32's request, not answered */
  struct mx_msg m;
  char hi[8] = {0};
  int id = mx_msg_receive(mine, hi, sizeof hi, &m);
  mx_printf("vm33: request %d on its port from process %d: %.2s; ending without a reply\n", id,
            m.from, hi);
  CHECK(id > 0);
  CHECK(m.len == 2 && hi[0] == 'h' && hi[1] == 'i');
  return t_done("vm33");
}
