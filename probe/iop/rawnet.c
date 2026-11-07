/* What the SMAP driver can do without a TCP/IP stack: this module is the
   network stack of netman, and only counts and sends frames. It answers ARP
   and the commands of rawnet_host.py on UDP port 18766, and reports there too,
   as the IOP has no console then: netman takes one stack, so this needs an IOP
   without ps2ip, and p26_rawnet.c resets the IOP for it.

   Command M 1 sends each data frame on to the EE by SIF DMA, to the ring whose
   address is the module's argument: what a file transfer past the stack would
   do. The ring's slots are the receive buffers, as DMA runs on after the frame.

   The start thread watches a deadline and the module stays resident (netman
   keeps its functions); the program then ends and mxlink resets the IOP.

   All frames are handled in the driver's thread.

   Built with RAWNET_MXETH (rawnet_mx.c) it is a client of maxsdk's mxeth
   instead (docs/iop-drivers.md), loaded after mxdev9 and mxeth: the same
   commands, so both drivers are measured alike. */

#ifdef RAWNET_MXETH
#include "rawnet_mx_imports.h"
static void kick_tx(void);
#define XMIT() kick_tx()
#else
#include "rawnet_imports.h"
#define XMIT() NetManNetIFXmit()
#endif

IRX_ID("rawnet_probe", 1, 0);

#define PORT 18766
#define TICKS_S 36864000u
#define DEADLINE_MS 45000u
#define FRAME 1514u /* the largest: 1472 bytes of UDP data */
#define HDR 42u

static const u8 my_ip[4] = {192, 168, 1, 10}; /* mxlink's default */
static u8 my_mac[6], peer_mac[6], peer_ip[4];
static u32 peer_port;

#define SLOTS 16
#define SLOT 2048u
#ifndef RAWNET_MXETH
static u8 rxbuf[SLOTS][SLOT] __attribute__((aligned(16)));
#endif
static int dma_id[SLOTS];
static u32 slot, sif_mode, ee_ring, waits, drops;
static u8 reply[192] __attribute__((aligned(16)));
static u8 blast[1536] __attribute__((aligned(16)));
static u32 rx_len, reply_len, tx_left, tx_sent;

static int timer;
static volatile int quit;
static volatile u32 idle_count;
static volatile int idle_stop;
static u32 idle_rate; /* counts per ms */
static u32 t0, n0, frames, bytes;

#define now() ((u32)GetTimerCounter(timer))

static void idle(void *arg) {
  (void)arg;
  while (!idle_stop)
    idle_count++;
  ExitDeleteThread();
}

static u32 be16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }
static void put16(u8 *p, u32 v) { p[0] = (u8)(v >> 8), p[1] = (u8)v; }
static void put32(u8 *p, u32 v) { put16(p, v >> 16), put16(p + 2, v & 0xFFFF); }

/* Ethernet, IP and UDP headers for size bytes of data to the host */
static void headers(u8 *f, u32 size) {
  memcpy(f, peer_mac, 6);
  memcpy(f + 6, my_mac, 6);
  put16(f + 12, 0x0800);
  u8 *ip = f + 14;
  memset(ip, 0, 20);
  ip[0] = 0x45;
  put16(ip + 2, 28 + size);
  put16(ip + 6, 0x4000);
  ip[8] = 64, ip[9] = 17;
  memcpy(ip + 12, my_ip, 4);
  memcpy(ip + 16, peer_ip, 4);
  u32 sum = 0;
  for (u32 i = 0; i < 20; i += 2)
    sum += be16(ip + i);
  sum = (sum & 0xFFFF) + (sum >> 16);
  sum = (sum & 0xFFFF) + (sum >> 16);
  put16(ip + 10, ~sum & 0xFFFF);
  u8 *udp = ip + 20;
  put16(udp, PORT);
  put16(udp + 2, peer_port);
  put16(udp + 4, 8 + size);
  put16(udp + 6, 0); /* no checksum */
}

#ifdef RAWNET_MXETH
static u32 c0[MXETH_STATS + 6];
#endif

static void start_count(void) {
#ifdef RAWNET_MXETH
  mxeth_stats(c0);
  mxdev9_stats(c0 + MXETH_STATS);
#endif
  frames = bytes = waits = drops = 0;
  n0 = idle_count;
  t0 = now();
}

/* The counters since start_count, to the host */
static void report(u8 what) {
  u32 ticks = now() - t0, n = idle_count - n0;
  u8 *d = reply + HDR;
  headers(reply, 32);
  d[0] = what, d[1] = d[2] = d[3] = 0;
  put32(d + 4, frames);
  put32(d + 8, bytes);
  put32(d + 12, ticks);
  put32(d + 16, n);
  put32(d + 20, idle_rate);
  put32(d + 24, waits);
  put32(d + 28, drops);
  reply_len = HDR + 32;
#ifdef RAWNET_MXETH
  /* then mxeth's and mxdev9's counters (mxeth.h, mxdev9.h), since the start */
  u32 c[MXETH_STATS + 6];
  mxeth_stats(c);
  mxdev9_stats(c + MXETH_STATS);
  for (u32 i = 0; i < MXETH_STATS + 6; i++)
    put32(d + 32 + 4 * i, c[i] - c0[i]);
  headers(reply, 32 + 4 * (MXETH_STATS + 6));
  reply_len = HDR + 32 + 4 * (MXETH_STATS + 6);
#endif
  XMIT();
}

static void arp(const u8 *f) {
  if (be16(f + 20) != 1 || memcmp(f + 38, my_ip, 4))
    return;
  memcpy(reply, f + 6, 6);
  memcpy(reply + 6, my_mac, 6);
  memcpy(reply + 12, f + 12, 8); /* type, then hardware and protocol types and sizes */
  put16(reply + 20, 2);
  memcpy(reply + 22, my_mac, 6);
  memcpy(reply + 28, my_ip, 4);
  memcpy(reply + 32, f + 22, 10);
  reply_len = 42;
  XMIT();
}

/* The frame in the current slot to the same slot of the EE's ring */
static void to_ee(const u8 *f, u32 len) {
  SifDmaTransfer_t d;
  int st, id;
  d.src = (void *)f;
  d.dest = (void *)(ee_ring + slot * SLOT);
  d.size = (len + 15) & ~15u;
  d.attr = 0;
  for (u32 i = 0;; i++) {
    CpuSuspendIntr(&st);
    id = (int)sceSifSetDma(&d, 1);
    CpuResumeIntr(st);
    if (id)
      break;
    waits++;
    if (i == 20000) {
      drops++;
      return;
    }
  }
  dma_id[slot] = id;
}

static void udp(const u8 *f, u32 len) {
  u8 cmd = f[HDR];
  if (cmd == 'D') {
    frames++;
    bytes += len;
    if (sif_mode)
      to_ee(f, len);
    return;
  }
  memcpy(peer_mac, f + 6, 6);
  memcpy(peer_ip, f + 26, 4);
  peer_port = be16(f + 34);
  switch (cmd) {
  case 'H':
    start_count();
    report('H');
    break;
  case 'S':
    start_count();
    report('S');
    break;
  case 'E':
    report('E');
    break;
  case 'T':
    start_count();
    headers(blast, FRAME - HDR);
    blast[HDR] = 'D';
    tx_sent = 0;
    tx_left = (u32)be16(f + HDR + 1) << 16 | be16(f + HDR + 3);
    XMIT();
    break;
  case 'M':
    sif_mode = f[HDR + 1] && ee_ring;
    start_count();
    report('M');
    break;
  case 'Q':
    quit = 1;
    break;
  }
}

#ifndef RAWNET_MXETH
static void link_up(void) {}
static void link_down(void) {}

static void *alloc_rx(unsigned int size, void **payload) {
  slot = (slot + 1) % SLOTS;
  for (u32 i = 0; dma_id[slot] && sceSifDmaStat(dma_id[slot]) >= 0; i++) {
    waits++;
    if (i == 20000) {
      drops++;
      return 0;
    }
  }
  dma_id[slot] = 0;
  rx_len = size;
  *payload = rxbuf[slot];
  return rxbuf[slot];
}

static void free_rx(void *packet) { (void)packet; }
#endif

static void enq_rx(void *packet) {
  const u8 *f = packet;
  u32 type = be16(f + 12);
  if (type == 0x0806)
    arp(f);
  else if (type == 0x0800 && f[23] == 17 && !memcmp(f + 30, my_ip, 4) && be16(f + 36) == PORT)
    udp(f, rx_len);
}

#ifndef RAWNET_MXETH
static int next_tx(void **payload) {
  if (reply_len) {
    *payload = reply;
    return (int)reply_len;
  }
  if (tx_left) {
    *payload = blast;
    return FRAME;
  }
  return 0;
}

static void deq_tx(void) {
  if (reply_len)
    reply_len = 0;
  else if (tx_left) {
    frames++;
    bytes += FRAME;
    if (!--tx_left)
      report('T');
  }
}
#endif

#ifdef RAWNET_MXETH
/* mxeth's frames: in SIF mode one is held per slot of the EE's ring until its
   DMA is done */
static struct mxeth_frame *held[SLOTS];

static int mx_rx(struct mxeth_frame *f, void *arg) {
  (void)arg;
  slot = (slot + 1) % SLOTS;
  for (u32 i = 0; dma_id[slot] && sceSifDmaStat(dma_id[slot]) >= 0; i++) {
    waits++;
    if (i == 20000) {
      drops++;
      mxeth_free(f);
      return 1;
    }
  }
  dma_id[slot] = 0;
  if (held[slot]) {
    mxeth_free(held[slot]);
    held[slot] = 0;
  }
  rx_len = f->len;
  enq_rx(f->data);
  if (dma_id[slot])
    held[slot] = f;
  else
    mxeth_free(f);
  return 1;
}

static struct mxeth_client client = {0, 10, mx_rx, 0, 0};
#define BLASTS 4
static struct mxeth_tx reply_tx, blast_tx[BLASTS];
static u32 blast_busy, blast_queued;

static void reply_done(struct mxeth_tx *t) { (void)t; }

static void blast_done(struct mxeth_tx *t) {
  blast_busy &= ~(1u << (t - blast_tx));
  blast_queued--;
  if (tx_left) {
    frames++;
    bytes += FRAME;
    if (!--tx_left)
      report('T');
  }
  kick_tx();
}

/* what next_tx and deq_tx give netman, pushed to mxeth */
static void kick_tx(void) {
  if (reply_len) {
    reply_tx.data = reply;
    reply_tx.len = (u16)reply_len;
    reply_tx.done = reply_done;
    reply_len = 0;
    mxeth_send(&reply_tx);
  }
  for (u32 i = 0; i < BLASTS && blast_queued < tx_left; i++) {
    struct mxeth_tx *t = &blast_tx[i];
    if (blast_busy & 1u << i)
      continue;
    blast_busy |= 1u << i;
    blast_queued++;
    t->data = blast;
    t->len = FRAME;
    t->done = blast_done;
    t->arg = 0;
    mxeth_send(t);
  }
}
#endif

static u32 hex(const char *s) {
  u32 v = 0;
  for (; *s; s++)
    v = v << 4 | (u32)(*s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10);
  return v;
}

int _start(int argc, char **argv) {
  if (argc > 1)
    ee_ring = hex(argv[1]);
#ifndef RAWNET_MXETH
  static const struct NetManNetProtStack stack = {
      link_up, link_down, alloc_rx, free_rx, enq_rx, next_tx, deq_tx, 0, 0};
#endif
  iop_thread_t th = {TH_C, 0, idle, 0x800, 126};

  timer = AllocHardTimer(1, 32, 1);
  if (timer < 0)
    return (1 << 2) | MODULE_NO_RESIDENT_END;
  SetTimerMode(timer, 0);
  int id = CreateThread(&th);
  if (id <= 0)
    return (2 << 2) | MODULE_NO_RESIDENT_END;
  StartThread(id, 0);

  u32 a = now(), b = idle_count;
  DelayThread(1000000);
  idle_rate = (idle_count - b) / ((now() - a) / (TICKS_S / 1000u));

#ifdef RAWNET_MXETH
  mxeth_mac(my_mac);
  mxeth_attach(&client);
#else
  NetManIoctl(NETMAN_NETIF_IOCTL_ETH_GET_MAC, 0, 0, my_mac, sizeof my_mac);
  NetManRegisterNetworkStack(&stack);
#endif

  for (u32 ms = 0; ms < DEADLINE_MS && !quit; ms += 50) {
    DelayThread(50000);
#ifndef RAWNET_MXETH /* mxeth: blast_done sends on, all in its driver thread */
    if (tx_left)
      XMIT();
#endif
  }
  idle_stop = 1;
  DelayThread(100000);
  return MODULE_RESIDENT_END;
}
