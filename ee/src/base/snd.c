/* Sound through ps2sdk's audsrv.irx (on libsd.irx), carried in libmx as
   data (#embed, from the Makefile's --embed-dir) and loaded from EE
   memory. audsrv's RPC server is 0x870884e: each function takes its
   arguments as words and answers with a word (ps2sdk ee/rpc/audsrv has its
   client, iop/sound/audsrv its source). Its INIT binds back to an EE server
   of the same id, for its callbacks: mx_sif_serve answers it. */

#include <mx/cpu.h>
#include <mx/iop.h>
#include <mx/sif.h>
#include <mx/snd.h>
#include <mx/string.h>
#include <mx/timer.h>

#define AUDSRV_SID 0x870884eu
#define AUDSRV_INIT 0x0000
#define AUDSRV_SET_FORMAT 0x0003
#define AUDSRV_PLAY_AUDIO 0x0004
#define AUDSRV_STOP_AUDIO 0x0006
#define AUDSRV_SET_VOLUME 0x0007
#define AUDSRV_INIT_ADPCM 0x0016
#define AUDSRV_LOAD_ADPCM 0x0017
#define AUDSRV_PLAY_ADPCM 0x0018
#define AUDSRV_SET_ADPCM_VOL 0x0019
#define AUDSRV_AVAILABLE 0x001a
#define AUDSRV_QUEUED 0x001b
#define AUDSRV_FREE_ADPCM 0x001c
#define AUDSRV_IS_ADPCM_PLAYING 0x001d

/* audsrv's receive buffer is 18000 bytes: a queue call sends the byte count
   and up to this much data */
#define QUEUE_MAX 16368
#define CHUNK 16384

/* An IOP module as data: 16-byte aligned for the transfer to the IOP */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
static const u8 libsd_irx[] __attribute__((aligned(16))) = {
#embed <libsd.irx>
};
static const u8 audsrv_irx[] __attribute__((aligned(16))) = {
#embed <audsrv.irx>
};
#pragma clang diagnostic pop

static struct mx_sif_client srv;
/* the stream's calls (PLAY_AUDIO, AVAILABLE) run without waiting
   (mx_sif_call_async), one at a time */
static u32 sbuf[(QUEUE_MAX + 16) / 4] MX_DMA_LINES;
static u32 sres[4] MX_DMA_LINES;
static u32 sarg[4] MX_DMA_LINES;
static u32 arg[4] MX_DMA_LINES;
static u32 res[4] MX_DMA_LINES;
static u8 bounce[CHUNK] MX_DMA_LINES;
static volatile u32 callbacks;
static int ready;

static void callback(int fno, void *arg) {
  (void)fno, (void)arg;
  callbacks++;
}

static void drain(void);

static int call(int fno, u32 a, u32 b, u32 c) {
  drain(); /* the stream's calls first: they use the same server */
  volatile u32 *w = MX_UNCACHED(arg);
  volatile u32 *r = MX_UNCACHED(res);
  w[0] = a;
  w[1] = b;
  w[2] = c;
  w[3] = 0;
  if (mx_sif_call(&srv, fno, (void *)w, 16, (void *)r, 4) < 0)
    return -1;
  return (int)r[0];
}

/* Load and start the module from buf to end; < 0 if it does not load or does
   not stay (NO_RESIDENT_END). */
static int load(const u8 *buf, const u8 *end) {
  int r = 0;
  int id = mx_iop_load_module_buffer(buf, (u32)(end - buf), "", 0, &r);
  return id < 0 || (r & 3) == 1 ? -1 : 0;
}

int mx_snd_init(void) {
  if (ready)
    return 0;
  if (!mx_sif_ready() && mx_sif_init() < 0)
    return -1;
  if (mx_sif_serve(AUDSRV_SID, callback, 0) < 0)
    return -1;
  if (mx_sif_bind_tries(&srv, AUDSRV_SID, 2) < 0 || !srv.server) {
    if (load(libsd_irx, libsd_irx + sizeof libsd_irx) < 0 ||
        load(audsrv_irx, audsrv_irx + sizeof audsrv_irx) < 0)
      return -1;
    if (mx_sif_bind_tries(&srv, AUDSRV_SID, 100) < 0 || !srv.server)
      return -1;
  }
  if (call(AUDSRV_INIT, 0, 0, 0) != 0 || call(AUDSRV_INIT_ADPCM, 0, 0, 0) != 0)
    return -1;
  ready = 1;
  return 0;
}

/* ---- the PCM stream ---------------------------------------------------------- */

static u32 byte_rate = 48000 * 4; /* of the stream's format: bytes played a second */

int mx_snd_stream_format(u32 rate, int bits, int channels) {
  if (call(AUDSRV_SET_FORMAT, rate, (u32)bits, (u32)channels) != 0)
    return -1;
  byte_rate = rate * (u32)(bits / 8) * (u32)channels;
  return 0;
}

/* The stream's calls are not waited for, so a frame's room and queue cost
   no round trip to the IOP. audsrv's server takes one call at a time (all
   its clients share its receive buffer), so one is in flight at most: the
   PLAY_AUDIO of the last queue, or an AVAILABLE. The room is estimated from
   the last AVAILABLE answer: plus what the stream played since, less one
   feed block, less what was queued since. audsrv moves its ring's read
   position a feed block (512 samples at 48 kHz) at a time, as it renders
   one ahead, so the room grows in steps; the real room is never less than
   that. Played at 63/64 of the byte rate (the EE's clock and the SPU2's
   differ), capped at the most AVAILABLE ever answered. Every RESYNC calls
   an AVAILABLE is asked again; a queue meanwhile takes nothing, the next one
   does. Bytes audsrv does not take are lost (the caller counted them as
   queued), so the estimate errs low; too low starves the ring, and audsrv
   plays on regardless (its read position then runs past the write
   position, and its AVAILABLE answers wrap). */
#define RESYNC 15
enum { NONE, PLAY, AVAIL };
static struct mx_sif_client sc; /* a copy of srv */
static int flight, have_room;
static u32 ans_room, ans_t, queued_since, ring, since_sync;
volatile u32 mx__snd_short; /* bytes audsrv did not take (the estimate was off) */

static void answer(s32 n) {
  ans_room = n < 0 ? 0 : (u32)n; /* nothing was queued meanwhile: one call in flight */
  ring = ans_room > ring ? ans_room : ring;
  ans_t = mx_count();
  queued_since = 0;
  have_room = 1;
  since_sync = 0;
}

/* the call in flight, if answered (or wait: it is waited for) */
static void collect(int wait) {
  if (flight == NONE || (!wait && !sc.done))
    return;
  mx_sif_wait(&sc);
  volatile u32 *r = MX_UNCACHED(sres);
  if (flight == PLAY) {
    if (r[0] < sbuf[0])
      mx__snd_short += sbuf[0] - r[0];
  } else
    answer((s32)r[0]);
  flight = NONE;
}

static void drain(void) {
  collect(1);
  have_room = 0; /* a call of another kind may change the room */
}

static u32 estimate(void) {
  u32 dt = mx_count() - ans_t; /* < 14.5 s: an AVAILABLE every RESYNC calls */
  u32 rate = byte_rate - byte_rate / 64, feed = 512u * byte_rate / 48000u;
  u64 played = (u64)dt * rate / MX_EE_HZ;
  u64 room = ans_room + (played > feed ? played - feed : 0);
  room = room > queued_since ? room - queued_since : 0;
  return room > ring ? ring : (u32)room;
}

u32 mx_snd_stream_room(void) {
  collect(0);
  if (!have_room)
    answer(call(AUDSRV_AVAILABLE, 0, 0, 0)); /* the first: waited for */
  if (!sc.server)
    sc.server = srv.server, sc.buf = srv.buf, sc.cbuf = srv.cbuf;
  if (flight == NONE && ++since_sync >= RESYNC) {
    flight = AVAIL;
    mx_sif_call_async(&sc, AUDSRV_AVAILABLE, MX_UNCACHED(sarg), 16, MX_UNCACHED(sres), 4);
  }
  if (flight == AVAIL)
    return 0; /* until it answers */
  return estimate();
}

u32 mx_snd_stream_queued(void) {
  int n = call(AUDSRV_QUEUED, 0, 0, 0);
  return n < 0 ? 0 : (u32)n;
}

/* Word 0 the byte count, the data after it; audsrv answers with the bytes
   it took. At most the room estimate is sent, in one call. */
u32 mx_snd_stream_queue(const void *data, u32 bytes) {
  u32 room = mx_snd_stream_room();
  if (flight == PLAY)
    collect(1); /* the last queue's call (answered long ago, normally) */
  if (flight != NONE || !room || !bytes)
    return 0;
  u32 n = bytes < QUEUE_MAX ? bytes : QUEUE_MAX;
  if (n > room)
    n = room;
  sbuf[0] = n;
  memcpy(&sbuf[1], data, n);
  flight = PLAY;
  mx_sif_call_async(&sc, AUDSRV_PLAY_AUDIO, sbuf, (n + 4 + 15) & ~15u, MX_UNCACHED(sres), 4);
  queued_since += n;
  return n;
}

void mx_snd_stream_stop(void) { call(AUDSRV_STOP_AUDIO, 0, 0, 0); }

/* the home menu (ee/src/gs/home.c): the stream silenced while the program
   waits (audsrv would play its ring over and over); the next queue starts it */
void mx__snd_quiet(void) {
  if (ready)
    mx_snd_stream_stop();
}

void mx_snd_stream_volume(int volume) { call(AUDSRV_SET_VOLUME, (u32)volume, 0, 0); }

/* ---- ADPCM ------------------------------------------------------------------ */

/* A block: shift (0 .. 12) and filter in byte 0, flags in byte 1, then 28
   4-bit samples, the first in the low nibble. The SPU decodes a sample as
   (nibble << 12 >> shift) + (s1 * f0 + s2 * f1 + 32 >> 6), clamped, with s1,
   s2 the two samples before. The encoder tries each filter with the shift
   its largest residual needs and one finer, against the decoded history, and
   keeps the one with the least squared error. */
static const int filt[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

static u64 try_block(const s16 *x, int f, int sh, int *h1, int *h2, u8 *b) {
  int s1 = *h1, s2 = *h2, step = 1 << (12 - sh);
  u64 err = 0;
  for (int i = 0; i < 14; i++)
    b[2 + i] = 0;
  for (int i = 0; i < 28; i++) {
    int pred = (s1 * filt[f][0] + s2 * filt[f][1] + 32) >> 6;
    int r = x[i] - pred;
    int q = (r + (r < 0 ? -step / 2 : step / 2)) / step;
    q = q < -8 ? -8 : q > 7 ? 7 : q;
    int d = clamp16(q * step + pred);
    err += (u64)((x[i] - d) * (x[i] - d));
    s2 = s1;
    s1 = d;
    b[2 + i / 2] |= (u8)((q & 15) << (i & 1 ? 4 : 0));
  }
  b[0] = (u8)(f << 4 | sh);
  *h1 = s1;
  *h2 = s2;
  return err;
}

u32 mx_snd_encode(const s16 *pcm, u32 n, u8 *out, int loop) {
  u32 blocks = (n + 27) / 28;
  int h1 = 0, h2 = 0;
  for (u32 k = 0; k < blocks; k++) {
    s16 x[28];
    for (u32 i = 0; i < 28; i++)
      x[i] = k * 28 + i < n ? pcm[k * 28 + i] : 0;
    u64 best = ~0ull;
    int b1 = 0, b2 = 0;
    u8 b[16];
    for (int f = 0; f < 5; f++) {
      /* the shift for the largest residual, predicted from the source */
      int p1 = h1, p2 = h2, m = 0;
      for (int i = 0; i < 28; i++) {
        int r = x[i] - ((p1 * filt[f][0] + p2 * filt[f][1] + 32) >> 6);
        r = r < 0 ? -r : r;
        m = r > m ? r : m;
        p2 = p1;
        p1 = x[i];
      }
      int sh = 12;
      while (sh > 0 && (7 << (12 - sh)) < m)
        sh--;
      for (int s = sh; s >= 0 && s >= sh - 1; s--) {
        int t1 = h1, t2 = h2;
        u64 e = try_block(x, f, s, &t1, &t2, b);
        if (e < best) {
          best = e;
          b1 = t1;
          b2 = t2;
          for (int i = 0; i < 16; i++)
            out[k * 16 + i] = b[i];
        }
      }
    }
    h1 = b1;
    h2 = b2;
    out[k * 16 + 1] = 0;
  }
  if (loop) {
    out[1] |= 4;                     /* loop start */
    out[(blocks - 1) * 16 + 1] |= 3; /* loop end, repeat */
  } else {
    out[(blocks - 1) * 16 + 1] |= 1; /* end: the voice is released */
  }
  return blocks * 16;
}


/* ---- samples ----------------------------------------------------------------- */

static u32 pitch(u32 rate) {
  u32 p = (rate * 4096u + MX_SND_RATE / 2) / MX_SND_RATE;
  return p > 0x3FFF ? 0x3FFF : p;
}

/* audsrv takes a 16-byte header (word 1: channels << 8, loop << 16; word 2:
   the pitch) and the ADPCM after it, in IOP memory; it copies the ADPCM to
   SPU RAM (libsd's voice transfer, waited for) and answers with 0. The
   copy here is padded to 64 bytes (the SPU2 DMA's block) with silence. */
int mx_snd_load(struct mx_snd_sample *s, const void *adpcm, u32 size, u32 rate) {
  if (!ready)
    return -1;
  u32 total = 16 + ((size + 63) & ~63u);
  u32 iop = mx_iop_alloc(total);
  if (!iop)
    return -1;
  const u8 *src = adpcm;
  for (u32 o = 0; o < total; o += CHUNK) {
    u32 n = total - o < CHUNK ? total - o : CHUNK;
    u8 *b = MX_UNCACHED(bounce);
    for (u32 i = 0; i < n; i++) {
      u32 at = o + i;
      b[i] = at < 16 ? 0 : at - 16 < size ? src[at - 16] : 0;
    }
    if (o == 0) {
      volatile u32 *h = (volatile u32 *)b;
      h[1] = 1u << 8;
      h[2] = pitch(rate);
    }
    if (mx_iop_write(iop + o, b, (n + 15) & ~15u) < 0) {
      mx_iop_free(iop);
      return -1;
    }
  }
  int r = call(AUDSRV_LOAD_ADPCM, iop, total, (u32)(uintptr_t)s);
  mx_iop_free(iop);
  if (r != 0)
    return -1;
  s->rate = rate;
  s->size = size;
  return 0;
}

int mx_snd_free(struct mx_snd_sample *s) {
  return call(AUDSRV_FREE_ADPCM, (u32)(uintptr_t)s, 0, 0) == 0 ? 0 : -1;
}

int mx_snd_play(const struct mx_snd_sample *s, int voice) {
  return call(AUDSRV_PLAY_ADPCM, (u32)voice, (u32)(uintptr_t)s, 0);
}

int mx_snd_playing(int voice, const struct mx_snd_sample *s) {
  return call(AUDSRV_IS_ADPCM_PLAYING, (u32)voice, (u32)(uintptr_t)s, 0) == 1;
}

void mx_snd_voice_volume(int voice, int left, int right) {
  call(AUDSRV_SET_ADPCM_VOL, (u32)voice, (u32)left, (u32)right);
}
