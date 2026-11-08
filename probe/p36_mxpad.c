/* mxpad.irx (iop/usb/mxpad) from the EE's side: loads usbd from mc0:/MXOS
   (if it is not running yet) and mxpad, gives mxpad the area, and prints
   each slot's state whenever it changes; a pad that comes rumbles briefly.

     p36_mxpad host:mxpad.irx [seconds [usbd path]]
     p36_mxpad - [seconds]     (mxpad already runs: mxlink loads it from mc0:/MXOS)
     p36_mxpad - seconds pair  (and pairs a new pad over Bluetooth: MXPAD_PAIR)

   Loading the modules makes mxlink reset the IOP when the program ends. */

#include <mx/iop.h>
#include <mx/mx.h>
#include <mx/pad.h>
#include <mx/sif.h>
#include <mx/thread.h>

#include "../common/include/mxpad.h"

static u8 irx[64 * 1024] MX_ALIGNED(64);
static struct mxpad_area area MX_DMA_LINES;
static struct mx_sif_client rpc;
static u32 io[16] MX_DMA_LINES;

static u32 arg_len(const char *a) {
  u32 n = 0;
  if (!a)
    return 0;
  while (a[n])
    n++;
  return n + 1;
}

static int load(const char *path, const char *arg) {
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return fd;
  int n = mx_fio_read(fd, irx, sizeof irx), res = 0;
  mx_fio_close(fd);
  if (n <= 0)
    return -1;
  int r = mx_iop_load_module_buffer(irx, (u32)n, arg, arg_len(arg), &res);
  return r < 0 ? r : res;
}

static int output(u32 slot, u8 small, u8 big) {
  volatile u32 *a = MX_UNCACHED(io);
  a[0] = slot, a[1] = (u32)small | (u32)big << 8, a[2] = 0;
  mx_printf("p36: output slot %u %u/%u ...\n", slot, small, big);
  int r = mx_sif_call(&rpc, MXPAD_OUTPUT, (void *)a, 16, (void *)MX_UNCACHED(io + 8), 16);
  mx_printf("p36: output slot %u: %d\n", slot, r < 0 ? r : (int)MX_UNCACHED(io + 8)[0]);
  return r < 0 ? -1 : (int)MX_UNCACHED(io + 8)[0];
}

/* the newer of the slot's two records */
static struct mxpad_record newest(int slot) {
  volatile struct mxpad_record *r = MX_UNCACHED(area.r[slot]);
  struct mxpad_record a = r[0], b = r[1];
  return (int)(b.seq - a.seq) > 0 ? b : a;
}

static const char *const names[19] = {"select", "L3",       "R3",     "start", "up",    "right", "down",
                                      "left",   "L2",       "R2",     "L1",    "R1",    "triangle",
                                      "circle", "cross",    "square", "PS",    "touchpad", "mute"};
static const char *const kinds[4] = {"none", "DS3", "DS4", "DualSense"};

static void show(int slot, const struct mxpad_record *s) {
  mx_printf("p36: slot %d %s #%u: L %3u,%3u R %3u,%3u L2 %3u R2 %3u", slot, kinds[s->kind & 3], s->connected,
            s->lx, s->ly, s->rx, s->ry, s->l2, s->r2);
  if (s->flags & MXPAD_TOUCH)
    mx_printf(" touch %u,%u", s->tx, s->ty);
  if (s->battery != 255)
    mx_printf(" battery %u%s", s->battery, s->flags & MXPAD_CABLE ? " cable" : "");
  mx_printf(" |");
  for (int i = 0; i < 19; i++)
    if (s->buttons & (1u << i))
      mx_printf(" %s", names[i]);
  mx_printf("\n");
}

static int far(u8 a, u8 b) { return (a > b ? a - b : b - a) >= 12; }

static int changed(const struct mxpad_record *a, const struct mxpad_record *b) {
  return a->kind != b->kind || a->connected != b->connected || a->buttons != b->buttons || far(a->lx, b->lx) ||
         far(a->ly, b->ly) || far(a->rx, b->rx) || far(a->ry, b->ry) || far(a->l2, b->l2) || far(a->r2, b->r2) ||
         (a->flags ^ b->flags) || a->battery != b->battery ||
         ((a->flags & MXPAD_TOUCH) && (a->tx / 128 != b->tx / 128 || a->ty / 128 != b->ty / 128));
}

int main(int argc, char **argv) {
  if (argc < 2 || mx_sif_init() < 0) {
    MX_TEST_END(1);
    return 1;
  }
  int secs = 0;
  for (const char *c = argc > 2 ? argv[2] : "30"; *c >= '0' && *c <= '9'; c++)
    secs = secs * 10 + (*c - '0');
  int pair = argc > 3 && argv[3][0] == 'p';
  const char *usbd = argc > 3 && !pair ? argv[3] : "mc0:/MXOS/usbd.irx";
  mx_printf("p36: %s: %d (an error is fine when usbd already runs)\n", usbd, load(usbd, 0));
  /* "-": mxpad already runs (mxlink loads it at the boot) */
  int r = argv[1][0] == '-' && !argv[1][1] ? 0 : load(argv[1], 0);
  mx_printf("p36: %s: %d\n", argv[1], r);
  if (r < 0 || (r & 3) == 1 || mx_sif_bind(&rpc, MXPAD_SID) < 0) {
    MX_TEST_END(2);
    return 2;
  }
  volatile u32 *z = (volatile u32 *)MX_UNCACHED(&area);
  for (u32 i = 0; i < sizeof area / 4; i++)
    z[i] = 0;
  volatile u32 *a = MX_UNCACHED(io);
  a[0] = mx_paddr_of(&area).v;
  mx_sif_call(&rpc, MXPAD_AREA, (void *)a, 16, (void *)MX_UNCACHED(io + 8), 16);
  mx_printf("p36: area at %08x (paddr %08x), %u bytes\n", (u32)&area, mx_paddr_of(&area).v, (u32)sizeof area);
  mx_printf("p36: mxpad version %u, %u slots, %d s; plug pads in and out, press buttons\n",
            MX_UNCACHED(io + 8)[0], MX_UNCACHED(io + 8)[1], secs);
  if (pair) {
    a[0] = (u32)(secs > 10 ? secs - 10 : secs);
    mx_sif_call(&rpc, MXPAD_PAIR, (void *)a, 16, (void *)MX_UNCACHED(io + 8), 16);
    mx_printf("p36: pairing for %u s: put a pad in pairing mode (DualSense: Create + PS)\n", a[0]);
  }

  struct mxpad_record last[MXPAD_PADS] = {0};
  int rumble_off[MXPAD_PADS] = {0};
  int dumps = 0;
  u32 updates = 0, seen_seq = 0; /* records seen, the highest seq */
  for (int t = 0; t < secs * 50; t++) {
    mx_sleep_us(20000);
    if (t % 500 == 0)
      mx_printf("p36: %d s\n", t / 50);
    for (int s = 0; s < MXPAD_PADS; s++) {
      struct mxpad_record n = newest(s);
      if (n.kind && !n.connected && dumps < 6) { /* not a record mxpad writes: the area as it is */
        volatile u32 *w = (volatile u32 *)MX_UNCACHED(&area);
        mx_printf("p36: odd record in slot %d; area by records:\n", s);
        for (int k = 0; k < 8; k++)
          mx_printf("p36:   %d.%d: %08x %08x %08x %08x %08x %08x %08x %08x\n", k / 2, k % 2, w[k * 8], w[k * 8 + 1],
                    w[k * 8 + 2], w[k * 8 + 3], w[k * 8 + 4], w[k * 8 + 5], w[k * 8 + 6], w[k * 8 + 7]);
        dumps++;
      }
      if (n.seq > seen_seq)
        updates += n.seq - seen_seq > 1000 ? 0 : 1, seen_seq = n.seq;
      if (!n.seq || !changed(&n, &last[s]))
        continue;
      if (n.kind && n.connected != last[s].connected) {
        mx_printf("p36: slot %d: a pad came: rumble %d\n", s, output((u32)s, 0xc0, 0xc0));
        rumble_off[s] = t + 25; /* half a second */
      } else if (!n.kind && last[s].kind) {
        mx_printf("p36: slot %d: the pad went\n", s);
      }
      show(s, &n);
      last[s] = n;
    }
    for (int s = 0; s < MXPAD_PADS; s++)
      if (rumble_off[s] && t >= rumble_off[s]) {
        output((u32)s, 0, 0);
        rumble_off[s] = 0;
      }
  }
  mx_printf("p36: the area off ...\n");
  a[0] = 0; /* no more DMA into this program's memory */
  mx_sif_call(&rpc, MXPAD_AREA, (void *)a, 16, (void *)MX_UNCACHED(io + 8), 16);
  mx_printf("p36: end: %u updates seen, last seq %u\n", updates, seen_seq);
  MX_TEST_END(0);
  return 0;
}
