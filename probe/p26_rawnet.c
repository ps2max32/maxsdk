/* Starts probe/iop/rawnet.c: reads the IOP modules given as host:<file>, in
   the order to load them (poweroff, ps2dev9, netman, smap, rawnet), resets
   the IOP and loads them. The reset ends host: and the console, so it prints
   nothing after it: rawnet_host.py gets the results, over UDP. When rawnet
   ends, by the host or its deadline, this program ends and mxlink resets the
   IOP, as after any program that changes it.

   The last module gets the address of a ring for frames sent by SIF DMA. Its
   frames are checked at the end: the exit status (ps2client status) is 0 when
   all slots hold the host's pattern, 10 + the bad ones if not, 9 if none came.

   An argument ip=<ip>,<mask>,<gateway> goes to the next module, for a TCP/IP
   stack (net.c): for its probes this program only stands in for mxlink. */

#include <mx/iop.h>
#include <mx/mx.h>
#include <mx/sif.h>

#define MAX 8
#define SLOTS 16
#define SLOT 2048u
#define HDR 42u
#define DATA 1472u

static u8 mem[512 * 1024] MX_ALIGNED(64);
static struct {
  u8 *at;
  u32 size;
  char arg[64];
  u32 arg_len;
} mod[MAX];

static u8 ring[SLOTS * SLOT] MX_ALIGNED(64);

/* the bad slots of the ring; 0 with none filled is reported by the caller */
static int check(int *filled) {
  int bad = 0;
  *filled = 0;
  for (u32 s = 0; s < SLOTS; s++) {
    const volatile u8 *f = MX_UNCACHED(ring + s * SLOT);
    if (!f[HDR])
      continue;
    (*filled)++;
    int ok = f[HDR] == 'D';
    for (u32 i = 1; i < DATA; i++)
      ok &= f[HDR + i] == (u8)(i * 7);
    bad += !ok;
  }
  return bad;
}

int main(int argc, char **argv) {
  u8 *to = mem;
  int n = 0;
  if (mx_sif_init() < 0) {
    MX_TEST_END(1);
    return 1;
  }
  for (int i = 1; i < argc && n < MAX; i++) {
    if (argv[i][0] == 'i' && argv[i][1] == 'p' && argv[i][2] == '=') {
      u32 l = 0;
      for (const char *p = argv[i] + 3; l < sizeof mod[n].arg - 2; p++) {
        mod[n].arg[l++] = *p == ',' ? 0 : *p;
        if (!*p)
          break;
      }
      mod[n].arg_len = l;
      continue;
    }
    int fd = mx_fio_open(argv[i], MX_FIO_RDONLY);
    int size = fd < 0 ? -1 : mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
    if (size <= 0 || to + size > mem + sizeof mem || mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET) != 0 ||
        mx_fio_read(fd, to, (u32)size) != size) {
      mx_printf("p26: cannot read %s\n", argv[i]);
      MX_TEST_END(1);
      return 1;
    }
    mx_fio_close(fd);
    mod[n].at = to, mod[n].size = (u32)size;
    to += ((u32)size + 63) & ~63u;
    n++;
  }
  if (n < 1) {
    mx_printf("p26: need the modules\n");
    MX_TEST_END(1);
    return 1;
  }
  mx_printf("p26: %d modules read, resetting the IOP\n", n);
  if (mx_iop_reset("") < 0)
    return 2;
  mx_iop_patch_lmb();
  mx_iop_patch_prefix_check();
  for (int i = 0; i < n; i++) {
    int res = 0;
    u32 a = (u32)ring & 0x1FFFFFFFu;
    if (i == n - 1) {
      for (int b = 28; b >= 0; b -= 4)
        mod[i].arg[mod[i].arg_len++] = "0123456789abcdef"[a >> b & 15];
      mod[i].arg[mod[i].arg_len++] = 0;
    }
    if (mx_iop_load_module_buffer(mod[i].at, mod[i].size, mod[i].arg_len ? mod[i].arg : 0,
                                  mod[i].arg_len, &res) < 0)
      return 3;
  }
  int filled, bad = check(&filled);
  return !filled ? 9 : bad ? 10 + bad : 0;
}
