/* A Bluetooth dongle on USB (class E0/01/01), HCI over USB (docs/pad-bt-plan.md,
   step 2): commands on the control pipe, events on the interrupt pipe, ACL
   data on the bulk pipes.

   usbd's callbacks reassemble events and ACL packets from the USB packets
   (an event comes in pieces of the interrupt pipe's size) and put them in a
   ring; one thread takes them from there. Commands wait in a queue and go
   out one at a time, as the controller's command credits allow.

   The dongle is set up for pads: page scan on (a paired pad connects when
   its PS button is pressed), connections accepted, link keys from a table
   (written at pairing over USB: mxpad.c) or new ones kept (SSP's "just
   works"). ACL data is only shown yet: L2CAP and HID are step 3. */

#include "irx_imports.h"
#include "usbd_macro.h"

#include "mxpad_int.h"

#include <mxpad.h>

#define LOG(...) printf("mxpad: bt: " __VA_ARGS__)

/* ---- the dongle ---- */

static struct {
  int id, used, gone, ready;
  int ctrl, ev_in, acl_in, acl_out;
  u16 ev_size, acl_size;
  u8 addr[6];
  u16 acl_mtu, acl_free;
  int sema;
  u32 gen;
} bt;
static u32 bt_generation;
static void (*pair_all)(void);

/* ---- what came: a ring of whole packets ---- */

#define RX_SLOTS 8
#define RX_SIZE 320
enum { PKT_EVENT = 4, PKT_ACL = 2 };
static struct {
  u8 type;
  u16 len;
  u8 data[RX_SIZE];
} rx[RX_SLOTS];
static volatile u32 rx_head, rx_tail, rx_drops;

static u8 ev_piece[64] __attribute__((aligned(16)));
static u8 ev_buf[260];
static u32 ev_have;
static u8 acl_piece[64] __attribute__((aligned(16)));
static u8 acl_buf[RX_SIZE];
static u32 acl_have;

static void rx_put(u8 type, const u8 *p, u32 n) {
  int st;
  CpuSuspendIntr(&st);
  if (rx_head - rx_tail >= RX_SLOTS || n > RX_SIZE) {
    rx_drops++;
  } else {
    u32 i = rx_head % RX_SLOTS;
    rx[i].type = type, rx[i].len = (u16)n;
    memcpy(rx[i].data, p, n);
    rx_head++;
  }
  CpuResumeIntr(st);
  SignalSema(bt.sema);
}

#define TAG ((void *)bt.gen)

static void ev_cb(int result, int bytes, void *arg);
static void acl_cb(int result, int bytes, void *arg);

static void ev_next(void) {
  if (!bt.gone)
    sceUsbdInterruptTransfer(bt.ev_in, ev_piece, bt.ev_size, ev_cb, TAG);
}

static void acl_next(void) {
  if (!bt.gone)
    sceUsbdBulkTransfer(bt.acl_in, acl_piece, bt.acl_size, acl_cb, TAG);
}

static void ev_cb(int result, int bytes, void *arg) {
  if ((u32)arg != bt.gen || bt.gone)
    return;
  if (result == USB_RC_OK && bytes > 0) {
    if (ev_have + (u32)bytes > sizeof ev_buf)
      ev_have = 0; /* lost track: start again */
    memcpy(ev_buf + ev_have, ev_piece, (u32)bytes);
    ev_have += (u32)bytes;
    if (ev_have >= 2 && ev_have >= 2u + ev_buf[1]) {
      rx_put(PKT_EVENT, ev_buf, 2u + ev_buf[1]);
      ev_have = 0;
    }
  }
  ev_next();
}

static void acl_cb(int result, int bytes, void *arg) {
  if ((u32)arg != bt.gen || bt.gone)
    return;
  if (result == USB_RC_OK && bytes > 0) {
    if (acl_have + (u32)bytes > sizeof acl_buf)
      acl_have = 0;
    memcpy(acl_buf + acl_have, acl_piece, (u32)bytes);
    acl_have += (u32)bytes;
    if (acl_have >= 4 && acl_have >= 4u + (acl_buf[2] | acl_buf[3] << 8)) {
      rx_put(PKT_ACL, acl_buf, 4u + (acl_buf[2] | acl_buf[3] << 8));
      acl_have = 0;
    }
  }
  acl_next();
}

/* ---- commands ---- */

#define CMD_SLOTS 8
static struct {
  u16 op;
  u8 len;
  u8 par[255];
} cmdq[CMD_SLOTS];
static u32 cmd_head, cmd_tail;
static int credits = 1;
static u8 cmd_out[260] __attribute__((aligned(16)));

static void cmd_sent(int result, int bytes, void *arg) {
  (void)bytes;
  if ((u32)arg == bt.gen && result != USB_RC_OK)
    LOG("a command was not sent (%d)\n", result);
}

/* the next command, if the controller takes one: from the thread only */
static void cmd_kick(void) {
  if (credits <= 0 || cmd_head == cmd_tail || bt.gone)
    return;
  u32 i = cmd_tail++ % CMD_SLOTS;
  cmd_out[0] = (u8)cmdq[i].op, cmd_out[1] = (u8)(cmdq[i].op >> 8), cmd_out[2] = cmdq[i].len;
  memcpy(cmd_out + 3, cmdq[i].par, cmdq[i].len);
  credits--;
  sceUsbdControlTransfer(bt.ctrl, 0x20, 0x00, 0, 0, 3 + cmdq[i].len, cmd_out, cmd_sent, TAG);
}

static void cmd(u16 op, const void *par, u8 len) {
  if (cmd_head - cmd_tail >= CMD_SLOTS) {
    LOG("command queue full: %04x dropped\n", op);
    return;
  }
  u32 i = cmd_head++ % CMD_SLOTS;
  cmdq[i].op = op, cmdq[i].len = len;
  if (len)
    memcpy(cmdq[i].par, par, len);
  cmd_kick();
}

/* opcodes */
#define OP_ACCEPT_CONN 0x0409
#define OP_LINK_KEY_REPLY 0x040B
#define OP_LINK_KEY_NEG 0x040C
#define OP_PIN_NEG 0x040E
#define OP_IO_CAP_REPLY 0x042B
#define OP_USER_CONFIRM_REPLY 0x042C
#define OP_DEFAULT_LINK_POLICY 0x080F
#define OP_SET_EVENT_MASK 0x0C01
#define OP_RESET 0x0C03
#define OP_WRITE_LOCAL_NAME 0x0C13
#define OP_WRITE_SCAN_ENABLE 0x0C1A
#define OP_WRITE_CLASS 0x0C24
#define OP_WRITE_SSP_MODE 0x0C56
#define OP_READ_BUFFER_SIZE 0x1005
#define OP_READ_BD_ADDR 0x1009

static void setup(void) {
  /* Linux's mask: the default leaves out Inquiry Result with RSSI and Extended
     Inquiry Result; reserved bits are refused (docs/pad-bt-plan.md) */
  static const u8 mask[8] = {0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d};
  static const u8 cls[3] = {0x08, 0x05, 0x00}; /* PS3-like: major 'peripheral', minor 'gamepad' */
  static const u8 one = 1, page_scan = 0x02, policy[2] = {0x05, 0x00}; /* role switch, sniff */
  static u8 name[248] = "MXOS";
  cmd(OP_RESET, 0, 0);
  cmd(OP_SET_EVENT_MASK, mask, 8);
  cmd(OP_READ_BD_ADDR, 0, 0);
  cmd(OP_READ_BUFFER_SIZE, 0, 0);
  cmd(OP_WRITE_CLASS, cls, 3);
  cmd(OP_WRITE_LOCAL_NAME, name, 248);
  cmd(OP_WRITE_SSP_MODE, &one, 1);    /* refused by a 2.0 dongle: fine */
  cmd(OP_DEFAULT_LINK_POLICY, policy, 2);
  cmd(OP_WRITE_SCAN_ENABLE, &page_scan, 1); /* the last: then the dongle is ready */
}

/* ---- link keys ---- */

#define KEYS 8
static struct {
  u8 addr[6], key[16], used, has_key, kind;
} keys[KEYS];
static u32 key_next;
static volatile int keys_dirty;

static int key_find(const u8 *addr) {
  for (int i = 0; i < KEYS; i++)
    if (keys[i].used && !memcmp(keys[i].addr, addr, 6))
      return i;
  return -1;
}

/* key 0: a pad without one (the DS3); kind 0: as it was */
static void key_put(const u8 *addr, const u8 *key, int kind) {
  int i = key_find(addr);
  if (i < 0) {
    i = (int)(key_next++ % KEYS);
    keys[i].kind = 0;
  }
  memcpy(keys[i].addr, addr, 6);
  keys[i].has_key = key != 0;
  if (key)
    memcpy(keys[i].key, key, 16);
  if (kind)
    keys[i].kind = (u8)kind;
  keys[i].used = 1;
  keys_dirty = 1;
  if (bt.used && !bt.gone)
    SignalSema(bt.sema); /* the thread writes the card */
}

void bt_known(const u8 addr[6], int kind) { key_put(addr, 0, kind); }

/* ---- the pairings on the memory card ----
   mc0:/MXOS/BTPADS.DAT: "MXBT", a version, the count, then per pad its
   address (HCI byte order), kind, whether it has a key, and the key. Read
   when a dongle comes up, written by the thread after a new key. */

#define KEYS_FILE "mc0:/MXOS/BTPADS.DAT"
#define KEYS_VERSION 1
#define KEY_REC 24
static u8 keys_io[4 + 2 + KEYS * KEY_REC];

static void keys_load(void) {
  int fd = open(KEYS_FILE, O_RDONLY);
  if (fd < 0)
    return;
  int n = read(fd, keys_io, sizeof keys_io), loaded = 0;
  close(fd);
  if (n < 6 || memcmp(keys_io, "MXBT", 4) || keys_io[4] != KEYS_VERSION)
    return;
  for (u32 i = 0; i < keys_io[5] && i < KEYS && 6 + (i + 1) * KEY_REC <= (u32)n; i++) {
    const u8 *r = keys_io + 6 + i * KEY_REC;
    key_put(r, r[7] ? r + 8 : 0, r[6]);
    loaded++;
  }
  keys_dirty = 0;
  LOG("%d paired pads from %s\n", loaded, KEYS_FILE);
}

static void keys_save(void) {
  u32 n = 0;
  keys_dirty = 0;
  memcpy(keys_io, "MXBT", 4);
  keys_io[4] = KEYS_VERSION;
  for (int i = 0; i < KEYS; i++) {
    if (!keys[i].used)
      continue;
    u8 *r = keys_io + 6 + n * KEY_REC;
    memcpy(r, keys[i].addr, 6);
    r[6] = keys[i].kind, r[7] = keys[i].has_key;
    memcpy(r + 8, keys[i].key, 16);
    n++;
  }
  keys_io[5] = (u8)n;
  int fd = open(KEYS_FILE, O_WRONLY | O_CREAT | O_TRUNC);
  int w = fd < 0 ? fd : write(fd, keys_io, (int)(6 + n * KEY_REC));
  if (fd >= 0)
    close(fd);
  if (w != (int)(6 + n * KEY_REC))
    LOG("%s not written (%d)\n", KEYS_FILE, w);
}

void bt_new_key(const u8 addr[6], u8 key[16]) {
  iop_sys_clock_t c;
  GetSystemTime(&c);
  u32 x = c.lo ^ c.hi * 2654435761u;
  for (int i = 0; i < 16; i++) {
    x ^= (u32)addr[i % 6] << (i % 4 * 8);
    x = x * 1664525u + 1013904223u;
    key[i] = (u8)(x >> 24);
  }
  key_put(addr, key, MXPAD_DS4);
}

int bt_host(u8 addr[6]) {
  if (!bt.used || bt.gone || !bt.ready)
    return 0;
  memcpy(addr, bt.addr, 6);
  return 1;
}

/* ---- connections: L2CAP and HID (step 3) ---- */

#define CONNS 4
#define CID_CTRL 0x0040 /* ours */
#define CID_INTR 0x0041
#define PSM_CTRL 0x0011
#define PSM_INTR 0x0013
/* a channel's configuration: ours acknowledged, theirs answered */
#define CFG_OUT 1
#define CFG_IN 2

static struct conn {
  u16 handle, used;
  u8 addr[6];
  int kind, slot;
  u16 ctrl, intr;   /* the pad's channel ids, 0: not open */
  u8 ctrl_cfg, intr_cfg, started, id;
  u8 led_wanted; /* the DS3's light, after the pad answered its start (HID handshake) */
} conns[CONNS];

static void open_channel(struct conn *c, u16 psm, u16 ours);

static void conn_open(u16 handle, const u8 *addr) {
  struct conn *c = 0;
  for (int i = 0; i < CONNS && !c; i++)
    if (!conns[i].used)
      c = &conns[i];
  if (!c) {
    LOG("no room for another connection\n");
    return;
  }
  memset(c, 0, sizeof *c);
  c->used = 1, c->handle = handle, c->slot = -1, c->id = 0x10;
  memcpy(c->addr, addr, 6);
  int k = key_find(addr);
  c->kind = k < 0 ? 0 : keys[k].kind;
  if (c->kind == MXPAD_DS4)
    open_channel(c, PSM_CTRL, CID_CTRL); /* the DS4 waits for the host to open them */
}

static struct conn *conn_of(u16 handle) {
  for (int i = 0; i < CONNS; i++)
    if (conns[i].used && conns[i].handle == handle)
      return &conns[i];
  return 0;
}

static void conn_close(u16 handle) {
  struct conn *c = conn_of(handle);
  if (!c)
    return;
  if (c->slot >= 0) {
    LOG("slot %d: gone\n", c->slot);
    mxpad_close(c->slot);
  }
  c->used = 0;
}

/* ---- ACL out: from the thread only, one at a time ---- */

static u8 acl_out_buf[4 + 4 + 96] __attribute__((aligned(16)));
static int out_sema;
static volatile int out_done, out_timeout, out_result;

static unsigned int out_alarm(void *arg) {
  (void)arg;
  out_timeout = 1;
  iSignalSema(out_sema);
  return 0;
}

static void out_cb(int result, int bytes, void *arg) {
  (void)bytes;
  if ((u32)arg != bt.gen)
    return;
  out_result = result, out_done = 1;
  SignalSema(out_sema);
}

/* an L2CAP packet of n bytes to channel cid */
static int l2cap_send(struct conn *c, u16 cid, const u8 *d, u32 n) {
  if (n > sizeof acl_out_buf - 8 || bt.gone)
    return -1;
  u8 *b = acl_out_buf;
  u16 h = c->handle | 0x2000; /* packet boundary: first, flushable */
  b[0] = (u8)h, b[1] = (u8)(h >> 8), b[2] = (u8)(n + 4), b[3] = (u8)((n + 4) >> 8);
  b[4] = (u8)n, b[5] = (u8)(n >> 8), b[6] = (u8)cid, b[7] = (u8)(cid >> 8);
  memcpy(b + 8, d, n);
  out_done = out_timeout = 0;
  if (sceUsbdBulkTransfer(bt.acl_out, b, 8 + n, out_cb, TAG) != USB_RC_OK)
    return -1;
  iop_sys_clock_t clk;
  USec2SysClock(500 * 1000, &clk);
  SetAlarm(&clk, out_alarm, 0);
  while (!out_done && !out_timeout)
    WaitSema(out_sema);
  CancelAlarm(out_alarm, 0);
  return out_done && out_result == USB_RC_OK ? 0 : -1;
}

/* a signalling command: code, a new or the given id, its data */
static void sig(struct conn *c, u8 code, u8 id, const u8 *d, u16 n) {
  u8 b[4 + 16];
  b[0] = code, b[1] = id ? id : ++c->id, b[2] = (u8)n, b[3] = (u8)(n >> 8);
  memcpy(b + 4, d, n);
  l2cap_send(c, 0x0001, b, 4u + n);
}

static void config_req(struct conn *c, u16 their) {
  /* the MTU we take: 672, L2CAP's default for these pads */
  u8 d[8] = {(u8)their, (u8)(their >> 8), 0, 0, 0x01, 0x02, (u8)672, (u8)(672 >> 8)};
  sig(c, 0x04, 0, d, 8);
}

static void open_channel(struct conn *c, u16 psm, u16 ours) {
  u8 d[4] = {(u8)psm, (u8)(psm >> 8), (u8)ours, (u8)(ours >> 8)};
  sig(c, 0x02, 0, d, 4);
}

/* the CRC of a DS4's or DualSense's report over Bluetooth (Linux's: CRC-32,
   with the transaction header first) */
static u32 crc32(u32 crc, const u8 *p, u32 n) {
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++)
      crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
  }
  return ~crc;
}

/* both channels open and configured: the pad's input, and its light */
static void hid_start(struct conn *c) {
  u8 rgb[3];
  c->started = 1;
  c->slot = mxpad_open(c->kind);
  if (c->slot < 0) {
    LOG("no slot free\n");
    return;
  }
  mxpad_slot_rgb(c->slot, rgb);
  if (c->kind == MXPAD_DS3) {
    /* SET_REPORT feature 0xF4 over Bluetooth: 42 03 */
    static const u8 start[6] = {0x53, 0xf4, 0x42, 0x03, 0x00, 0x00};
    l2cap_send(c, c->ctrl, start, 6);
    /* the light once the pad answered: one transaction at a time on the
       control channel, as Linux's hidp waits for each handshake */
    c->led_wanted = 1;
  } else if (c->kind == MXPAD_DS4) {
    /* GET_REPORT feature 0x02: the DS4 sends its full report 0x11 from then on */
    static const u8 get2[2] = {0x43, 0x02};
    l2cap_send(c, c->ctrl, get2, 2);
    /* output report 0x11 on the interrupt channel (Linux's hid-playstation):
       id, hardware control (HID, CRC), audio control, then the USB report 5's
       bytes from its flags on: motors at 6, 7, the light at 8-10 (o[] has the
       transaction header first); the CRC over header and report */
    u8 o[79] = {0xa2, 0x11, 0xc0, 0x00, 0x07};
    o[9] = rgb[0], o[10] = rgb[1], o[11] = rgb[2];
    u32 crc = crc32(0, o, 75);
    o[75] = (u8)crc, o[76] = (u8)(crc >> 8), o[77] = (u8)(crc >> 16), o[78] = (u8)(crc >> 24);
    l2cap_send(c, c->intr, o, sizeof o);
  }
  LOG("slot %d: %s over Bluetooth\n", c->slot, c->kind == MXPAD_DS3 ? "DualShock 3" : "DualShock 4");
}

/* the DS3's light: SET_REPORT output 1, 36 bytes with the id, as Linux's
   hid-sony sends it (48, the USB length, made a DS3 hang over Bluetooth) */
static void ds3_led(struct conn *c) {
  u8 o[1 + 36] = {0x52, 0x01};
  o[2 + 9] = (u8)(0x02 << c->slot);
  for (int i = 0; i < 4; i++) {
    u8 *l = o + 2 + 10 + 5 * i;
    l[0] = 0xff, l[1] = 0x27, l[2] = 0x10, l[3] = 0, l[4] = 0x32;
  }
  l2cap_send(c, c->ctrl, o, sizeof o);
}

static void check_ready(struct conn *c) {
  if (c->started || !c->kind)
    return;
  if (c->ctrl_cfg == (CFG_OUT | CFG_IN) && !c->intr && c->kind == MXPAD_DS4)
    open_channel(c, PSM_INTR, CID_INTR); /* the DS4 waits for the host to open it */
  if (c->ctrl_cfg == (CFG_OUT | CFG_IN) && c->intr_cfg == (CFG_OUT | CFG_IN))
    hid_start(c);
}

static void signalling(struct conn *c, const u8 *s, u32 n) {
  while (n >= 4) {
    u8 code = s[0], id = s[1];
    u16 len = (u16)(s[2] | s[3] << 8);
    const u8 *d = s + 4;
    if (4u + len > n)
      return;
    switch (code) {
    case 0x02: { /* Connection Request: psm, their cid */
      u16 psm = (u16)(d[0] | d[1] << 8), their = (u16)(d[2] | d[3] << 8);
      u16 ours = psm == PSM_CTRL ? CID_CTRL : psm == PSM_INTR ? CID_INTR : 0;
      u8 r[8] = {(u8)ours, (u8)(ours >> 8), (u8)their, (u8)(their >> 8), 0, 0, 0, 0};
      if (psm == 0x0001 && c->kind) {
        /* SDP: a DS4 asks the host first; a PS4 lets it wait and opens the HID
           channels itself (conn_open), and so does this */
        break;
      }
      if (!ours || !c->kind) {
        r[4] = 0x02; /* refused: PSM not supported (or a pad we do not know) */
        LOG("channel to psm %04x refused\n", psm);
      } else if (psm == PSM_CTRL) {
        c->ctrl = their, c->ctrl_cfg = 0;
      } else {
        c->intr = their, c->intr_cfg = 0;
      }
      sig(c, 0x03, id, r, 8);
      if (ours && c->kind) {
        LOG("channel %04x opened by the pad (its %04x)\n", ours, their);
        config_req(c, their);
      }
      break;
    }
    case 0x03: { /* Connection Response to ours: their cid, ours, result */
      u16 their = (u16)(d[0] | d[1] << 8), ours = (u16)(d[2] | d[3] << 8), result = (u16)(d[4] | d[5] << 8);
      if (result == 1)
        break; /* pending */
      if (result) {
        LOG("channel %04x refused: %u\n", ours, result);
        break;
      }
      if (ours == CID_INTR)
        c->intr = their, c->intr_cfg = 0;
      else if (ours == CID_CTRL)
        c->ctrl = their, c->ctrl_cfg = 0;
      LOG("channel %04x open (the pad's %04x)\n", ours, their);
      config_req(c, their);
      break;
    }
    case 0x04: { /* Configuration Request for our channel: all is taken */
      u16 ours = (u16)(d[0] | d[1] << 8);
      u16 their = ours == CID_CTRL ? c->ctrl : c->intr;
      u8 r[6] = {(u8)their, (u8)(their >> 8), 0, 0, 0, 0};
      sig(c, 0x05, id, r, 6);
      LOG("configuration of %04x by the pad: taken\n", ours);
      if (ours == CID_CTRL)
        c->ctrl_cfg |= CFG_IN;
      else if (ours == CID_INTR)
        c->intr_cfg |= CFG_IN;
      check_ready(c);
      break;
    }
    case 0x05: { /* Configuration Response to ours */
      u16 ours = (u16)(d[0] | d[1] << 8), result = (u16)(d[4] | d[5] << 8);
      LOG("configuration of %04x: %u\n", ours, result);
      if (ours == CID_CTRL)
        c->ctrl_cfg |= CFG_OUT;
      else if (ours == CID_INTR)
        c->intr_cfg |= CFG_OUT;
      check_ready(c);
      break;
    }
    case 0x06: { /* Disconnection Request: ours, theirs */
      u8 r[4] = {d[0], d[1], d[2], d[3]};
      sig(c, 0x07, id, r, 4);
      if ((d[0] | d[1] << 8) == CID_INTR)
        c->intr = 0;
      else
        c->ctrl = 0;
      break;
    }
    case 0x08: /* Echo Request */
      sig(c, 0x09, id, d, len > 16 ? 16 : len);
      break;
    case 0x0a: { /* Information Request: not supported */
      u8 r[4] = {d[0], d[1], 0x01, 0x00};
      sig(c, 0x0b, id, r, 4);
      break;
    }
    case 0x01: /* Command Reject */
      LOG("command rejected: reason %u\n", d[0] | d[1] << 8);
      break;
    }
    s += 4u + len, n -= 4u + len;
  }
}

static u8 report[64];

/* an input report on the interrupt channel: 0xA1, then the report */
static void hid_input(struct conn *c, const u8 *d, u32 n) {
  if (c->slot < 0 || n < 2 || d[0] != 0xa1)
    return;
  if (c->kind == MXPAD_DS3 && d[1] == 0x01) {
    mxpad_input(c->slot, d + 1, n - 1); /* as over USB */
  } else if (c->kind == MXPAD_DS4 && d[1] == 0x11 && n >= 4 + 63) {
    /* report 0x11: id, two bytes, then report 0x01's bytes after its id */
    report[0] = 0x01;
    memcpy(report + 1, d + 4, 63);
    mxpad_input(c->slot, report, 64);
  }
}

/* ---- events ---- */

static void addr_str(const u8 *a, char *s) {
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < 6; i++) {
    u8 b = a[5 - i];
    s[i * 3] = hx[b >> 4], s[i * 3 + 1] = hx[b & 15], s[i * 3 + 2] = i < 5 ? ':' : 0;
  }
}

static void complete(u16 op, const u8 *r, u32 n) {
  u8 status = n ? r[0] : 0xff;
  if (status && op != OP_WRITE_SSP_MODE)
    LOG("command %04x: status %02x\n", op, status);
  switch (op) {
  case OP_READ_BD_ADDR:
    if (!status && n >= 7)
      memcpy(bt.addr, r + 1, 6);
    break;
  case OP_READ_BUFFER_SIZE:
    if (!status && n >= 8)
      bt.acl_mtu = (u16)(r[1] | r[2] << 8), bt.acl_free = (u16)(r[4] | r[5] << 8);
    break;
  case OP_WRITE_SCAN_ENABLE:
    if (!status && !bt.ready) {
      char a[18];
      addr_str(bt.addr, a);
      bt.ready = 1;
      LOG("ready: %s, ACL %u bytes x %u, page scan on\n", a, bt.acl_mtu, bt.acl_free);
      if (pair_all)
        pair_all();
    }
    break;
  }
}

static void event(const u8 *e, u32 n) {
  u8 code = e[0];
  const u8 *p = e + 2;
  char a[18];
  switch (code) {
  case 0x0e: /* Command Complete */
    credits = p[0];
    complete((u16)(p[1] | p[2] << 8), p + 3, n - 5);
    break;
  case 0x0f: /* Command Status */
    credits = p[1];
    if (p[0])
      LOG("command %04x: status %02x\n", p[2] | p[3] << 8, p[0]);
    break;
  case 0x04: { /* Connection Request: accept, we become master */
    addr_str(p, a);
    LOG("connection request from %s, class %02x%02x%02x, link %u\n", a, p[8], p[7], p[6], p[9]);
    u8 par[7];
    memcpy(par, p, 6);
    par[6] = 0x00;
    cmd(OP_ACCEPT_CONN, par, 7);
    break;
  }
  case 0x03: /* Connection Complete */
    addr_str(p + 3, a);
    LOG("connected %s: status %02x, handle %03x, encryption %u\n", a, p[0], (p[1] | p[2] << 8) & 0xfff, p[10]);
    if (!p[0])
      conn_open((u16)((p[1] | p[2] << 8) & 0xfff), p + 3);
    break;
  case 0x05: /* Disconnection Complete */
    LOG("disconnected: handle %03x, reason %02x\n", (p[1] | p[2] << 8) & 0xfff, p[3]);
    conn_close((u16)((p[1] | p[2] << 8) & 0xfff));
    break;
  case 0x17: { /* Link Key Request */
    addr_str(p, a);
    int i = key_find(p);
    LOG("link key request from %s: %s\n", a, i < 0 || !keys[i].has_key ? "none" : "known");
    if (i < 0 || !keys[i].has_key) {
      cmd(OP_LINK_KEY_NEG, p, 6);
    } else {
      u8 par[22];
      memcpy(par, p, 6);
      memcpy(par + 6, keys[i].key, 16);
      cmd(OP_LINK_KEY_REPLY, par, 22);
    }
    break;
  }
  case 0x18: /* Link Key Notification: keep it */
    addr_str(p, a);
    LOG("new link key for %s, type %u\n", a, p[22]);
    key_put(p, p + 6, 0);
    break;
  case 0x16: /* PIN Code Request: pads use none */
    addr_str(p, a);
    LOG("PIN request from %s: refused\n", a);
    cmd(OP_PIN_NEG, p, 6);
    break;
  case 0x31: { /* IO Capability Request: no input, no output: "just works" */
    u8 par[9];
    memcpy(par, p, 6);
    par[6] = 0x03, par[7] = 0x00, par[8] = 0x04; /* NoInputNoOutput, no OOB, general bonding */
    cmd(OP_IO_CAP_REPLY, par, 9);
    break;
  }
  case 0x33: /* User Confirmation Request */
    cmd(OP_USER_CONFIRM_REPLY, p, 6);
    break;
  case 0x36: /* Simple Pairing Complete */
    addr_str(p + 1, a);
    LOG("pairing with %s: status %02x\n", a, p[0]);
    break;
  case 0x06: /* Authentication Complete */
    LOG("authentication: handle %03x, status %02x\n", (p[1] | p[2] << 8) & 0xfff, p[0]);
    break;
  case 0x08: /* Encryption Change */
    LOG("encryption: handle %03x, status %02x, on %u\n", (p[1] | p[2] << 8) & 0xfff, p[0], p[3]);
    break;
  case 0x13: /* Number Of Completed Packets */
    for (u32 i = 0; i < p[0]; i++)
      bt.acl_free += (u16)(p[3 + i * 4] | p[4 + i * 4] << 8);
    break;
  case 0x10: /* Hardware Error: some dongles say so before the first reset */
    LOG("hardware error %02x\n", p[0]);
    break;
  case 0x12: /* Role Change */
  case 0x1b: /* Max Slots Change */
  case 0x20: /* Page Scan Repetition Mode Change */
  case 0x0b: /* Read Remote Supported Features */
  case 0x0c: /* Read Remote Version */
  case 0x0d: /* QoS Setup */
  case 0x32: /* IO Capability Response */
  case 0x3d: /* Remote Host Supported Features */
  case 0xff: /* the maker's */
    break;
  default:
    LOG("event %02x, %u bytes\n", code, n);
  }
}

static void acl(const u8 *d, u32 n) {
  if (n < 8)
    return;
  u16 handle = (u16)((d[0] | d[1] << 8) & 0xfff);
  u32 len = (u32)(d[4] | d[5] << 8), cid = (u32)(d[6] | d[7] << 8);
  struct conn *c = conn_of(handle);
  if (!c || 8 + len > n)
    return; /* a continuation, or a link we do not know: not from these pads */
  if (cid == 0x0001)
    signalling(c, d + 8, len);
  else if (cid == CID_INTR)
    hid_input(c, d + 8, len);
  else if (cid == CID_CTRL && len >= 1 && (d[8] & 0xf0) == 0x00) {
    /* a HID handshake: the answer to our last SET_REPORT */
    if (d[8] != 0x00)
      LOG("handshake %02x\n", d[8]);
    if (c->led_wanted) {
      c->led_wanted = 0;
      ds3_led(c);
    }
  }
  /* other answers on the control channel (GET_REPORT's data): not needed */
}

static void bt_thread(void *arg) {
  (void)arg;
  keys_load();
  setup();
  while (!bt.gone) {
    WaitSema(bt.sema);
    if (keys_dirty)
      keys_save();
    while (rx_tail != rx_head && !bt.gone) {
      u32 i = rx_tail % RX_SLOTS;
      if (rx[i].type == PKT_EVENT)
        event(rx[i].data, rx[i].len);
      else
        acl(rx[i].data, rx[i].len);
      rx_tail++;
    }
    cmd_kick();
  }
  LOG("dongle gone\n");
  for (int i = 0; i < CONNS; i++)
    if (conns[i].used)
      conn_close(conns[i].handle);
  DeleteSema(bt.sema);
  bt.used = 0;
  ExitDeleteThread();
}

/* ---- usbd ---- */

static UsbInterfaceDescriptor *first_interface(int id, UsbConfigDescriptor **cp) {
  UsbDeviceDescriptor *d = sceUsbdScanStaticDescriptor(id, 0, USB_DT_DEVICE);
  UsbConfigDescriptor *c = d ? sceUsbdScanStaticDescriptor(id, d, USB_DT_CONFIG) : 0;
  *cp = c;
  if (!c)
    return 0;
  u8 *q = (u8 *)c, *end = q + c->wTotalLength;
  for (; q < end && q[0] >= 2; q += q[0])
    if (q[1] == USB_DT_INTERFACE)
      return (UsbInterfaceDescriptor *)q;
  return 0;
}

static int bt_probe(int id) {
  UsbConfigDescriptor *c;
  UsbInterfaceDescriptor *i = first_interface(id, &c);
  return !bt.used && i && i->bInterfaceClass == 0xe0 && i->bInterfaceSubClass == 1 && i->bInterfaceProtocol == 1;
}

static void config_cb(int result, int bytes, void *arg) {
  (void)bytes;
  if ((u32)arg != bt.gen)
    return;
  if (result != USB_RC_OK) {
    LOG("set configuration: %d\n", result);
    return;
  }
  ev_next();
  acl_next();
  iop_thread_t t = {TH_C, 0, bt_thread, 0x1000, 0x28};
  int th = CreateThread(&t);
  if (th > 0)
    StartThread(th, 0);
}

static int bt_connect(int id) {
  UsbConfigDescriptor *c;
  UsbInterfaceDescriptor *intf = first_interface(id, &c);
  memset(&bt, 0, sizeof bt);
  bt.used = 1, bt.id = id, bt.gen = ++bt_generation;
  bt.ev_in = bt.acl_in = bt.acl_out = -1;
  bt.ctrl = sceUsbdOpenPipe(id, 0);
  u8 *q = (u8 *)intf, *end = (u8 *)c + c->wTotalLength;
  for (q += q[0]; q < end && q[0] >= 2 && q[1] != USB_DT_INTERFACE; q += q[0]) {
    if (q[1] != USB_DT_ENDPOINT)
      continue;
    UsbEndpointDescriptor *e = (UsbEndpointDescriptor *)q;
    int in = (e->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN;
    u16 size = (u16)(e->wMaxPacketSizeLB | e->wMaxPacketSizeHB << 8);
    if (e->bmAttributes == USB_ENDPOINT_XFER_INT && in && bt.ev_in < 0)
      bt.ev_in = sceUsbdOpenPipe(id, e), bt.ev_size = size > 64 ? 64 : size;
    else if (e->bmAttributes == USB_ENDPOINT_XFER_BULK && in && bt.acl_in < 0)
      bt.acl_in = sceUsbdOpenPipe(id, e), bt.acl_size = size > 64 ? 64 : size;
    else if (e->bmAttributes == USB_ENDPOINT_XFER_BULK && !in && bt.acl_out < 0)
      bt.acl_out = sceUsbdOpenPipeAligned(id, e);
  }
  if (bt.ev_in < 0 || bt.acl_in < 0 || bt.acl_out < 0) {
    LOG("dongle without its pipes\n");
    bt.used = 0;
    return -1;
  }
  iop_sema_t s = {0, 0, 0, 256};
  bt.sema = CreateSema(&s);
  if (out_sema <= 0) {
    iop_sema_t o = {0, 0, 0, 64};
    out_sema = CreateSema(&o);
  }
  rx_head = rx_tail = 0, ev_have = acl_have = 0;
  cmd_head = cmd_tail = 0, credits = 1;
  LOG("dongle (usb %d)\n", id);
  sceUsbdSetConfiguration(bt.ctrl, c->bConfigurationValue, config_cb, TAG);
  return 0;
}

static int bt_disconnect(int id) {
  (void)id;
  bt.gone = 1, bt.ready = 0;
  SignalSema(bt.sema);
  return 0;
}

static sceUsbdLddOps bt_driver = {0, 0, "mxpad-bt", bt_probe, bt_connect, bt_disconnect, 0, 0, 0, 0, 0, 0};

void bt_init(void (*pair)(void)) {
  pair_all = pair;
  if (sceUsbdRegisterLdd(&bt_driver) != USB_RC_OK)
    LOG("not registered\n");
}
