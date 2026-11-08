/* Step 0 of docs/pad-bt-plan.md: what DualShock 3, DualShock 4 and DualSense
   pads and Bluetooth dongles look like on the PS2's USB. A class driver of
   ps2sdk's usbd that takes every device but hubs and mass storage, and for
   each:
     - prints its device and configuration descriptors (decoded and raw);
     - a Sony pad: reads its feature reports with the Bluetooth addresses
       (read only, nothing is paired), starts its input (DS3: report 0xF4),
       sends one output report (a light and a short rumble), then once a
       second prints the report rate, the spread between reports and the
       newest report;
     - a Bluetooth dongle (class E0/01/01): HCI Reset, Read Local Version,
       Read BD_ADDR, Read Buffer Size, Read Local Features, Read Local Name,
       then an inquiry of 5 s (put a pad in pairing mode to see it: DS4
       Share + PS, DualSense Create + PS);
     - any other HID device: its first interrupt IN pipe, as a pad's.
   Plug devices in and out while it runs. After the given time (argument,
   seconds, default 30) it prints a summary and IOP idle time, and returns.
   Run by probe/p35_hid.c; output on the IOP's console. */

#include "hidprobe_imports.h"
#include "usbd_macro.h"

IRX_ID("hidprobe", 1, 0);

#define DEVS 4
#define TICKS_US 37 /* timer ticks a microsecond, about (36.864 MHz) */
#define TICKS_MS 36864u

enum kind { K_OTHER, K_DS3, K_DS4, K_DS5, K_BT };
static const char *const kind_name[] = {"hid", "DS3", "DS4", "DualSense", "bt-dongle"};

struct dev {
  int id, used, gone, sema;
  enum kind kind;
  const char *model;
  int ctrl, in, out, bulk_in, bulk_out;
  u16 in_size, out_size;
  u8 config, intf;
  /* one synchronous operation at a time (control, or one event read) */
  volatile int done, timeout, result, bytes;
  /* the pad's input stream, from usbd's callback */
  volatile u32 reports, errors, last_t, d_min, d_max, d_sum, d_n, len;
  volatile int streaming;
  u32 reports_total, shown_at;
  u8 rep[64] __attribute__((aligned(16)));
  u8 shown[64], prev[64];
  u8 buf[264] __attribute__((aligned(16)));
  u8 ev[264];
};

static struct dev devs[DEVS];
static int timer, stop;
static volatile int stop_inquiry; /* set 7 s before the end: an inquiry takes 5 s */
static volatile u32 idle_count;

#define now() ((u32)GetTimerCounter(timer))

/* ---- waiting with a timeout ---- */

static unsigned int alarm_cb(void *arg) {
  struct dev *d = arg;
  d->timeout = 1;
  iSignalSema(d->sema);
  return 0;
}

static void op_cb(int result, int bytes, void *arg) {
  struct dev *d = arg;
  d->result = result, d->bytes = bytes, d->done = 1;
  SignalSema(d->sema);
}

/* after starting a transfer with op_cb: its result (USB_RC_*), -1 on a timeout */
static int op_wait(struct dev *d, int started, u32 ms) {
  if (started != USB_RC_OK)
    return started;
  iop_sys_clock_t c;
  USec2SysClock(ms * 1000, &c);
  SetAlarm(&c, alarm_cb, d);
  while (!d->done && !d->timeout)
    WaitSema(d->sema);
  CancelAlarm(alarm_cb, d);
  return d->done ? d->result : -1;
}

static void op_begin(struct dev *d) { d->done = d->timeout = 0, d->bytes = 0; }

static int control(struct dev *d, int type, int req, int value, int index, int len, void *data) {
  op_begin(d);
  return op_wait(d, sceUsbdControlTransfer(d->ctrl, type, req, value, index, len, data, op_cb, d), 1000);
}

static void hex(const char *what, const u8 *p, u32 n) {
  printf("hidprobe:   %s (%u):", what, n);
  for (u32 i = 0; i < n; i++)
    printf("%s%02x", i && !(i % 32) ? "\nhidprobe:     " : " ", p[i]);
  printf("\n");
}

/* ---- descriptors ---- */

static void dump_descriptors(struct dev *d) {
  UsbDeviceDescriptor *dd = sceUsbdScanStaticDescriptor(d->id, 0, USB_DT_DEVICE);
  UsbConfigDescriptor *c = sceUsbdScanStaticDescriptor(d->id, dd, USB_DT_CONFIG);
  printf("hidprobe: dev %d: %04x:%04x usb %x class %02x/%02x/%02x ep0 %u bcdDevice %x configs %u\n", d->id,
         dd->idVendor, dd->idProduct, dd->bcdUSB, dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol,
         dd->bMaxPacketSize0, dd->bcdDevice, dd->bNumConfigurations);
  if (!c)
    return;
  u8 *p = (u8 *)c, *end = p + c->wTotalLength;
  printf("hidprobe: dev %d: config %u, %u interfaces, attributes %02x, %u mA\n", d->id, c->bConfigurationValue,
         c->bNumInterfaces, c->bmAttributes, c->maxPower * 2);
  for (; p < end && p[0] >= 2; p += p[0]) {
    if (p[1] == USB_DT_INTERFACE) {
      UsbInterfaceDescriptor *i = (UsbInterfaceDescriptor *)p;
      printf("hidprobe:   interface %u alt %u: class %02x/%02x/%02x, %u endpoints\n", i->bInterfaceNumber,
             i->bAlternateSetting, i->bInterfaceClass, i->bInterfaceSubClass, i->bInterfaceProtocol,
             i->bNumEndpoints);
    } else if (p[1] == USB_DT_ENDPOINT) {
      UsbEndpointDescriptor *e = (UsbEndpointDescriptor *)p;
      static const char *const t[] = {"control", "iso", "bulk", "interrupt"};
      printf("hidprobe:     endpoint %02x %s, %u bytes, interval %u\n", e->bEndpointAddress,
             t[e->bmAttributes & 3], e->wMaxPacketSizeLB | e->wMaxPacketSizeHB << 8, e->bInterval);
    } else if (p[1] == 0x21) {
      printf("hidprobe:     HID descriptor: version %x, report descriptor %u bytes\n", p[2] | p[3] << 8,
             p[7] | p[8] << 8);
    }
  }
  hex("config descriptor", (u8 *)c, c->wTotalLength);
}

/* the first interface of the configuration */
static UsbInterfaceDescriptor *first_interface(int id) {
  UsbDeviceDescriptor *dd = sceUsbdScanStaticDescriptor(id, 0, USB_DT_DEVICE);
  UsbConfigDescriptor *c = dd ? sceUsbdScanStaticDescriptor(id, dd, USB_DT_CONFIG) : 0;
  if (!c)
    return 0;
  u8 *p = (u8 *)c, *end = p + c->wTotalLength;
  for (; p < end && p[0] >= 2; p += p[0])
    if (p[1] == USB_DT_INTERFACE)
      return (UsbInterfaceDescriptor *)p;
  return 0;
}

static enum kind kind_of(int id, const char **model) {
  UsbDeviceDescriptor *dd = sceUsbdScanStaticDescriptor(id, 0, USB_DT_DEVICE);
  UsbInterfaceDescriptor *i = first_interface(id);
  *model = "";
  if (dd->idVendor == 0x054c) {
    switch (dd->idProduct) {
    case 0x0268: *model = "DualShock 3"; return K_DS3;
    case 0x05c4: *model = "DualShock 4 v1"; return K_DS4;
    case 0x09cc: *model = "DualShock 4 v2"; return K_DS4;
    case 0x0ba0: *model = "DualShock 4 wireless adapter"; return K_DS4;
    case 0x0ce6: *model = "DualSense"; return K_DS5;
    case 0x0df2: *model = "DualSense Edge"; return K_DS5;
    }
  }
  if ((dd->bDeviceClass == 0xe0 && dd->bDeviceSubClass == 1 && dd->bDeviceProtocol == 1) ||
      (i && i->bInterfaceClass == 0xe0 && i->bInterfaceSubClass == 1 && i->bInterfaceProtocol == 1))
    return K_BT;
  return K_OTHER;
}

/* the interface to use (alt 0): HID (class 3) or Bluetooth (E0/01/01),
   else the first one with an interrupt endpoint. Pads can be composite: the
   DualShock 4 v2 and the DualSense have audio interfaces before their HID one. */
static int pick_interface(UsbConfigDescriptor *c) {
  u8 *p = (u8 *)c, *end = p + c->wTotalLength;
  int any = -1, cur = -1, cur_alt = 0;
  for (; p < end && p[0] >= 2; p += p[0]) {
    if (p[1] == USB_DT_INTERFACE) {
      UsbInterfaceDescriptor *i = (UsbInterfaceDescriptor *)p;
      cur = i->bInterfaceNumber, cur_alt = i->bAlternateSetting;
      if (!cur_alt && (i->bInterfaceClass == USB_CLASS_HID ||
                       (i->bInterfaceClass == 0xe0 && i->bInterfaceSubClass == 1 && i->bInterfaceProtocol == 1)))
        return cur;
    } else if (p[1] == USB_DT_ENDPOINT && !cur_alt && any < 0 &&
               ((UsbEndpointDescriptor *)p)->bmAttributes == USB_ENDPOINT_XFER_INT)
      any = cur;
  }
  return any < 0 ? 0 : any;
}

/* opens the pipes of that interface: first interrupt in/out, first bulk in/out */
static void open_pipes(struct dev *d) {
  UsbDeviceDescriptor *dd = sceUsbdScanStaticDescriptor(d->id, 0, USB_DT_DEVICE);
  UsbConfigDescriptor *c = sceUsbdScanStaticDescriptor(d->id, dd, USB_DT_CONFIG);
  u8 *p = (u8 *)c, *end = p + c->wTotalLength;
  int want = pick_interface(c), here = 0;
  d->in = d->out = d->bulk_in = d->bulk_out = -1;
  d->config = c->bConfigurationValue;
  d->intf = (u8)want;
  for (; p < end && p[0] >= 2; p += p[0]) {
    if (p[1] == USB_DT_INTERFACE) {
      UsbInterfaceDescriptor *i = (UsbInterfaceDescriptor *)p;
      here = i->bInterfaceNumber == want && !i->bAlternateSetting;
      continue;
    }
    if (p[1] != USB_DT_ENDPOINT || !here)
      continue;
    UsbEndpointDescriptor *e = (UsbEndpointDescriptor *)p;
    int in = (e->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN;
    u16 size = (u16)(e->wMaxPacketSizeLB | e->wMaxPacketSizeHB << 8);
    if (e->bmAttributes == USB_ENDPOINT_XFER_INT && in && d->in < 0)
      d->in = sceUsbdOpenPipe(d->id, e), d->in_size = size;
    else if (e->bmAttributes == USB_ENDPOINT_XFER_INT && !in && d->out < 0)
      d->out = sceUsbdOpenPipeAligned(d->id, e), d->out_size = size;
    else if (e->bmAttributes == USB_ENDPOINT_XFER_BULK && in && d->bulk_in < 0)
      d->bulk_in = sceUsbdOpenPipe(d->id, e);
    else if (e->bmAttributes == USB_ENDPOINT_XFER_BULK && !in && d->bulk_out < 0)
      d->bulk_out = sceUsbdOpenPipeAligned(d->id, e);
  }
  printf("hidprobe: dev %d: interface %u, pipes: control %d, interrupt in %d (%u) out %d (%u), bulk in %d out %d\n", d->id,
         d->intf, d->ctrl, d->in, d->in_size, d->out, d->out_size, d->bulk_in, d->bulk_out);
}

/* ---- pads ---- */

#define GET_REPORT(d, id, n) control(d, 0xa1, 0x01, 0x0300 | (id), (d)->intf, n, (d)->buf)

static void feature(struct dev *d, u8 id, u32 n, const char *what) {
  memset(d->buf, 0, n);
  int r = GET_REPORT(d, id, n);
  printf("hidprobe:   feature %02x %s\n", id, what);
  if (r)
    printf("hidprobe:   feature %02x: failed %d\n", id, r);
  else
    hex("feature", d->buf, (u32)d->bytes);
}

static void mac(const char *what, const u8 *p, int reversed) {
  printf("hidprobe:   %s ", what);
  for (int i = 0; i < 6; i++)
    printf("%s%02x", i ? ":" : "", p[reversed ? 5 - i : i]);
  printf("\n");
}

/* ---- the pads' input reports (USB; the report id at 0) ---- */

struct state {
  int ok;     /* 1: a layout this probe knows */
  u32 b;      /* buttons, B_* */
  int dpad;   /* 0..7 from up clockwise, 8 none */
  u8 lx, ly, rx, ry, l2, r2;
  int touch;  /* 1: a finger on the touchpad */
  u16 tx, ty;
  int battery, cable; /* -1: not known */
};

enum {
  B_CROSS = 1 << 0, B_CIRCLE = 1 << 1, B_SQUARE = 1 << 2, B_TRIANGLE = 1 << 3, B_L1 = 1 << 4, B_R1 = 1 << 5,
  B_L2 = 1 << 6, B_R2 = 1 << 7, B_SELECT = 1 << 8, B_START = 1 << 9, B_L3 = 1 << 10, B_R3 = 1 << 11,
  B_PS = 1 << 12, B_PAD = 1 << 13, B_MUTE = 1 << 14, B_BITS = 15
};
static const char *const b_name[B_BITS] = {"cross", "circle", "square", "triangle", "L1", "R1", "L2", "R2",
                                           "select/share/create", "start/options", "L3", "R3", "PS",
                                           "touchpad", "mute"};
static const char *const dpad_name[9] = {"up", "up-right", "right", "down-right", "down", "down-left",
                                         "left", "up-left", ""};

/* face buttons in the high nibble, the d-pad in the low one (DS4, DualSense) */
static void face_dpad(u8 v, struct state *s) {
  s->dpad = v & 15;
  if (s->dpad > 8)
    s->dpad = 8;
  s->b |= (v & 0x10 ? B_SQUARE : 0) | (v & 0x20 ? B_CROSS : 0) | (v & 0x40 ? B_CIRCLE : 0) |
          (v & 0x80 ? B_TRIANGLE : 0);
}

static void shoulders(u8 v, struct state *s) {
  static const u32 m[8] = {B_L1, B_R1, B_L2, B_R2, B_SELECT, B_START, B_L3, B_R3};
  for (int i = 0; i < 8; i++)
    if (v & (1 << i))
      s->b |= m[i];
}

static void decode(enum kind k, const u8 *r, u32 n, struct state *s) {
  memset(s, 0, sizeof *s);
  s->battery = s->cable = -1;
  if (k == K_DS4 && n >= 39 && r[0] == 0x01) {
    s->lx = r[1], s->ly = r[2], s->rx = r[3], s->ry = r[4];
    face_dpad(r[5], s);
    shoulders(r[6], s);
    s->b |= (r[7] & 1 ? B_PS : 0) | (r[7] & 2 ? B_PAD : 0);
    s->l2 = r[8], s->r2 = r[9];
    s->battery = r[30] & 15, s->cable = (r[30] >> 4) & 1;
    s->touch = !(r[35] & 0x80);
    s->tx = (u16)(r[36] | (r[37] & 15) << 8), s->ty = (u16)(r[37] >> 4 | r[38] << 4);
    s->ok = 1;
  } else if (k == K_DS5 && n >= 54 && r[0] == 0x01) {
    s->lx = r[1], s->ly = r[2], s->rx = r[3], s->ry = r[4], s->l2 = r[5], s->r2 = r[6];
    face_dpad(r[8], s);
    shoulders(r[9], s);
    s->b |= (r[10] & 1 ? B_PS : 0) | (r[10] & 2 ? B_PAD : 0) | (r[10] & 4 ? B_MUTE : 0);
    s->touch = !(r[33] & 0x80);
    s->tx = (u16)(r[34] | (r[35] & 15) << 8), s->ty = (u16)(r[35] >> 4 | r[36] << 4);
    s->battery = r[53] & 15, s->cable = (r[53] >> 4) != 0;
    s->ok = 1;
  } else if (k == K_DS3 && n >= 26 && r[0] == 0x01) {
    static const u32 m2[8] = {B_SELECT, B_L3, B_R3, B_START, 0, 0, 0, 0};
    static const u32 m3[8] = {B_L2, B_R2, B_L1, B_R1, B_TRIANGLE, B_CIRCLE, B_CROSS, B_SQUARE};
    for (int i = 0; i < 8; i++)
      s->b |= (r[2] & (1 << i) ? m2[i] : 0) | (r[3] & (1 << i) ? m3[i] : 0);
    s->b |= r[4] & 1 ? B_PS : 0;
    int up = r[2] & 0x10, right = r[2] & 0x20, down = r[2] & 0x40, left = r[2] & 0x80;
    s->dpad = up ? (right ? 1 : left ? 7 : 0) : down ? (right ? 3 : left ? 5 : 4) : right ? 2 : left ? 6 : 8;
    s->lx = r[6], s->ly = r[7], s->rx = r[8], s->ry = r[9];
    s->l2 = r[18], s->r2 = r[19];
    s->ok = 1;
  }
}

static int far(u8 a, u8 b) { return (a > b ? a - b : b - a) >= 12; }

static int changed(const struct state *a, const struct state *b) {
  return b->ok < 0 || a->b != b->b || a->dpad != b->dpad || far(a->lx, b->lx) || far(a->ly, b->ly) ||
         far(a->rx, b->rx) || far(a->ry, b->ry) || far(a->l2, b->l2) || far(a->r2, b->r2) ||
         a->touch != b->touch || (a->touch && (a->tx / 64 != b->tx / 64 || a->ty / 64 != b->ty / 64)) ||
         a->cable != b->cable || a->battery != b->battery;
}

static void show(struct dev *d, const struct state *s) {
  printf("hidprobe: dev %d %s: L %3u,%3u R %3u,%3u L2 %3u R2 %3u", d->id, kind_name[d->kind], s->lx, s->ly, s->rx,
         s->ry, s->l2, s->r2);
  if (s->touch)
    printf(" touch %u,%u", s->tx, s->ty);
  if (s->battery >= 0)
    printf(" battery %d%s", s->battery, s->cable ? " cable" : "");
  printf(" |%s%s", s->dpad < 8 ? " " : "", dpad_name[s->dpad]);
  for (int i = 0; i < B_BITS; i++)
    if (s->b & (1u << i))
      printf(" %s", b_name[i]);
  printf("\n");
}

static void report_cb(int result, int bytes, void *arg);

static void report_next(struct dev *d) {
  if (stop || d->gone || sceUsbdInterruptTransfer(d->in, d->rep, d->in_size, report_cb, d) != USB_RC_OK)
    d->streaming = 0;
}

static void report_cb(int result, int bytes, void *arg) {
  struct dev *d = arg;
  if (result != USB_RC_OK) {
    if (++d->errors > 100) { /* not a pad that streams: give up */
      d->streaming = 0;
      return;
    }
  } else {
    u32 t = now();
    if (d->reports) {
      u32 dt = t - d->last_t;
      if (dt < d->d_min)
        d->d_min = dt;
      if (dt > d->d_max)
        d->d_max = dt;
      d->d_sum += dt, d->d_n++;
    }
    d->last_t = t, d->reports++, d->len = (u32)bytes;
  }
  report_next(d);
}

/* a light and some rumble (on 1), or rumble off (on 0) */
static int output(struct dev *d, int on) {
  u8 *b = d->buf;
  memset(b, 0, 64);
  switch (d->kind) {
  case K_DS3: /* output report 1 by SET_REPORT, without its id */
    b[1] = on ? 0x30 : 0, b[2] = on ? 0x01 : 0; /* right (small) motor: duration, on */
    b[3] = on ? 0x30 : 0, b[4] = on ? 0x40 : 0; /* left motor: duration, force */
    b[9] = 0x02;                               /* LED 1 */
    for (int i = 0; i < 4; i++) {
      u8 *l = b + 10 + 5 * i;
      l[0] = 0xff, l[1] = 0x27, l[2] = 0x10, l[3] = 0, l[4] = 0x32;
    }
    return control(d, 0x21, 0x09, 0x0201, d->intf, 48, b);
  case K_DS4: /* report 5: flags (motor, light), -, -, right, left, r, g, b */
    b[0] = 0x05, b[1] = 0x03;
    b[4] = on ? 0x40 : 0, b[5] = on ? 0x40 : 0;
    b[6] = 0x00, b[7] = 0x40, b[8] = 0x00;
    if (d->out < 0)
      return control(d, 0x21, 0x09, 0x0205, d->intf, 32, b);
    op_begin(d);
    return op_wait(d, sceUsbdInterruptTransfer(d->out, b, 32, op_cb, d), 1000);
  case K_DS5: /* report 2 (USB): the common part of hid-playstation's layout */
    b[0] = 0x02;
    b[1] = 0x03;              /* compatible vibration, haptics select */
    b[2] = 0x04 | 0x10;       /* light bar, player lights */
    b[3] = on ? 0x40 : 0;     /* right motor */
    b[4] = on ? 0x40 : 0;     /* left motor */
    b[39] = 0x02;             /* light bar setup control */
    b[42] = 0x02;             /* light bar setup: light out (end the start-up animation) */
    b[44] = 0x04;             /* player light: the middle one */
    b[45] = 0x00, b[46] = 0x40, b[47] = 0x00;
    if (d->out < 0)
      return control(d, 0x21, 0x09, 0x0202, d->intf, 63, b);
    op_begin(d);
    return op_wait(d, sceUsbdInterruptTransfer(d->out, b, 63, op_cb, d), 1000);
  default:
    return 0;
  }
}

static void pad(struct dev *d) {
  switch (d->kind) {
  case K_DS3:
    feature(d, 0xf2, 17, "(pad address at 4..9)");
    if (!d->bytes)
      break;
    mac("pad address", d->buf + 4, 0);
    feature(d, 0xf5, 8, "(host address at 2..7)");
    if (d->bytes >= 8)
      mac("host address it connects to", d->buf + 2, 0);
    {
      static const u8 start[4] = {0x42, 0x0c, 0x00, 0x00};
      memcpy(d->buf, start, 4);
      printf("hidprobe:   start input (feature f4): %d\n", control(d, 0x21, 0x09, 0x03f4, d->intf, 4, d->buf));
    }
    break;
  case K_DS4:
    feature(d, 0x12, 16, "(pad address 1..6, host address 10..15, reversed)");
    if (d->bytes >= 16) {
      mac("pad address", d->buf + 1, 1);
      mac("host address it connects to", d->buf + 10, 1);
    }
    feature(d, 0xa3, 49, "(firmware)");
    break;
  case K_DS5:
    feature(d, 0x09, 20, "(pad address 1..6, reversed)");
    if (d->bytes >= 7)
      mac("pad address", d->buf + 1, 1);
    feature(d, 0x20, 64, "(firmware)");
    break;
  default:
    break;
  }
  if (d->in < 0) {
    printf("hidprobe: dev %d: no interrupt in pipe\n", d->id);
    return;
  }
  d->d_min = 0xffffffff, d->streaming = 1;
  report_next(d);
  DelayThread(1000 * 1000);
  printf("hidprobe: dev %d: output (light, rumble): %d\n", d->id, output(d, 1));
  DelayThread(500 * 1000);
  printf("hidprobe: dev %d: output (rumble off): %d\n", d->id, output(d, 0));
  struct state last;
  memset(&last, 0, sizeof last);
  last.ok = -1;
  u32 t_rate = now(), raw_at = 0;
  d->reports = d->d_n = d->d_sum = d->d_max = 0, d->d_min = 0xffffffff;
  while (!stop && !d->gone && d->streaming) {
    DelayThread(50 * 1000);
    u32 len = d->len;
    if (d->reports && len <= 64) {
      struct state st;
      memcpy(d->shown, d->rep, len);
      decode(d->kind, d->shown, len, &st);
      if (!st.ok && memcmp(d->shown, d->prev, len)) { /* unknown layout: the raw report on a change */
        if (d->kind == K_OTHER)
          hex("report", d->shown, len);
        memcpy(d->prev, d->shown, len);
      } else if (d->kind == K_DS3 && memcmp(d->shown, d->prev, len < 41 ? len : 41) &&
                 now() - raw_at >= 250 * TICKS_MS) { /* the DS3's raw report too, up to its motion sensors */
        hex("report", d->shown, len);
        memcpy(d->prev, d->shown, len);
        raw_at = now();
      }
      if (st.ok && changed(&st, &last)) {
        show(d, &st);
        last = st;
      }
    }
    if (now() - t_rate >= 5000 * TICKS_MS) {
      u32 n = d->reports, dn = d->d_n, ms = (now() - t_rate) / TICKS_MS;
      d->reports_total += n;
      printf("hidprobe: dev %d %s: %u reports/s, %u bytes, apart %u..%u us (avg %u), errors %u\n", d->id,
             kind_name[d->kind], n * 1000 / ms, len, dn ? d->d_min / TICKS_US : 0, dn ? d->d_max / TICKS_US : 0,
             dn ? d->d_sum / dn / TICKS_US : 0, d->errors);
      if (last.ok < 0 && len <= 64)
        hex("report", d->shown, len);
      t_rate = now();
      d->reports = d->d_n = d->d_sum = d->d_max = 0, d->d_min = 0xffffffff;
    }
  }
  d->reports_total += d->reports;
  if (!d->streaming && !d->gone)
    printf("hidprobe: dev %d: input stopped (errors %u)\n", d->id, d->errors);
}

/* ---- Bluetooth dongles: HCI over USB (commands on the control pipe, events on interrupt in) ---- */

/* one whole event into d->ev (code, length, parameters): 0, or < 0 */
static int hci_event(struct dev *d, u32 ms) {
  u32 have = 0, want = 2;
  while (have < want) {
    op_begin(d);
    int r = op_wait(d, sceUsbdInterruptTransfer(d->in, d->buf, d->in_size, op_cb, d), ms);
    if (r)
      return r < 0 ? r : -r;
    if (have + (u32)d->bytes > sizeof d->ev)
      return -2;
    memcpy(d->ev + have, d->buf, (u32)d->bytes);
    have += (u32)d->bytes;
    if (have >= 2)
      want = 2u + d->ev[1];
  }
  return 0;
}

static int hci_send(struct dev *d, u16 op, const u8 *par, u8 n) {
  u8 *b = d->buf;
  b[0] = (u8)op, b[1] = (u8)(op >> 8), b[2] = n;
  if (n)
    memcpy(b + 3, par, n);
  return control(d, 0x20, 0x00, 0, 0, 3 + n, b);
}

/* a command, then events up to its Command Complete (or failing Command
   Status): the return parameters at d->ev + 5 (status first), or < 0 */
static int hci_cmd(struct dev *d, u16 op, const u8 *par, u8 n, const char *what) {
  int r = hci_send(d, op, par, n);
  if (r) {
    printf("hidprobe:   %s: send failed %d\n", what, r);
    return -1;
  }
  for (int tries = 0; tries < 16; tries++) {
    if ((r = hci_event(d, 2000))) {
      printf("hidprobe:   %s: no event (%d)\n", what, r);
      return -1;
    }
    u8 *e = d->ev;
    if (e[0] == 0x0e && (e[3] | e[4] << 8) == op) {
      if (e[5])
        printf("hidprobe:   %s: status %02x\n", what, e[5]);
      return e[5] ? -1 : 0;
    }
    if (e[0] == 0x0f && (e[4] | e[5] << 8) == op && e[2]) {
      printf("hidprobe:   %s: command status %02x\n", what, e[2]);
      return -1;
    }
    hex("other event", e, 2u + e[1]);
  }
  return -1;
}

static const char *maker(u16 m) {
  switch (m) {
  case 0: return "Ericsson";
  case 2: return "Intel";
  case 10: return "Qualcomm (CSR)";
  case 13: return "Texas Instruments";
  case 15: return "Broadcom";
  case 29: return "Qualcomm";
  case 70: return "MediaTek";
  case 93: return "Realtek (needs firmware)";
  case 305: return "Cypress";
  default: return "?";
  }
}

static const char *bt_version(u8 v) {
  static const char *const s[] = {"1.0b", "1.1", "1.2", "2.0", "2.1", "3.0", "4.0",
                                  "4.1",  "4.2", "5.0", "5.1", "5.2", "5.3", "5.4"};
  return v < sizeof s / sizeof *s ? s[v] : "?";
}

/* the name in extended inquiry response data, or "" */
static void eir_name(const u8 *p, u32 n, char *out, u32 size) {
  out[0] = 0;
  for (u32 i = 0; i + 1 < n && p[i];) {
    u32 len = p[i], type = p[i + 1];
    if (i + 1 + len > n)
      break;
    if (type == 0x08 || type == 0x09) {
      u32 k = len - 1 < size - 1 ? len - 1 : size - 1;
      memcpy(out, p + i + 2, k);
      out[k] = 0;
      return;
    }
    i += 1 + len;
  }
}

static void inquiry(struct dev *d) {
  /* extended inquiry results (with names), else with RSSI, else plain; a dongle
     with mode 2 alternates between 2 and 1, to compare them */
  static int best = -1, round;
  int mode = best;
  if (best < 0) {
    for (best = 2; best > 0; best--) {
      u8 m = (u8)best;
      if (!hci_cmd(d, 0x0c45, &m, 1, "write inquiry mode"))
        break;
    }
    mode = best;
  } else if (best == 2) {
    u8 m = (round & 1) ? 1 : 2;
    mode = hci_cmd(d, 0x0c45, &m, 1, "write inquiry mode") ? -1 : m;
  }
  round++;
  printf("hidprobe:   inquiry mode %d\n", mode);
  static const u8 inq[5] = {0x33, 0x8b, 0x9e, 0x04, 0x00}; /* GIAC, 4 x 1.28 s, no limit */
  if (hci_send(d, 0x0401, inq, 5)) {
    printf("hidprobe:   inquiry: send failed\n");
    return;
  }
  printf("hidprobe:   inquiry 5 s (DS4: Share + PS, DualSense: Create + PS, until the light flashes)\n");
  int found = 0, answers = 0;
  u8 addrs[8][6];
  for (;;) {
    if (hci_event(d, 8000)) {
      printf("hidprobe:   inquiry: no end\n");
      return;
    }
    u8 *e = d->ev;
    if (e[0] == 0x0f) {
      if (e[2]) {
        printf("hidprobe:   inquiry: status %02x\n", e[2]);
        return;
      }
    } else if (e[0] == 0x01) {
      printf("hidprobe:   inquiry done: %d devices, %d answers, status %02x\n", found, answers, e[2]);
      return;
    } else if (e[0] == 0x02 || e[0] == 0x22 || e[0] == 0x2f) {
      u8 *r = e + 3; /* one response: address, then per type */
      u32 cls = e[0] == 0x02 ? (u32)(r[9] | r[10] << 8 | r[11] << 16) : (u32)(r[8] | r[9] << 8 | r[10] << 16);
      int rssi = e[0] == 0x02 ? 0 : (s8)r[13];
      int seen = 0;
      for (int k = 0; k < found && k < 8 && !seen; k++)
        seen = !memcmp(addrs[k], r, 6);
      answers++;
      if (seen)
        continue;
      if (found < 8)
        memcpy(addrs[found], r, 6);
      char name[32] = "";
      if (e[0] == 0x2f)
        eir_name(r + 14, 240, name, sizeof name);
      printf("hidprobe:   found %02x:%02x:%02x:%02x:%02x:%02x class %06x%s rssi %d '%s'\n", r[5], r[4], r[3], r[2],
             r[1], r[0], cls, (cls & 0x1f00) == 0x0500 ? " (peripheral)" : "", rssi, name);
      found++;
    } else {
      hex("other event", e, 2u + e[1]);
    }
  }
}

static void dongle(struct dev *d) {
  if (d->in < 0) {
    printf("hidprobe: dev %d: no event pipe\n", d->id);
    return;
  }
  u32 t0 = now();
  if (hci_cmd(d, 0x0c03, 0, 0, "reset"))
    return;
  printf("hidprobe:   reset: %u ms\n", (now() - t0) / TICKS_MS);
  /* the default mask leaves out Inquiry Result with RSSI (bit 33) and Extended
     Inquiry Result (bit 46), and the SSP events; reserved bits are refused
     (status 12): Linux's mask, 0x3DBFF807FFFBFFFF */
  static const u8 mask[8] = {0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d};
  hci_cmd(d, 0x0c01, mask, 8, "set event mask");
  u8 *p = d->ev + 6; /* after the status */
  if (!hci_cmd(d, 0x1001, 0, 0, "read local version"))
    printf("hidprobe:   HCI %u (Bluetooth %s) revision %04x, LMP %u subversion %04x, maker %u %s\n", p[0],
           bt_version(p[0]), p[1] | p[2] << 8, p[3], p[6] | p[7] << 8, p[4] | p[5] << 8, maker(p[4] | p[5] << 8));
  if (!hci_cmd(d, 0x1009, 0, 0, "read bd_addr"))
    mac("dongle address", p, 1);
  if (!hci_cmd(d, 0x1005, 0, 0, "read buffer size"))
    printf("hidprobe:   ACL %u bytes x %u, SCO %u bytes x %u\n", p[0] | p[1] << 8, p[3] | p[4] << 8, p[2],
           p[5] | p[6] << 8);
  if (!hci_cmd(d, 0x1003, 0, 0, "read local features")) {
    hex("features", p, 8);
    printf("hidprobe:   SSP %s, EDR 2M %s, LE %s\n", p[6] & 0x08 ? "yes" : "no", p[3] & 0x02 ? "yes" : "no",
           p[4] & 0x40 ? "yes" : "no");
  }
  if (!hci_cmd(d, 0x0c14, 0, 0, "read local name")) {
    p[247] = 0;
    printf("hidprobe:   name '%s'\n", (char *)p);
  }
  while (!stop_inquiry && !d->gone) /* again and again: time to put a pad in pairing mode */
    inquiry(d);
}

/* ---- the device's thread ---- */

static void dev_thread(void *arg) {
  struct dev *d = arg;
  dump_descriptors(d);
  open_pipes(d);
  int r = control(d, 0x00, 0x09, d->config, 0, 0, 0); /* SET_CONFIGURATION */
  printf("hidprobe: dev %d: %s %s, set configuration %u: %d\n", d->id, kind_name[d->kind], d->model, d->config, r);
  if (!r) {
    if (d->kind == K_BT)
      dongle(d);
    else
      pad(d);
  }
  printf("hidprobe: dev %d: done\n", d->id);
  ExitDeleteThread();
}

static int probe(int id) {
  UsbDeviceDescriptor *dd = sceUsbdScanStaticDescriptor(id, 0, USB_DT_DEVICE);
  UsbInterfaceDescriptor *i = first_interface(id);
  if (!dd || !i)
    return 0;
  int take = dd->bDeviceClass != USB_CLASS_HUB && i->bInterfaceClass != USB_CLASS_MASS_STORAGE &&
             i->bInterfaceClass != USB_CLASS_HUB;
  printf("hidprobe: probe %d: %04x:%04x class %02x, interface class %02x: %s\n", id, dd->idVendor, dd->idProduct,
         dd->bDeviceClass, i->bInterfaceClass, take ? "taken" : "left");
  return take;
}

static int connect(int id) {
  struct dev *d = 0;
  for (int i = 0; i < DEVS && !d; i++)
    if (!devs[i].used)
      d = &devs[i];
  if (!d) {
    printf("hidprobe: connect %d: no room\n", id);
    return -1;
  }
  iop_sema_t s = {0, 0, 0, 64};
  iop_thread_t t = {TH_C, 0, dev_thread, 0x1000, 0x30};
  memset(d, 0, sizeof *d);
  d->used = 1, d->id = id;
  d->kind = kind_of(id, &d->model);
  d->sema = CreateSema(&s);
  d->ctrl = sceUsbdOpenPipe(id, 0);
  t.attr = TH_C;
  sceUsbdSetPrivateData(id, d);
  StartThread(CreateThread(&t), d);
  return 0;
}

static int disconnect(int id) {
  struct dev *d = sceUsbdGetPrivateData(id);
  printf("hidprobe: dev %d unplugged\n", id);
  if (d)
    d->gone = 1;
  return 0;
}

static sceUsbdLddOps driver = {0, 0, "hidprobe", probe, connect, disconnect, 0, 0, 0, 0, 0, 0};

static void idle_thread(void *arg) {
  (void)arg;
  while (!stop) /* a measurement: counts what the IOP has left */
    idle_count++;
  ExitDeleteThread();
}

int _start(int argc, char **argv) {
  u32 secs = 30;
  if (argc > 1)
    secs = (u32)strtol(argv[1], 0, 10);
  timer = AllocHardTimer(1, 32, 1);
  if (timer < 0)
    return MODULE_NO_RESIDENT_END;
  SetTimerMode(timer, 0);
  iop_thread_t idl = {TH_C, 0, idle_thread, 0x200, 126};
  StartThread(CreateThread(&idl), 0);
  /* what the idle thread counts in a millisecond with nothing else running */
  u32 c0 = idle_count, t0 = now();
  DelayThread(1000 * 1000);
  u32 rate = (idle_count - c0) / ((now() - t0) / TICKS_MS);
  printf("hidprobe: %u s; plug in pads and dongles, one at a time\n", secs);
  c0 = idle_count, t0 = now();
  int r = sceUsbdRegisterLdd(&driver);
  if (r) {
    printf("hidprobe: register: %d (usbd not loaded?)\n", r);
    stop = 1;
    return MODULE_NO_RESIDENT_END;
  }
  if (secs < 10)
    secs = 10;
  DelayThread((secs - 7) * 1000 * 1000);
  stop_inquiry = 1;
  DelayThread(7 * 1000 * 1000);
  u32 ms = (now() - t0) / TICKS_MS, idle = idle_count - c0;
  stop = 1;
  DelayThread(1500 * 1000); /* the devices' threads end */
  printf("hidprobe: summary, IOP idle %u%% over %u s\n", rate ? idle / ms * 100 / rate : 0, ms / 1000);
  for (int i = 0; i < DEVS; i++) {
    struct dev *d = &devs[i];
    if (d->used)
      printf("hidprobe:   dev %d %s %s%s: %u reports, %u errors\n", d->id, kind_name[d->kind], d->model,
             d->gone ? " (unplugged)" : "", d->reports_total, d->errors);
  }
  sceUsbdUnregisterLdd(&driver);
  printf("hidprobe: end\n");
  return MODULE_RESIDENT_END;
}
