/* The fast path (mxfast.h) inside ps2ip-nm, on ps2sdk's netman. On maxsdk's
   mxeth it is a module of its own: iop/tcpip/mxfast. */
#include "lwip/pbuf.h"
#include "lwip/sockets.h"
#include "irx_imports.h"
#include <mxfast.h>

#define PENDING 16
#define MAX_CHUNKS ((MXFAST_MAX + MXFAST_CHUNK - 1) / MXFAST_CHUNK)
#define STALL_MS 30
#define TRIES 40

/* The transfer. Its fields are shared between the RPC thread and the driver's,
   whose priority is higher: mxfast_rx runs to its end, and the RPC thread
   only reads while active is set. */
static struct {
  volatile int active;
  u32 id, ee, size, total, got;
  u32 have[(MAX_CHUNKS + 31) / 32];
} x;

static struct {
  struct pbuf *p;
  int dma;
} pending[PENDING];
static u32 head, frames, doubles, drops, again;

/* The frames being sent: slots of tx_base. Only the last one the driver
   took is dequeued, and the driver's thread is above the RPC thread. */
static volatile u32 tx_n;
static u8 *tx_base;
static u32 tx_i, tx_list[MXFAST_WRITE_FRAMES], tx_size[MXFAST_WRITE_FRAMES];
static int tx_taken; /* the driver's last frame was ours */

/* From the host's answers: what a frame to it starts with */
static u8 eth_ip[42];
static int have_peer;

static int sock = -1;
static struct sockaddr_in host;
static u32 next_id;

static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static void put32(u8 *p, u32 v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }

int mxfast_rx(struct pbuf *p) {
  const u8 *f = p->payload;
  if (p->len < MXFAST_DATA_AT || f[12] != 8 || f[13] != 0 || f[23] != 17 ||
      f[36] != (MXFAST_PORT >> 8) || f[37] != (MXFAST_PORT & 255) || f[42] != 'M' ||
      f[43] != 'F')
    return 0;
  if (f[44] & MXFAST_ANSWER || f[44] == MXFAST_HELLO) {
    /* the way back; lwIP's socket takes the answers */
    memcpy(eth_ip, f + 6, 6);       /* its MAC */
    memcpy(eth_ip + 6, f, 6);       /* ours */
    memcpy(eth_ip + 12, f + 12, 2);
    memcpy(eth_ip + 26, f + 30, 4); /* our IP */
    memcpy(eth_ip + 30, f + 26, 4); /* its IP */
    have_peer = 1;
    if (f[44] == MXFAST_HELLO) {
      pbuf_free(p);
      return 1;
    }
    return 0;
  }
  if (f[44] != MXFAST_DATA)
    return 0;

  u32 id = le32(f + 46), off = le32(f + 50), len = f[54] | f[55] << 8, total = le32(f + 56);
  u32 chunk = off / MXFAST_CHUNK;
  if (!x.active || id != x.id || off >= x.size + 1 || off + len > x.size ||
      off % MXFAST_CHUNK || p->len < MXFAST_DATA_AT + len)
    goto done;
  frames++;
  if (total < x.total)
    x.total = total;
  if (!len)
    goto done;
  if (x.have[chunk / 32] >> (chunk % 32) & 1) {
    doubles++;
    goto done;
  }

  u32 i = head % PENDING;
  if (pending[i].p) {
    if (sceSifDmaStat(pending[i].dma) >= 0) {
      drops++; /* asked for again later */
      goto done;
    }
    pbuf_free(pending[i].p);
    pending[i].p = 0;
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
    goto done;
  }
  pending[i].p = p, pending[i].dma = dma;
  head++;
  x.have[chunk / 32] |= 1u << (chunk % 32);
  x.got += len;
  return 1;
done:
  pbuf_free(p);
  return 1;
}

int mxfast_tx_next(void **payload) {
  tx_taken = tx_i < tx_n;
  if (!tx_taken)
    return 0;
  *payload = tx_base + tx_list[tx_i] * MXFAST_FRAME;
  return (int)tx_size[tx_i];
}

int mxfast_tx_deq(void) {
  if (!tx_taken)
    return 0;
  tx_taken = 0;
  tx_i++;
  return 1;
}

/* After the transfer: the driver no longer adds to pending */
static void finish_dma(void) {
  for (u32 i = 0; i < PENDING; i++)
    if (pending[i].p) {
      for (u32 n = 0; sceSifDmaStat(pending[i].dma) >= 0 && n < 1000; n++)
        DelayThread(100);
      pbuf_free(pending[i].p);
      pending[i].p = 0;
    }
}

static void header(u8 *b, u32 op, u32 id, u32 a, u32 v, u32 c) {
  memset(b, 0, MXFAST_HDR);
  b[0] = 'M', b[1] = 'F', b[2] = op;
  put32(b + 4, id);
  put32(b + 8, a);
  put32(b + 14, v);
  put32(b + 18, c);
}

static int send_host(const void *b, u32 n) {
  return lwip_sendto(sock, (void *)b, n, 0, (struct sockaddr *)&host, sizeof host);
}

static int read_range(u32 handle, u32 pos, u32 size, u32 at) {
  u8 b[MXFAST_HDR + 4];
  header(b, MXFAST_READ, x.id, handle, pos, size);
  put32(b + MXFAST_HDR, at);
  return send_host(b, sizeof b);
}

static void put16be(u8 *p, u32 v) { p[0] = v >> 8, p[1] = v; }

/* The headers of frame i: len bytes of data */
static void frame_headers(u8 *f, u32 id, u32 handle, u32 pos, u32 at, u32 len) {
  u32 padded = (len + 15) & ~15u, udp = 8 + MXFAST_HDR + padded, sum = 0;
  memcpy(f, eth_ip, 14);
  u8 *ip = f + 14;
  memset(ip, 0, 20);
  ip[0] = 0x45;
  put16be(ip + 2, 20 + udp);
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
  put16be(u + 4, udp);
  put16be(u + 6, 0); /* no checksum: Ethernet's CRC, on the LAN */
  header(f + 42, MXFAST_WRITE, id, handle, pos, at);
  f[54] = len, f[55] = len >> 8;
}

static u32 answer_b;

/* A request with an answer: result, or < 0 */
static int ask(u8 *b, u32 n, u32 id);

/* The frames in tx_list to the driver, and wait for them: 0, < 0 if it stalls */
static int send_frames(u32 n) {
  tx_i = 0;
  tx_n = n;
  NetManNetIFXmit();
  /* the driver refills its FIFO at each frame sent: disturb it only if it stops */
  u32 last = 0, idle = 0;
  for (u32 ms = 0; tx_i < tx_n; ms++) {
    if (ms > 1000) {
      tx_n = tx_i = 0;
      return -5;
    }
    DelayThread(1000);
    if (tx_i != last)
      last = tx_i, idle = 0;
    else if (++idle == 20) {
      idle = 0;
      NetManNetIFXmit();
    }
  }
  tx_n = tx_i = 0;
  return 0;
}


int mxfast_write(int handle, u32 pos, u32 size, void *frames) {
  u32 n = (size + MXFAST_CHUNK - 1) / MXFAST_CHUNK;
  if (!size || n > MXFAST_WRITE_FRAMES || !have_peer || sock < 0)
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
    u8 b[MXFAST_HDR];
    header(b, MXFAST_COMMIT, id, (u32)handle, 0, 0);
    if (ask(b, sizeof b, id) < 0)
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

int mxfast_read(int handle, u32 pos, u32 size, u32 ee) {
  if (size > MXFAST_MAX || ee & 15 || sock < 0)
    return -22;
  memset(x.have, 0, sizeof x.have);
  x.id = ++next_id, x.ee = ee, x.size = x.total = size, x.got = 0;
  x.active = 1;
  read_range(handle, pos, size, 0);

  int result = -5;
  u32 last = 0, idle = 0, tries = 0;
  for (;;) {
    if (x.got >= x.total) {
      result = (int)x.total;
      break;
    }
    DelayThread(1000);
    if (x.got != last) {
      last = x.got, idle = 0;
      continue;
    }
    if (++idle < STALL_MS)
      continue;
    idle = 0;
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

static int ask(u8 *b, u32 n, u32 id) {
  for (u32 t = 0; t < 5; t++) {
    send_host(b, n);
    fd_set set;
    struct timeval tv = {0, 200000};
    FD_ZERO(&set);
    FD_SET(sock, &set);
    if (lwip_select(sock + 1, &set, 0, 0, &tv) <= 0)
      continue;
    u8 r[MXFAST_HDR];
    int got = lwip_recv(sock, r, sizeof r, MSG_DONTWAIT);
    if (got >= (int)MXFAST_HDR && r[2] & MXFAST_ANSWER && le32(r + 4) == id) {
      answer_b = le32(r + 14);
      return (int)le32(r + 8);
    }
  }
  return -110;
}

/* The socket, and where the host is: the one that said HELLO */
static int ready(void) {
  if (!have_peer)
    return 0;
  if (sock < 0) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(MXFAST_PORT);
    sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock >= 0 && lwip_bind(sock, (struct sockaddr *)&a, sizeof a) < 0) {
      lwip_close(sock);
      sock = -1;
    }
  }
  memset(&host, 0, sizeof host);
  host.sin_family = AF_INET;
  host.sin_port = htons(MXFAST_PORT);
  memcpy(&host.sin_addr.s_addr, eth_ip + 30, 4);
  return sock >= 0;
}

static u8 msg[MXFAST_HDR + 256];

static u32 text(const char *s) {
  u32 n = strlen(s) + 1;
  if (n > 256)
    n = 256;
  memcpy(msg + MXFAST_HDR, s, n);
  msg[MXFAST_HDR + n - 1] = 0;
  return MXFAST_HDR + n;
}

int mxfast_open(const char *path, int mode) {
  if (!ready())
    return -6;
  u32 id = ++next_id;
  header(msg, MXFAST_OPEN, id, (u32)mode, 0, 0);
  return ask(msg, text(path), id);
}

int mxfast_close(int handle) {
  u32 id = ++next_id;
  header(msg, MXFAST_CLOSE, id, (u32)handle, 0, 0);
  return ask(msg, MXFAST_HDR, id);
}

int mxfast_size(int handle) {
  u32 id = ++next_id;
  header(msg, MXFAST_SIZE, id, (u32)handle, 0, 0);
  return ask(msg, MXFAST_HDR, id);
}

int mxfast_log(const char *s) {
  if (!ready())
    return -6;
  header(msg, MXFAST_LOG, ++next_id, 0, 0, 0);
  return send_host(msg, text(s));
}

void mxfast_stats(int *out) {
  out[0] = (int)frames, out[1] = (int)doubles, out[2] = (int)drops, out[3] = (int)again;
}
