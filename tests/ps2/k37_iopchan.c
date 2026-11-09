/* Step 2e-3 (docs/mx-os-apps-plan.md): IOP channels (kernel version 17).
   This program (libmxk without VM) gives cid 7 to a port of its own, loads
   tests/iop/chantest (embedded), which sends three SIF commands with cid 7,
   and receives them on the port: from process 0, the payload without the
   header. Loading a module, it ends with the IOP reset. Started by mxlink:
     k37_iopchan.elf     (status 0 expected) */
#include "t.h"
#include <mx/iop.h>
#include <mx/loader.h>
#include <mx/msg.h>
#include <mx/string.h>

MX_CORE_HEAP(256 << 10);

static const u8 irx[] MX_ALIGNED(64) = {
#embed <chantest.irx>
};

int main(void) {
  int port = mx_port_create("k37"), other = mx_port_create("k37b");
  CHECK(port > 0 && other > 0);
  CHECK_EQ(mx_iop_channel(32, port), -1); /* not a user cid */
  CHECK_EQ(mx_iop_channel(7, port + 5), -1); /* not its port */
  CHECK_EQ(mx_iop_channel(7, other), 0);
  CHECK_EQ(mx_iop_channel(7, port), 0); /* moved to another of its own */
  CHECK_EQ(mx_iop_channel(9, other), 0);
  CHECK_EQ(mx_iop_channel(9, 0), 0); /* given up */

  int res = -1;
  int id = mx_iop_load_module_buffer(irx, sizeof irx, 0, 0, &res);
  mx_printf("k37: chantest loaded: %d, %d\n", id, res);
  CHECK(id > 0);

  u32 got = 0;
  for (u32 i = 1; i <= 3; i++) {
    struct {
      u32 n;
      char text[12];
    } b;
    struct mx_msg m;
    memset(&b, 0, sizeof b);
    int r = mx_msg_receive(port, &b, sizeof b, &m);
    mx_printf("k37: message %d: from %d, %u bytes, %u %.8s\n", r, m.from, m.len, b.n, b.text);
    got += r > 0 && m.from == 0 && m.len == 16 && b.n == i && !memcmp(b.text, "chantest", 8);
  }
  CHECK_EQ(got, 3);
  return t_done("k37");
}
