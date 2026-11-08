/* mxfast.irx: the fast path (mxfast.h) on maxsdk's mxeth, without lwIP
   (docs/iop-drivers.md).

   A client of mxeth before lwIP: it takes every frame to UDP port 18197. DATA
   goes by SIF DMA straight from mxeth's receive buffer to the EE, answers wake
   the caller. Requests leave as frames it builds itself, to the host that said
   HELLO (its MAC and IP come from that frame), through mxeth_send.

   One caller at a time (mxio's thread, below the driver's): the driver thread
   runs rx to its end, the caller only reads what rx writes. The caller sleeps
   on one semaphore, given by rx (an answer, a whole read), by a sent frame's
   done, or by an alarm (a timeout); every wake checks why. */
#include <irx.h>
#include <intrman.h>
#include <ioman.h>
#include <loadcore.h>
#include <mxeth.h>
#include <sifman.h>
#include <stdio.h>
#include <sysclib.h>
#include <sysmem.h>
#include <thbase.h>
#include <thsemap.h>
#include <mxfast.h>
#include "mxfast_int.h"

#define MODNAME "mxfast"
IRX_ID(MODNAME, 1, 0);

#define PENDING 16
#define MAX_CHUNKS ((MXFAST_MAX + MXFAST_CHUNK - 1) / MXFAST_CHUNK)
#define STALL_US 30000 /* a read without progress for this long asks again */
#define TRIES 40
#define ASK_US 200000 /* a request without answer for this long is sent again */
#define ASK_TRIES 5
#define PRIO 10 /* before the console and lwIP (30) */

static int wake = -1;
static volatile int rang;

static unsigned int alarm_rang(void *arg) {
  (void)arg;
  rang = 1;
  iSignalSema(wake);
  return 0;
}

/* Sleep until something happened or us passed: 1 if the time passed */
static int sleep_for(u32 us) {
  iop_sys_clock_t c;
  USec2SysClock(us, &c);
  rang = 0;
  SetAlarm(&c, alarm_rang, 0);
  WaitSema(wake);
  CancelAlarm(alarm_rang, 0);
  return rang;
}

static void wake_caller(void) { SignalSema(wake); }

/* The read. Its fields are shared with rx, in the driver thread. */
static struct {
  volatile int active;
  u32 id, ee, size, total;
  volatile u32 got;
  u32 have[(MAX_CHUNKS + 31) / 32];
} x;

/* Frames whose SIF DMA may still run */
static struct {
  struct mxeth_frame *f;
  int dma;
} pending[PENDING];
static u32 head, frames, doubles, drops, again;

/* The request waiting for its answer */
static volatile u32 ask_id;
static volatile int answered;
static int answer_a;
static u32 answer_b;

u8 fast_eth_ip[42];
volatile int fast_have_peer;
#define eth_ip fast_eth_ip
#define have_peer fast_have_peer
static u32 next_id;

static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static void put32(u8 *p, u32 v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }
static void put16be(u8 *p, u32 v) { p[0] = v >> 8, p[1] = v; }

static void free_pending(u32 i) {
  mxeth_free(pending[i].f);
  pending[i].f = 0;
}

/* A DATA frame, in the driver thread: 1 if its buffer went to a DMA */
static int data(struct mxeth_frame *fr, const u8 *f) {
  u32 id = le32(f + 46), off = le32(f + 50), len = f[54] | f[55] << 8, total = le32(f + 56);
  u32 chunk = off / MXFAST_CHUNK;
  if (!x.active || id != x.id || off >= x.size + 1 || off + len > x.size || off % MXFAST_CHUNK ||
      fr->len < MXFAST_DATA_AT + len)
    return 0;
  frames++;
  if (total < x.total)
    x.total = total;
  if (!len || x.have[chunk / 32] >> (chunk % 32) & 1) {
    doubles += len != 0;
    goto check;
  }

  u32 i = head % PENDING;
  if (pending[i].f) {
    if (sceSifDmaStat(pending[i].dma) >= 0) {
      drops++; /* asked for again later */
      return 0;
    }
    free_pending(i);
  }
  SifDmaTransfer_t d;
  int st, dma;
  d.src = (void *)(f + MXFAST_DATA_AT);
  d.dest = (void *)(x.ee + off);
  d.size = (len + 15) & ~15u;
  d.attr = 0;
  CpuSuspendIntr(&st);
  dma = (int)sceSifSetDma(&d, 1);
  CpuResumeIntr(st);
  if (!dma) {
    drops++;
    return 0;
  }
  pending[i].f = fr, pending[i].dma = dma;
  head++;
  x.have[chunk / 32] |= 1u << (chunk % 32);
  x.got += len;
  if (x.got >= x.total)
    wake_caller();
  return 1;
check:
  if (x.got >= x.total)
    wake_caller();
  return 0;
}

static int rx(struct mxeth_frame *fr, void *arg) {
  const u8 *f = fr->data;
  (void)arg;
  if (fr->len < MXFAST_DATA_AT || f[12] != 8 || f[13] != 0 || f[14] != 0x45 || f[23] != 17 ||
      f[36] != (MXFAST_PORT >> 8) || f[37] != (MXFAST_PORT & 255) || f[42] != 'M' || f[43] != 'F')
    return 0;
  u32 op = f[44];
  if (op == MXFAST_DATA) {
    if (data(fr, f))
      return 1;
  } else if (op & MXFAST_ANSWER || op == MXFAST_HELLO) {
    /* the way back */
    memcpy(eth_ip, f + 6, 6); /* its MAC */
    mxeth_mac(eth_ip + 6);    /* ours */
    eth_ip[12] = 8, eth_ip[13] = 0;
    memcpy(eth_ip + 26, f + 30, 4); /* our IP */
    memcpy(eth_ip + 30, f + 26, 4); /* its IP */
    have_peer = 1;
    if (op == MXFAST_HELLO)
      console_hello(le32(f + 50));
    else if (op == (MXFAST_TTY | MXFAST_ANSWER))
      console_ack(le32(f + 56));
    else if (op & MXFAST_ANSWER && ask_id && le32(f + 46) == ask_id) {
      answer_a = (int)le32(f + 50);
      answer_b = le32(f + 56);
      ask_id = 0;
      answered = 1;
      wake_caller();
    }
  }
  /* the port is the fast path's alone */
  mxeth_free(fr);
  return 1;
}

static struct mxeth_client client = {0, PRIO, rx, 0, 0};

/* After a read: rx adds no more, and the last DMAs end (a frame's takes a few us) */
static void finish_dma(void) {
  for (u32 i = 0; i < PENDING; i++)
    if (pending[i].f) {
      for (u32 n = 0; sceSifDmaStat(pending[i].dma) >= 0 && n < 1000; n++)
        DelayThread(100);
      free_pending(i);
    }
}

/* The Ethernet, IP and UDP headers of a frame to the host, udp bytes after them */
void fast_headers(u8 *f, u32 udp) {
  u32 sum = 0;
  memcpy(f, eth_ip, 14);
  u8 *ip = f + 14;
  memset(ip, 0, 20);
  ip[0] = 0x45;
  put16be(ip + 2, 20 + 8 + udp);
  put16be(ip + 6, 0x4000);
  ip[8] = 64, ip[9] = 17;
  memcpy(ip + 12, eth_ip + 26, 8);
  for (u32 i = 0; i < 20; i += 2)
    sum += (u32)ip[i] << 8 | ip[i + 1];
  sum = (sum & 0xFFFF) + (sum >> 16);
  sum = (sum & 0xFFFF) + (sum >> 16);
  put16be(ip + 10, ~sum & 0xFFFF);
  u8 *u = ip + 20;
  put16be(u, MXFAST_PORT);
  put16be(u + 2, MXFAST_PORT);
  put16be(u + 4, 8 + udp);
  put16be(u + 6, 0); /* no checksum: Ethernet's CRC, on the LAN */
}

void fast_header(u8 *b, u32 op, u32 id, u32 a, u32 v, u32 c) {
  memset(b, 0, MXFAST_HDR);
  b[0] = 'M', b[1] = 'F', b[2] = op;
  put32(b + 4, id);
  put32(b + 8, a);
  put32(b + 14, v);
  put32(b + 18, c);
}

/* ---- requests: one frame, sent again until its answer comes */

#define headers fast_headers
#define header fast_header

static u8 req[42 + MXFAST_HDR + 4 + 256] __attribute__((aligned(16)));
static u8 *const msg = req + 42;
static struct mxeth_tx req_tx;
static volatile int req_queued;

static void req_done(struct mxeth_tx *t) {
  (void)t;
  req_queued = 0;
  wake_caller();
}

/* req's buffer is free to write */
static void req_wait(void) {
  for (u32 n = 0; req_queued && n < 10; n++)
    sleep_for(ASK_US);
}

/* msg (n bytes) to the host */
static int send_msg(u32 n) {
  if (req_queued)
    return -5; /* not out yet: a later try sends it */
  headers(req, n);
  req_tx.data = req;
  req_tx.len = 42 + n;
  req_tx.done = req_done;
  req_queued = 1;
  return mxeth_send(&req_tx) < 0 ? -5 : (int)n;
}

/* A request with an answer: result, or < 0 */
static int ask(u32 n, u32 id) {
  answered = 0;
  ask_id = id;
  for (u32 t = 0; t < ASK_TRIES && !answered; t++) {
    send_msg(n);
    while (!answered && !sleep_for(ASK_US))
      ;
  }
  ask_id = 0;
  return answered ? answer_a : -110;
}

static u32 text(const char *s) {
  u32 n = strlen(s) + 1;
  if (n > 256)
    n = 256;
  memcpy(msg + MXFAST_HDR, s, n);
  msg[MXFAST_HDR + n - 1] = 0;
  return MXFAST_HDR + n;
}

static void read_range(u32 handle, u32 pos, u32 size, u32 at) {
  req_wait();
  header(msg, MXFAST_READ, x.id, handle, pos, size);
  put32(msg + MXFAST_HDR, at);
  send_msg(MXFAST_HDR + 4);
}

int mxfast_open(const char *path, int mode) {
  if (!have_peer)
    return -6;
  req_wait();
  u32 id = ++next_id;
  header(msg, MXFAST_OPEN, id, (u32)mode, 0, 0);
  return ask(text(path), id);
}

int mxfast_close(int handle) {
  req_wait();
  u32 id = ++next_id;
  header(msg, MXFAST_CLOSE, id, (u32)handle, 0, 0);
  return ask(MXFAST_HDR, id);
}

int mxfast_size(int handle) {
  req_wait();
  u32 id = ++next_id;
  header(msg, MXFAST_SIZE, id, (u32)handle, 0, 0);
  return ask(MXFAST_HDR, id);
}

int mxfast_log(const char *s) {
  if (!have_peer)
    return -6;
  req_wait();
  header(msg, MXFAST_LOG, ++next_id, 0, 0, 0);
  return send_msg(text(s));
}

void mxfast_stats(int *out) {
  out[0] = (int)frames, out[1] = (int)doubles, out[2] = (int)drops, out[3] = (int)again;
}

/* ---- reads */

int mxfast_read(int handle, u32 pos, u32 size, u32 ee) {
  if (size > MXFAST_MAX || ee & 15 || !have_peer)
    return -22;
  memset(x.have, 0, sizeof x.have);
  x.id = ++next_id, x.ee = ee, x.size = x.total = size, x.got = 0;
  x.active = 1;
  read_range(handle, pos, size, 0);

  int result = -5;
  u32 last = 0, tries = 0;
  for (;;) {
    if (x.got >= x.total) {
      result = (int)x.total;
      break;
    }
    int late = sleep_for(STALL_US);
    if (x.got != last) {
      last = x.got;
      continue;
    }
    if (!late)
      continue;
    if (++tries > TRIES)
      break;
    /* each run of missing chunks up to total */
    u32 chunks = (x.total + MXFAST_CHUNK - 1) / MXFAST_CHUNK;
    for (u32 c = 0; c < chunks;) {
      if (x.have[c / 32] >> (c % 32) & 1) {
        c++;
        continue;
      }
      u32 e = c;
      while (e < chunks && !(x.have[e / 32] >> (e % 32) & 1))
        e++;
      u32 at = c * MXFAST_CHUNK, end = e * MXFAST_CHUNK;
      if (end > x.total)
        end = x.total;
      again++;
      read_range(handle, pos + at, end - at, at);
      c = e;
    }
  }
  x.active = 0;
  finish_dma();
  return result;
}

/* ---- writes: the frames go out as they are, then a COMMIT asks which came */

static struct mxeth_tx frame_tx[MXFAST_WRITE_FRAMES];
static volatile u32 frames_done;
static u32 tx_n, tx_list[MXFAST_WRITE_FRAMES], tx_size[MXFAST_WRITE_FRAMES];
static u8 *tx_base;

static void frame_done(struct mxeth_tx *t) {
  (void)t;
  if (++frames_done == tx_n)
    wake_caller();
}

static int send_frames(u32 n) {
  frames_done = 0;
  tx_n = n;
  for (u32 i = 0; i < n; i++) {
    frame_tx[i].data = tx_base + tx_list[i] * MXFAST_FRAME;
    frame_tx[i].len = (u16)tx_size[i];
    frame_tx[i].done = frame_done;
    mxeth_send(&frame_tx[i]);
  }
  /* each done comes within a few ms with a link, at once without one */
  for (u32 t = 0; frames_done < tx_n && t < 10; t++)
    sleep_for(ASK_US);
  int ok = frames_done == tx_n;
  tx_n = 0;
  return ok ? 0 : -5;
}

/* The headers of a frame: len bytes of data */
static void frame_headers(u8 *f, u32 id, u32 handle, u32 pos, u32 at, u32 len) {
  headers(f, MXFAST_HDR + ((len + 15) & ~15u));
  header(f + 42, MXFAST_WRITE, id, handle, pos, at);
  f[54] = len, f[55] = len >> 8;
}

int mxfast_write(int handle, u32 pos, u32 size, void *frames) {
  u32 n = (size + MXFAST_CHUNK - 1) / MXFAST_CHUNK;
  if (!size || n > MXFAST_WRITE_FRAMES || !have_peer)
    return -22;
  tx_base = frames;
  u32 id = ++next_id, all = n == 32 ? ~0u : (1u << n) - 1;
  for (u32 i = 0; i < n; i++) {
    u32 at = i * MXFAST_CHUNK, len = size - at > MXFAST_CHUNK ? MXFAST_CHUNK : size - at;
    frame_headers(tx_base + i * MXFAST_FRAME, id, (u32)handle, pos + at, at, len);
    tx_list[i] = i;
    tx_size[i] = MXFAST_DATA_AT + ((len + 15) & ~15u);
  }
  u32 count = n;
  for (u32 t = 0; t < TRIES; t++) {
    if (send_frames(count) < 0)
      return -5;
    req_wait();
    header(msg, MXFAST_COMMIT, id, (u32)handle, 0, 0);
    if (ask(MXFAST_HDR, id) < 0)
      return -110;
    u32 missing = all & ~answer_b;
    if (!missing)
      return (int)size;
    again++;
    count = 0;
    for (u32 i = 0; i < n; i++)
      if (missing >> i & 1) {
        u32 at = i * MXFAST_CHUNK, len = size - at > MXFAST_CHUNK ? MXFAST_CHUNK : size - at;
        tx_list[count] = i;
        tx_size[count++] = MXFAST_DATA_AT + ((len + 15) & ~15u);
      }
  }
  return -5;
}

extern struct irx_export_table _exp_mxfast;

int _start(int argc, char *argv[]) {
  (void)argc, (void)argv;
  iop_sema_t s = {0, 0, 0, 1};
  if ((wake = CreateSema(&s)) < 0 || RegisterLibraryEntries(&_exp_mxfast) != 0)
    return MODULE_NO_RESIDENT_END;
  mxeth_attach(&client);
  console_start();
  return MODULE_RESIDENT_END;
}
