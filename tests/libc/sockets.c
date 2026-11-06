/* BSD sockets (ee/compat/net) on the PS2, against probe/iop/sock_host.py: UDP
   echo, TCP echo, select and non-blocking calls.  sockets <a.b.c.d of the host> */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

int close(int fd);

static int fails;
#define CHECK(c)                                                                           \
  do {                                                                                     \
    if (!(c)) {                                                                            \
      printf("sockets: FAIL line %d: %s, errno %d\n", __LINE__, #c, errno);                \
      fails++;                                                                             \
    }                                                                                      \
  } while (0)

int main(int argc, char **argv) {
  static char buf[4096];
  struct sockaddr_in a, from;
  socklen_t len = sizeof from;
  struct hostent *h = argc > 1 ? gethostbyname(argv[1]) : 0;
  CHECK(h);
  if (!h)
    return 1;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  memcpy(&a.sin_addr, h->h_addr, 4);

  /* UDP: a sent datagram comes back, select says so first */
  int u = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(u >= 0);
  a.sin_port = htons(18770);
  CHECK(sendto(u, "ping", 4, 0, (struct sockaddr *)&a, sizeof a) == 4);
  fd_set set;
  FD_ZERO(&set);
  FD_SET(u, &set);
  struct timeval tv = {3, 0};
  CHECK(select(u + 1, &set, 0, 0, &tv) == 1 && FD_ISSET(u, &set));
  CHECK(recvfrom(u, buf, sizeof buf, 0, (struct sockaddr *)&from, &len) == 4);
  CHECK(!memcmp(buf, "ping", 4) && ntohs(from.sin_port) == 18770);

  /* non-blocking: nothing there */
  CHECK(fcntl(u, F_SETFL, O_NONBLOCK) == 0);
  CHECK(recv(u, buf, sizeof buf, 0) < 0 && errno == EAGAIN);
  FD_ZERO(&set);
  FD_SET(u, &set);
  tv.tv_sec = 0, tv.tv_usec = 100000;
  CHECK(select(u + 1, &set, 0, 0, &tv) == 0);
  CHECK(close(u) == 0);
  CHECK(recv(u, buf, 4, 0) < 0 && errno == EBADF);

  /* TCP: blocking */
  int t = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(t >= 0);
  a.sin_port = htons(18771);
  CHECK(connect(t, (struct sockaddr *)&a, sizeof a) == 0);
  for (int i = 0; i < 20; i++) {
    memset(buf, 'a' + i, 3000);
    CHECK(send(t, buf, 3000, 0) == 3000);
    int got = 0;
    while (got < 3000) {
      int n = recv(t, buf + got, 3000 - got, 0);
      CHECK(n > 0);
      if (n <= 0)
        break;
      got += n;
    }
    CHECK(buf[0] == 'a' + i && buf[2999] == 'a' + i);
  }
  CHECK(close(t) == 0);
  CHECK(socket(AF_UNSPEC, SOCK_DGRAM, 0) < 0);
  printf("sockets: %s\n", fails ? "FAILED" : "passed");
  printf("@@PS2TEST_END@@ status=%d\n", fails != 0);
  return fails != 0;
}
