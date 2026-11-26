/* A program's sockets stay open while its child runs and after the child
   ends (kernel version 51): the kernel closes what the ending program left
   open, not the sockets of a parent that waits for it (mxsock knows each
   socket's process). This one opens a UDP socket, starts itself as the
   child, which opens sockets of its own and ends leaving them open, then
   asks for its socket again (a closed one answers -9).
     k93_sockets.elf     (status 0 expected; on the PS2, with the network;
                          without one it says so and passes) */
#include "t.h"
#include <mx/kernel.h>
#include <mx/net.h>
#include <mx/proc.h>

int main(int argc, char **argv) {
  if (argc > 1) { /* the child: sockets of its own, left open */
    for (int i = 0; i < 2; i++)
      if (mx_net_socket(MX_NET_UDP) < 0)
        return 9;
    return 5;
  }
  CHECK(mx__kernel && mx__kernel->version >= 51);
  int s = mx_net_socket(MX_NET_UDP);
  if (s < 0) {
    mx_printf("k93: no sockets (%d): nothing to test\n", s);
    return t_done("k93");
  }
  CHECK(mx_net_bind(s, 18301) == 0);
  CHECK(mx_net_poll(s, MX_NET_WRITE) >= 0);
  for (int round = 0; round < 3; round++) {
    static const char args[] = "k93_sockets.elf\0child";
    int es = -1, id = mx_proc_spawn(2, args, sizeof args, 0);
    CHECK(id > 0 && mx_proc_wait(id, &es, 0) == 1 && es == 5);
    int p = mx_net_poll(s, MX_NET_WRITE);
    mx_printf("k93: round %d: the socket answers %d\n", round, p);
    CHECK(p >= 0);
  }
  CHECK(mx_net_close(s) == 0);
  return t_done("k93");
}
