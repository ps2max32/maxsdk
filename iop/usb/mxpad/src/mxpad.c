/* mxpad: DualShock 3, DualShock 4 and DualSense pads on USB for the EE
   (common/include/mxpad.h), a class driver of ps2sdk's usbd.

   Each pad on USB gets an entry and a thread; a pad's slot (its player
   number for the EE) is given once its address is known, the one it had
   before if that is free, so a pad keeps its slot from USB to Bluetooth and
   back. Each pad gets a thread. The thread sets the pad up (its
   configuration, the DS3's start report 0xF4, the slot's light), starts its
   input and then sends output (rumble, light) when the EE asks for it. The
   input reports come in usbd's callbacks: each is decoded into the slot's
   record, and a changed record goes to the EE by SIF DMA, into the older of
   the slot's two records in the EE's area. The report layouts are the ones
   measured in docs/pad-bt-plan.md (probe/iop/hidprobe.c). */

#include <irx.h>
#include <intrman.h>
#include <ioman.h>
#include <sys/fcntl.h>
#include <loadcore.h>
#include <sifcmd.h>
#include <sifman.h>
#include <stdio.h>
#include <sysclib.h>
#include <thbase.h>
#include <thsemap.h>
#include <usbd.h>
#include "usbd_macro.h"

#include <mxpad.h>

#include "mxpad_int.h"

IRX_ID("mxpad", 1, 0);

/* <mx/pad.h>'s bits */
#define B_SELECT 0x0001
#define B_L3 0x0002
#define B_R3 0x0004
#define B_START 0x0008
#define B_UP 0x0010
#define B_RIGHT 0x0020
#define B_DOWN 0x0040
#define B_LEFT 0x0080
#define B_L2 0x0100
#define B_R2 0x0200
#define B_L1 0x0400
#define B_R1 0x0800
#define B_TRIANGLE 0x1000
#define B_CIRCLE 0x2000
#define B_CROSS 0x4000
#define B_SQUARE 0x8000

struct pad {
  int id, used, gone;
  int bt; /* a Bluetooth pad (bt.c): no USB, no thread here */
  int slot; /* the EE's slot, -1: none yet */
  u8 addr[6], has_addr; /* its Bluetooth address (HCI byte order) */
  u32 gen; /* this connection's: usbd's callbacks of an earlier one are dropped */
  int op_sema;   /* the thread's own transfers */
  int work_sema; /* output wanted, or the pad gone */
  int kind;
  int ctrl, in, out;
  u16 in_size;
  u8 intf, config;
  volatile int done, timeout, result, bytes;
  volatile int streaming;
  u32 connected;
  /* output the EE wants; pairing with a dongle that came */
  volatile int want, want_pair;
  u8 small, big, r, g, b;
  struct mxpad_record now; /* the state as last decoded (seq unused) */
  struct mxpad_record sent;
  u8 rep[64] __attribute__((aligned(16)));
  u8 buf[64] __attribute__((aligned(16)));
};

/* more entries than slots: a pad plugged in while its Bluetooth link is
   still up has two for a moment */
#define ENTRIES (MXPAD_PADS + 2)
static struct pad pads[ENTRIES];
static u32 generation;
/* each slot's owner (an entry + 1, 0: free) and the slot's two records on
   their way to the EE */
static int slot_owner[MXPAD_PADS];
static struct mxpad_record slot_dma[MXPAD_PADS][2] __attribute__((aligned(16)));
static int slot_dma_id[MXPAD_PADS][2], slot_last[MXPAD_PADS];
/* the slot each pad had last, by address */
#define MEMOS 8
static struct {
  u8 addr[6], used;
  int slot;
} memo[MEMOS];
static u32 memo_next;
/* a callback's argument: the entry and the connection's generation, so that a
   transfer that ends after its pad went (usbd does not always call it back)
   never touches the entry's next pad */
#define TAG(p) ((void *)((u32)((p) - pads) | (p)->gen << 4))
static volatile u32 area; /* the EE's struct mxpad_area, physical; 0: none */
static u32 seq, connections;

/* ---- to the EE ---- */

/* a record to the older of the slot's two in the EE's area: 1 when it went.
   From usbd's callbacks and the threads: with interrupts off, so one at a time. */
static int send_slot(int slot, const struct mxpad_record *rec) {
  int st, sent = 0;
  CpuSuspendIntr(&st);
  u32 ee = area;
  int i = slot_last[slot] ^ 1;
  /* the older one still on its way (the IOP sends a record in microseconds):
     the next report sends the state */
  if (ee && !(slot_dma_id[slot][i] && sceSifDmaStat(slot_dma_id[slot][i]) >= 0)) {
    struct mxpad_record *r = &slot_dma[slot][i];
    *r = *rec;
    r->seq = ++seq;
    SifDmaTransfer_t d;
    d.src = r;
    d.dest = (void *)(ee + (u32)(slot * 2 + i) * sizeof *r);
    d.size = sizeof *r;
    d.attr = 0;
    slot_dma_id[slot][i] = (int)sceSifSetDma(&d, 1);
    if (slot_dma_id[slot][i])
      slot_last[slot] = i, sent = 1;
  }
  CpuResumeIntr(st);
  return sent;
}

/* the pad's state to its slot */
static void send(struct pad *p) {
  if (p->slot >= 0 && send_slot(p->slot, &p->now))
    p->sent = p->now;
}

/* a slot for the pad: the one it had before if that is free, else the first
   free one; -1 when all are taken */
static int claim_slot(struct pad *p) {
  int want = -1, m = -1;
  for (int i = 0; i < MEMOS && p->has_addr; i++)
    if (memo[i].used && !memcmp(memo[i].addr, p->addr, 6))
      m = i, want = memo[i].slot;
  int st;
  CpuSuspendIntr(&st);
  if (want < 0 || slot_owner[want])
    for (want = 0; want < MXPAD_PADS && slot_owner[want]; want++)
      ;
  if (want < MXPAD_PADS)
    slot_owner[want] = (int)(p - pads) + 1;
  CpuResumeIntr(st);
  if (want >= MXPAD_PADS)
    return -1;
  if (p->has_addr) {
    if (m < 0)
      m = (int)(memo_next++ % MEMOS);
    memcpy(memo[m].addr, p->addr, 6);
    memo[m].used = 1, memo[m].slot = want;
  }
  p->slot = want;
  p->now.connected = ++connections;
  send(p);
  return want;
}

/* the pad gone: its slot empty for the EE, and free */
static void release_slot(struct pad *p) {
  if (p->slot < 0)
    return;
  memset(&p->now, 0, sizeof p->now);
  send_slot(p->slot, &p->now);
  slot_owner[p->slot] = 0;
  p->slot = -1;
}

/* the slot's own: free or not, from its memo */
static int slot_of_addr(const u8 *addr) {
  for (int i = 0; i < MEMOS; i++)
    if (memo[i].used && !memcmp(memo[i].addr, addr, 6))
      return memo[i].slot;
  return -1;
}

static int same(const struct mxpad_record *a, const struct mxpad_record *b) {
  return !memcmp((const u8 *)a + 4, (const u8 *)b + 4, sizeof *a - 4);
}

/* ---- the reports ---- */

static void face_dpad(u8 v, struct mxpad_record *s) {
  static const u32 dpad[9] = {B_UP,   B_UP | B_RIGHT,  B_RIGHT, B_DOWN | B_RIGHT, B_DOWN,
                              B_DOWN | B_LEFT, B_LEFT, B_UP | B_LEFT, 0};
  u32 d = v & 15;
  s->buttons |= dpad[d > 8 ? 8 : d];
  s->buttons |= (v & 0x10 ? B_SQUARE : 0) | (v & 0x20 ? B_CROSS : 0) | (v & 0x40 ? B_CIRCLE : 0) |
                (v & 0x80 ? B_TRIANGLE : 0);
}

static void shoulders(u8 v, struct mxpad_record *s) {
  static const u32 m[8] = {B_L1, B_R1, B_L2, B_R2, B_SELECT, B_START, B_L3, B_R3};
  for (int i = 0; i < 8; i++)
    if (v & (1 << i))
      s->buttons |= m[i];
}

/* the report into p->now: 0, or -1 for a report that is not the pad's state */
static int decode(struct pad *p, const u8 *r, u32 n) {
  struct mxpad_record *s = &p->now;
  struct mxpad_record o = *s;
  s->buttons = 0, s->flags = 0, s->battery = 255;
  if (p->kind == MXPAD_DS4 && n >= 39 && r[0] == 0x01) {
    s->lx = r[1], s->ly = r[2], s->rx = r[3], s->ry = r[4];
    face_dpad(r[5], s);
    shoulders(r[6], s);
    s->buttons |= (r[7] & 1 ? MXPAD_PS : 0) | (r[7] & 2 ? MXPAD_TOUCHPAD : 0);
    s->l2 = r[8], s->r2 = r[9];
    u8 level = r[30] & 15;
    s->battery = level > 10 ? 10 : level;
    s->flags |= r[30] & 0x10 ? MXPAD_CABLE : 0;
    if (!(r[35] & 0x80)) {
      s->flags |= MXPAD_TOUCH;
      s->tx = (u16)(r[36] | (r[37] & 15) << 8), s->ty = (u16)(r[37] >> 4 | r[38] << 4);
    }
  } else if (p->kind == MXPAD_DS5 && n >= 54 && r[0] == 0x01) {
    s->lx = r[1], s->ly = r[2], s->rx = r[3], s->ry = r[4], s->l2 = r[5], s->r2 = r[6];
    face_dpad(r[8], s);
    shoulders(r[9], s);
    s->buttons |= (r[10] & 1 ? MXPAD_PS : 0) | (r[10] & 2 ? MXPAD_TOUCHPAD : 0) | (r[10] & 4 ? MXPAD_MUTE : 0);
    if (!(r[33] & 0x80)) {
      s->flags |= MXPAD_TOUCH;
      s->tx = (u16)(r[34] | (r[35] & 15) << 8), s->ty = (u16)(r[35] >> 4 | r[36] << 4);
    }
    /* the charge state in the high nibble (Linux's hid-playstation): 0
       discharging, 1 charging, 2 full */
    u8 state = r[53] >> 4, level = r[53] & 15;
    s->battery = state == 2 ? 10 : level > 10 ? 10 : level;
    s->flags |= state == 1 || state == 2 ? MXPAD_CABLE : 0;
  } else if (p->kind == MXPAD_DS3 && n >= 31 && r[0] == 0x01) {
    /* byte 2: Select L3 R3 Start Up Right Down Left; byte 3: L2 R2 L1 R1 Triangle
       Circle Cross Square: <mx/pad.h>'s order, as the SIO2 pad's */
    s->buttons = (u32)r[2] | (u32)r[3] << 8 | (r[4] & 1 ? MXPAD_PS : 0);
    s->lx = r[6], s->ly = r[7], s->rx = r[8], s->ry = r[9];
    s->l2 = r[18], s->r2 = r[19];
    if (s->lx == 0 && s->ly == 0 && s->rx == 0 && s->ry == 0) {
      *s = o; /* sent now and then after an output report */
      return -1;
    }
    if (r[30] >= 0xee)
      s->battery = 10, s->flags |= MXPAD_CABLE;
    else
      s->battery = (u8)(r[30] <= 5 ? r[30] * 2 : 10);
  } else {
    return -1;
  }
  return 0;
}

static void report_cb(int result, int bytes, void *arg);

static void report_next(struct pad *p) {
  if (p->gone || sceUsbdInterruptTransfer(p->in, p->rep, p->in_size, report_cb, TAG(p)) != USB_RC_OK)
    p->streaming = 0;
}

static void report_cb(int result, int bytes, void *arg) {
  struct pad *p = &pads[(u32)arg & 15];
  if (!p->used || p->gen != (u32)arg >> 4)
    return;
  if (result == USB_RC_OK && !decode(p, p->rep, (u32)bytes) && !same(&p->now, &p->sent))
    send(p);
  if (result != USB_RC_OK && p->gone) {
    p->streaming = 0;
    return;
  }
  report_next(p);
}

/* ---- the thread's transfers, with a time limit ---- */

static unsigned int alarm_cb(void *arg) {
  struct pad *p = arg;
  p->timeout = 1;
  iSignalSema(p->op_sema);
  return 0;
}

static void op_cb(int result, int bytes, void *arg) {
  struct pad *p = &pads[(u32)arg & 15];
  if (!p->used || p->gen != (u32)arg >> 4)
    return; /* a transfer that timed out, of a pad gone since */
  p->result = result, p->bytes = bytes, p->done = 1;
  SignalSema(p->op_sema);
}

static int op_wait(struct pad *p, int started) {
  if (started != USB_RC_OK)
    return started;
  iop_sys_clock_t c;
  USec2SysClock(1000 * 1000, &c);
  SetAlarm(&c, alarm_cb, p);
  while (!p->done && !p->timeout)
    WaitSema(p->op_sema);
  CancelAlarm(alarm_cb, p);
  return p->done ? p->result : -1;
}

static int control(struct pad *p, int type, int req, int value, int len, void *data) {
  p->done = p->timeout = 0;
  return op_wait(p, sceUsbdControlTransfer(p->ctrl, type, req, value, p->intf, len, data, op_cb, TAG(p)));
}

static int interrupt_out(struct pad *p, u32 len) {
  p->done = p->timeout = 0;
  return op_wait(p, sceUsbdInterruptTransfer(p->out, p->buf, len, op_cb, TAG(p)));
}

/* ---- output: rumble and lights ---- */

/* the light of each slot: the PS4's colours, the DualSense's player lights */
static const u8 slot_rgb[MXPAD_PADS][3] = {{0, 0, 64}, {64, 0, 0}, {0, 64, 0}, {64, 0, 32}};
static const u8 slot_ds5[MXPAD_PADS] = {0x04, 0x0a, 0x15, 0x1b};

static int output(struct pad *p) {
  int slot = p->slot < 0 ? 0 : p->slot;
  u8 *b = p->buf, r = p->r, g = p->g, bl = p->b;
  if (!r && !g && !bl)
    r = slot_rgb[slot][0], g = slot_rgb[slot][1], bl = slot_rgb[slot][2];
  memset(b, 0, sizeof p->buf);
  switch (p->kind) {
  case MXPAD_DS3: /* output report 1, without its id */
    b[1] = p->small ? 0xff : 0, b[2] = p->small ? 1 : 0;
    b[3] = p->big ? 0xff : 0, b[4] = p->big;
    b[9] = (u8)(0x02 << slot);
    for (int i = 0; i < 4; i++) {
      u8 *l = b + 10 + 5 * i;
      l[0] = 0xff, l[1] = 0x27, l[2] = 0x10, l[3] = 0, l[4] = 0x32;
    }
    return control(p, 0x21, 0x09, 0x0201, 48, b);
  case MXPAD_DS4: /* report 5: motor and light bar */
    b[0] = 0x05, b[1] = 0x03;
    b[4] = p->small, b[5] = p->big;
    b[6] = r, b[7] = g, b[8] = bl;
    return p->out >= 0 ? interrupt_out(p, 32) : control(p, 0x21, 0x09, 0x0205, 32, b);
  case MXPAD_DS5: /* report 2: hid-playstation's common part */
    b[0] = 0x02;
    b[1] = 0x03;         /* compatible vibration, haptics select */
    b[2] = 0x04 | 0x10;  /* light bar, player lights */
    b[3] = p->small, b[4] = p->big;
    b[39] = 0x02;        /* light bar setup control */
    b[42] = 0x02;        /* light bar setup: light out (ends the start-up animation) */
    b[44] = slot_ds5[slot];
    b[45] = r, b[46] = g, b[47] = bl;
    return p->out >= 0 ? interrupt_out(p, 63) : control(p, 0x21, 0x09, 0x0202, 63, b);
  }
  return -1;
}

/* ---- pairing over USB with the dongle (bt.c), as a PS3 or PS4 does ---- */

static void addr_print(const char *what, const u8 *a) {
  printf("%s %02x:%02x:%02x:%02x:%02x:%02x", what, a[5], a[4], a[3], a[2], a[1], a[0]);
}

/* the pad connects to the dongle from now on, when its PS button is pressed
   on battery (it forgets the host it had) */
static void pair(struct pad *p) {
  u8 host[6], me[6];
  int r = -1;
  if (!bt_host(host) || !p->has_addr)
    return;
  u8 *b = p->buf;
  memcpy(me, p->addr, 6);
  if (p->kind == MXPAD_DS4) {
    /* feature 0x13: the host's address and the link key */
    b[0] = 0x13;
    memcpy(b + 1, host, 6);
    bt_new_key(me, b + 7);
    r = control(p, 0x21, 0x09, 0x0313, 23, b);
  } else if (p->kind == MXPAD_DS3) {
    /* feature 0xF5: the host's, most significant byte first */
    b[0] = 0x01, b[1] = 0x00;
    for (int i = 0; i < 6; i++)
      b[2 + i] = host[5 - i];
    r = control(p, 0x21, 0x09, 0x03f5, 8, b);
    if (!r)
      bt_known(me, MXPAD_DS3);
  } else if (p->kind == MXPAD_DS5) {
    /* feature 0x0A (26 bytes, its report descriptor says): the host's
       address and the link key as the DS4's 0x13, then 4 bytes left 0. Not in
       Linux; feature 0x09 read back shows the host it took */
    memset(b, 0, 27);
    b[0] = 0x0a;
    memcpy(b + 1, host, 6);
    bt_new_key(me, b + 7);
    bt_kind(me, MXPAD_DS5);
    r = control(p, 0x21, 0x09, 0x030a, 27, b);
    if (!r && !control(p, 0xa1, 0x01, 0x0309, 20, b) && p->bytes >= 16)
      addr_print("mxpad: DualSense: feature 0x09 now says host", b + 10), printf("\n");
  } else {
    return;
  }
  printf("mxpad: slot %d: ", p->slot);
  if (r) {
    printf("pairing failed (%d)\n", r);
    return;
  }
  addr_print("paired", me);
  addr_print(" with", host);
  printf("\n");
}

static void pair_all(void) {
  for (int i = 0; i < ENTRIES; i++)
    if (pads[i].used && !pads[i].gone && !pads[i].bt) {
      pads[i].want_pair = 1;
      SignalSema(pads[i].work_sema);
    }
}

/* ---- a pad's life ---- */

static void open_pipes(struct pad *p) {
  UsbDeviceDescriptor *dd = sceUsbdScanStaticDescriptor(p->id, 0, USB_DT_DEVICE);
  UsbConfigDescriptor *c = sceUsbdScanStaticDescriptor(p->id, dd, USB_DT_CONFIG);
  u8 *q = (u8 *)c, *end = q + c->wTotalLength;
  int here = 0, found = 0;
  p->in = p->out = -1;
  p->config = c->bConfigurationValue;
  /* the HID interface: the DS4 v2 and the DualSense have audio ones first */
  for (; q < end && q[0] >= 2; q += q[0]) {
    if (q[1] == USB_DT_INTERFACE) {
      UsbInterfaceDescriptor *i = (UsbInterfaceDescriptor *)q;
      here = !found && i->bInterfaceClass == USB_CLASS_HID && !i->bAlternateSetting;
      if (here)
        found = 1, p->intf = i->bInterfaceNumber;
    } else if (q[1] == USB_DT_ENDPOINT && here) {
      UsbEndpointDescriptor *e = (UsbEndpointDescriptor *)q;
      if (e->bmAttributes != USB_ENDPOINT_XFER_INT)
        continue;
      if ((e->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN && p->in < 0)
        p->in = sceUsbdOpenPipe(p->id, e), p->in_size = (u16)(e->wMaxPacketSizeLB | e->wMaxPacketSizeHB << 8);
      else if ((e->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT && p->out < 0)
        p->out = sceUsbdOpenPipeAligned(p->id, e);
    }
  }
  if (p->in_size > sizeof p->rep)
    p->in_size = sizeof p->rep;
}

/* the feature reports in the pad's HID report descriptor: id and bytes (a
   look at what a pad offers; only shown) */
static u8 rdesc[512] __attribute__((aligned(16)));

static void show_features(struct pad *p) {
  int len = 0;
  UsbConfigDescriptor *c = sceUsbdScanStaticDescriptor(p->id, sceUsbdScanStaticDescriptor(p->id, 0, USB_DT_DEVICE),
                                                       USB_DT_CONFIG);
  u8 *q = (u8 *)c, *end = q + c->wTotalLength;
  int here = 0;
  for (; q < end && q[0] >= 2; q += q[0]) {
    if (q[1] == USB_DT_INTERFACE)
      here = q[2] == p->intf;
    else if (here && q[1] == 0x21 && q[0] >= 9)
      len = q[7] | q[8] << 8;
  }
  if (len <= 0 || len > (int)sizeof rdesc ||
      control(p, 0x81, 0x06, 0x2200, len, rdesc) || p->bytes != len) {
    printf("mxpad: report descriptor (%d bytes): not read\n", len);
    return;
  }
  u32 id = 0, size = 0, count = 0, bits[256] = {0};
  for (int i = 0; i < len;) {
    u8 b = rdesc[i];
    int n = (b & 3) == 3 ? 4 : (b & 3);
    u32 v = 0;
    for (int k = 0; k < n && i + 1 + k < len; k++)
      v |= (u32)rdesc[i + 1 + k] << (8 * k);
    switch (b & 0xfc) {
    case 0x84: id = v & 0xff; break;   /* Report ID */
    case 0x74: size = v; break;        /* Report Size */
    case 0x94: count = v; break;       /* Report Count */
    case 0xb0: bits[id] += size * count; break; /* Feature */
    }
    i += 1 + n;
  }
  printf("mxpad: feature reports (id: bytes after the id):");
  for (int i = 0; i < 256; i++)
    if (bits[i])
      printf(" %02x:%u", i, bits[i] / 8);
  printf("\n");
}

/* the pad's Bluetooth address (HCI byte order), from its feature reports */
static void read_addr(struct pad *p) {
  u8 *b = p->buf;
  if (p->kind == MXPAD_DS4 && !control(p, 0xa1, 0x01, 0x0312, 16, b) && p->bytes >= 7) {
    memcpy(p->addr, b + 1, 6), p->has_addr = 1; /* 0x12: at 1..6, least significant first */
  } else if (p->kind == MXPAD_DS5 && !control(p, 0xa1, 0x01, 0x0309, 20, b) && p->bytes >= 7) {
    memcpy(p->addr, b + 1, 6), p->has_addr = 1; /* 0x09: the same */
  } else if (p->kind == MXPAD_DS3 && !control(p, 0xa1, 0x01, 0x03f2, 17, b) && p->bytes >= 10) {
    for (int i = 0; i < 6; i++) /* 0xF2: at 4..9, most significant first */
      p->addr[i] = b[9 - i];
    p->has_addr = 1;
  }
}

static void pad_thread(void *arg) {
  struct pad *p = arg;
  open_pipes(p);
  /* a DS3 plugged in while its Bluetooth link is up does not answer for a
     while (it hung once for good, on the PS2): tried again before giving up */
  int r = -1;
  for (int tries = 0; tries < 4 && r && !p->gone && p->in >= 0; tries++) {
    if (tries)
      DelayThread(500 * 1000);
    r = control(p, 0x00, 0x09, p->config, 0, 0); /* SET_CONFIGURATION */
    if (!r && p->kind == MXPAD_DS3) {
      static const u8 start[4] = {0x42, 0x0c, 0x00, 0x00};
      memcpy(p->buf, start, 4);
      r = control(p, 0x21, 0x09, 0x03f4, 4, p->buf);
    }
  }
  if (!r) {
    if (p->kind == MXPAD_DS5)
      show_features(p);
    read_addr(p);
    /* plugged in while its Bluetooth link is up: that link goes, its slot is this one's */
    int had = p->has_addr ? slot_of_addr(p->addr) : -1;
    if (had >= 0 && slot_owner[had] && bt_drop(p->addr))
      for (int i = 0; i < 20 && slot_owner[had]; i++)
        DelayThread(20 * 1000);
    if (claim_slot(p) < 0)
      r = -1;
  }
  printf("mxpad: slot %d: %s (usb %d): %s\n", p->slot,
         p->kind == MXPAD_DS3 ? "DualShock 3" : p->kind == MXPAD_DS4 ? "DualShock 4" : "DualSense", p->id,
         r ? "set-up failed or no slot free" : "ready");
  if (!r) {
    output(p);
    pair(p);
    p->streaming = 1;
    report_next(p);
    while (!p->gone && p->streaming) {
      WaitSema(p->work_sema);
      if (p->want && !p->gone) {
        p->want = 0;
        output(p);
      }
      if (p->want_pair && !p->gone) {
        p->want_pair = 0;
        pair(p);
      }
    }
  }
  /* gone: the EE sees the slot empty */
  printf("mxpad: slot %d: gone\n", p->slot);
  release_slot(p);
  DeleteSema(p->op_sema);
  DeleteSema(p->work_sema);
  p->used = 0;
  ExitDeleteThread();
}

static int kind_of(int id) {
  UsbDeviceDescriptor *d = sceUsbdScanStaticDescriptor(id, 0, USB_DT_DEVICE);
  if (!d || d->idVendor != 0x054c)
    return MXPAD_NONE;
  switch (d->idProduct) {
  case 0x0268: return MXPAD_DS3;
  case 0x05c4: case 0x09cc: case 0x0ba0: return MXPAD_DS4;
  case 0x0ce6: case 0x0df2: return MXPAD_DS5;
  }
  return MXPAD_NONE;
}

static int probe(int id) { return kind_of(id) != MXPAD_NONE; }

static int connect(int id) {
  struct pad *p = 0;
  for (int i = 0; i < ENTRIES && !p; i++)
    if (!pads[i].used)
      p = &pads[i];
  if (!p)
    return -1;
  iop_sema_t s = {0, 0, 0, 64};
  iop_thread_t t = {TH_C, 0, pad_thread, 0x800, 0x28};
  memset(p, 0, sizeof *p);
  p->used = 1, p->id = id, p->kind = kind_of(id), p->gen = ++generation & 0x0fffffff;
  p->slot = -1;
  p->op_sema = CreateSema(&s);
  p->work_sema = CreateSema(&s);
  p->ctrl = sceUsbdOpenPipe(id, 0);
  p->now.kind = (u8)p->kind;
  p->now.battery = 255;
  p->now.lx = p->now.ly = p->now.rx = p->now.ry = 128;
  sceUsbdSetPrivateData(id, p);
  int th = CreateThread(&t);
  if (th <= 0 || StartThread(th, p) < 0) {
    p->used = 0;
    return -1;
  }
  return 0;
}

static int disconnect(int id) {
  struct pad *p = sceUsbdGetPrivateData(id);
  if (p) {
    p->gone = 1;
    SignalSema(p->work_sema);
  }
  return 0;
}

static sceUsbdLddOps driver = {0, 0, "mxpad", probe, connect, disconnect, 0, 0, 0, 0, 0, 0};

/* ---- the EE's calls ---- */

static u32 rpc_in[16] __attribute__((aligned(16)));

static void *rpc(int fn, void *data, int size) {
  u32 *a = data;
  (void)size;
  switch (fn) {
  case MXPAD_AREA: {
    static const struct mxpad_record none;
    area = a[0] & 0x1fffffff;
    for (int i = 0; i < MXPAD_PADS; i++) /* what the EE missed: every slot once */
      if (slot_owner[i])
        send(&pads[slot_owner[i] - 1]);
      else
        send_slot(i, &none);
    a[0] = MXPAD_VERSION, a[1] = MXPAD_PADS;
    break;
  }
  case MXPAD_OUTPUT: {
    u32 slot = a[0];
    const u8 *o = (const u8 *)(a + 1);
    struct pad *p = slot < MXPAD_PADS && slot_owner[slot] ? &pads[slot_owner[slot] - 1] : 0;
    if (!p || !p->used || p->gone) {
      a[0] = (u32)-1;
      break;
    }
    p->small = o[0], p->big = o[1], p->r = o[2], p->g = o[3], p->b = o[4];
    if (p->bt) {
      bt_output((int)slot, o[0], o[1], o + 2);
      a[0] = 0;
      break;
    }
    p->want = 1;
    SignalSema(p->work_sema);
    a[0] = 0;
    break;
  }
  case MXPAD_PAIR:
    bt_pair(a[0]);
    a[0] = 0;
    break;
  default:
    a[0] = (u32)-1;
  }
  return data;
}

static void rpc_thread(void *arg) {
  static SifRpcDataQueue_t queue;
  static SifRpcServerData_t server;
  (void)arg;
  sceSifSetRpcQueue(&queue, GetThreadId());
  sceSifRegisterRpc(&server, MXPAD_SID, rpc, rpc_in, 0, 0, &queue);
  sceSifRpcLoop(&queue);
}

int _start(int argc, char **argv) {
  (void)argc, (void)argv;
  for (int i = 0; i < ENTRIES; i++)
    pads[i].slot = -1;
  sceSifInitRpc(0);
  iop_thread_t t = {TH_C, 0, rpc_thread, 0x800, 0x50};
  int th = CreateThread(&t);
  if (th <= 0 || StartThread(th, 0) < 0)
    return MODULE_NO_RESIDENT_END;
  if (sceUsbdRegisterLdd(&driver) != USB_RC_OK) {
    printf("mxpad: usbd is not there\n");
    return MODULE_NO_RESIDENT_END;
  }
  bt_init(pair_all);
  return MODULE_RESIDENT_END;
}

/* ---- the slots of Bluetooth pads (bt.c) ----
   After _start on purpose: clang emits a file's static functions where they
   are first used, so the first function of .text is the file's first one
   that is not static, and iopfixup takes only _start at 0. */

int mxpad_open(int kind, const u8 addr[6]) {
  for (int i = 0; i < ENTRIES; i++) {
    struct pad *p = &pads[i];
    if (p->used)
      continue;
    memset(p, 0, sizeof *p);
    p->used = 1, p->bt = 1, p->kind = kind, p->gen = ++generation & 0x0fffffff;
    p->slot = -1;
    memcpy(p->addr, addr, 6), p->has_addr = 1;
    p->now.kind = (u8)kind;
    p->now.battery = 255;
    p->now.lx = p->now.ly = p->now.rx = p->now.ry = 128;
    if (claim_slot(p) < 0) {
      p->used = 0;
      return -1;
    }
    return p->slot;
  }
  return -1;
}

static struct pad *bt_pad(int slot) {
  struct pad *p = slot >= 0 && slot < MXPAD_PADS && slot_owner[slot] ? &pads[slot_owner[slot] - 1] : 0;
  return p && p->used && p->bt ? p : 0;
}

void mxpad_input(int slot, const u8 *r, u32 n) {
  struct pad *p = bt_pad(slot);
  if (p && !decode(p, r, n) && !same(&p->now, &p->sent))
    send(p);
}

void mxpad_close(int slot) {
  struct pad *p = bt_pad(slot);
  if (!p)
    return;
  release_slot(p);
  p->used = 0;
}

void mxpad_slot_rgb(int slot, u8 rgb[3]) { memcpy(rgb, slot_rgb[slot], 3); }
