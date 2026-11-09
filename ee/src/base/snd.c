/* Sound through maxsdk's mxsnd.irx (iop/sound/mxsnd, common/include/mxsnd.h)
   on ps2sdk's libsd.irx: both carried in libmx as data (#embed, from the
   Makefile's --embed-dirs) and loaded from EE memory unless the IOP has them
   (mxlink loads them at boot). The EE writes the stream's bytes and the
   commands into mxsnd's rings in IOP memory by SIF DMA, then their counts;
   mxsnd works every 10.7 ms and writes its status (what it took, the voices
   past their end) by SIF DMA into area here. No call waits on the IOP but
   mx_snd_init and mx_snd_load. */

#include <mx/cpu.h>
#include <mx/iop.h>
#include <mx/sif.h>
#include <mx/snd.h>
#include <mx/string.h>
#include <mxsnd.h>

/* IOP modules as data: 16-byte aligned for the transfer to the IOP */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
static const u8 libsd_irx[] __attribute__((aligned(16))) = {
#embed <libsd.irx>
};
static const u8 mxsnd_irx[] __attribute__((aligned(16))) = {
#embed <mxsnd.irx>
};
#pragma clang diagnostic pop

#define AREA(f) (iop_area + (u32)__builtin_offsetof(struct mxsnd_iop, f))
#define STAGE 16384 /* the stream's bytes a transfer, at most */

static struct mx_sif_client srv;
static u32 arg[4] MX_DMA_LINES;
static u32 res[16] MX_DMA_LINES;
static struct mxsnd_area area MX_DMA_LINES;     /* written by mxsnd */
static struct mxsnd_head head MX_DMA_LINES;     /* sent last of a transfer */
static struct mxsnd_cmd cbuf[64] MX_DMA_LINES;  /* commands not sent yet */
static u8 stage[STAGE + 32] MX_DMA_LINES;       /* the stream's bytes on their way */
static u8 bounce[MXSND_UPLOAD_BYTES] MX_DMA_LINES;
static int ready, ncmd;
static u32 iop_area, stream_w, cmd_w, stream_r0, cmd_r0;
static u8 line[16]; /* the stream's bytes of the line stream_w is in (not a whole one yet) */
volatile u32 mx__snd_dropped; /* commands left out: the ring was full */

static int call(int fno, u32 a, u32 b) {
  volatile u32 *w = MX_UNCACHED(arg);
  w[0] = a, w[1] = b, w[2] = w[3] = 0;
  if (mx_sif_call(&srv, fno, (void *)w, 16, (void *)MX_UNCACHED(res), 16) < 0)
    return -1;
  return (int)MX_UNCACHED(res)[0];
}

/* Load and start the module from buf to end; < 0 if it does not load or does
   not stay (NO_RESIDENT_END). */
static int load(const u8 *buf, const u8 *end) {
  int r = 0;
  int id = mx_iop_load_module_buffer(buf, (u32)(end - buf), "", 0, &r);
  return id < 0 || (r & 3) == 1 ? -1 : 0;
}

/* mxsnd's status: the newer record (none yet: what INIT answered) */
static struct mxsnd_status status(void) {
  const volatile struct mxsnd_status *s = MX_UNCACHED(area.s);
  u32 a = s[0].seq, b = s[1].seq;
  struct mxsnd_status r;
  if (!a && !b) {
    memset(&r, 0, sizeof r);
    r.stream_r = stream_r0, r.cmd_r = cmd_r0;
    return r;
  }
  const volatile struct mxsnd_status *n = (s32)(a - b) > 0 ? &s[0] : &s[1];
  r.seq = n->seq, r.stream_r = n->stream_r, r.cmd_r = n->cmd_r;
  r.endx0 = n->endx0, r.endx1 = n->endx1, r.frames = n->frames, r.starved = n->starved;
  return r;
}

/* the commands in cbuf into the ring, then the head */
static void send(void) {
  if (ncmd) {
    struct mxsnd_status s = status();
    u32 room = MXSND_CMDS - (cmd_w - s.cmd_r);
    u32 n = (u32)ncmd < room ? (u32)ncmd : room;
    mx__snd_dropped += (u32)ncmd - n;
    for (u32 done = 0; done < n;) {
      u32 at = (cmd_w + done) & (MXSND_CMDS - 1);
      u32 k = MXSND_CMDS - at < n - done ? MXSND_CMDS - at : n - done;
      mx_iop_write(AREA(cmd) + at * 16, &cbuf[done], k * 16);
      done += k;
    }
    cmd_w += n;
    ncmd = 0;
  }
  volatile struct mxsnd_head *h = MX_UNCACHED(&head);
  h->stream_w = stream_w, h->cmd_w = cmd_w;
  mx_iop_write(AREA(head), (const void *)h, 16);
}

static struct mxsnd_cmd *cmd(int op, int voice) {
  if (ncmd == (int)(sizeof cbuf / sizeof cbuf[0]))
    send();
  struct mxsnd_cmd *c = &cbuf[ncmd++];
  memset(c, 0, sizeof *c);
  c->op = (u8)op, c->voice = (u8)voice;
  return c;
}

static void voices_reset(void);

int mx_snd_init(void) {
  if (ready)
    return 0;
  if (!mx_sif_ready() && mx_sif_init() < 0)
    return -1;
  if (mx_sif_bind_tries(&srv, MXSND_SID, 2) < 0 || !srv.server) {
    struct mx_iop_module m;
    if (!mx_iop_find_module("libsd", &m) && load(libsd_irx, libsd_irx + sizeof libsd_irx) < 0)
      return -1;
    if (load(mxsnd_irx, mxsnd_irx + sizeof mxsnd_irx) < 0)
      return -1;
    if (mx_sif_bind_tries(&srv, MXSND_SID, 100) < 0 || !srv.server)
      return -1;
  }
  memset(MX_UNCACHED(&area), 0, sizeof area);
  if (call(MXSND_INIT, mx_paddr_of(&area).v, 0) < 0)
    return -1;
  const volatile struct mxsnd_init *in = (const volatile struct mxsnd_init *)MX_UNCACHED(res);
  if (in->version != MXSND_VERSION)
    return -1;
  iop_area = in->area;
  stream_w = stream_r0 = in->stream_r;
  cmd_w = cmd_r0 = in->cmd_r;
  ncmd = 0;
  voices_reset();
  ready = 1;
  return 0;
}

void mx_snd_commit(void) {
  if (ready)
    send();
}

/* ---- the PCM stream ---------------------------------------------------------- */

/* the most queued: about 100 ms of the format, as audsrv kept */
static u32 fill = 19200, frame_bytes = 4;

int mx_snd_stream_format(u32 rate, int bits, int channels) {
  if (!ready || !rate || rate > 48000 || (bits != 8 && bits != 16) || (channels != 1 && channels != 2))
    return -1;
  struct mxsnd_cmd *c = cmd(MXSND_C_FORMAT, 0);
  c->addr = rate, c->voll = (u16)bits, c->volr = (u16)channels;
  frame_bytes = (u32)(bits / 8 * channels);
  u32 f = rate * frame_bytes / 10;
  f = f < 4096 ? 4096 : f > MXSND_STREAM_BYTES - 64 ? MXSND_STREAM_BYTES - 64 : f;
  fill = f - f % frame_bytes;
  send();
  return 0;
}

u32 mx_snd_stream_queued(void) {
  if (!ready)
    return 0;
  return stream_w - status().stream_r;
}

u32 mx_snd_stream_room(void) {
  if (!ready)
    return 0;
  u32 q = mx_snd_stream_queued();
  return q < fill ? fill - q : 0;
}

/* The bytes go into the ring by whole 16-byte lines from the line stream_w
   is in (its bytes so far kept in line[]): what is after stream_w in the
   last line is not read */
u32 mx_snd_stream_queue(const void *data, u32 bytes) {
  u32 room = mx_snd_stream_room();
  u32 n = bytes < room ? bytes : room;
  n = n < STAGE ? n : STAGE;
  if (!n)
    return 0;
  u32 at = stream_w & (MXSND_STREAM_BYTES - 1), off = at & 15;
  u8 *st = stage; /* cached: mx_iop_write writes it back */
  memcpy(st, line, off);
  memcpy(st + off, data, n);
  u32 len = (off + n + 15) & ~15u, base = at - off;
  for (u32 done = 0; done < len;) {
    u32 p = (base + done) & (MXSND_STREAM_BYTES - 1);
    u32 k = MXSND_STREAM_BYTES - p < len - done ? MXSND_STREAM_BYTES - p : len - done;
    mx_iop_write(AREA(stream) + p, st + done, k);
    done += k;
  }
  u32 end = off + n;
  memcpy(line, st + (end & ~15u), end & 15);
  stream_w += n;
  send();
  return n;
}

void mx_snd_stream_stop(void) {
  if (!ready)
    return;
  cmd(MXSND_C_STREAM_DROP, 0)->addr = stream_w;
  send();
}

/* the home menu (ee/src/gs/home.c): the stream and the voices silenced while
   the program waits */
void mx__snd_quiet(void) {
  if (!ready)
    return;
  cmd(MXSND_C_SILENCE, 0);
  mx_snd_stream_stop();
}

void mx_snd_stream_volume(int volume) {
  if (!ready)
    return;
  cmd(MXSND_C_STREAM_VOL, 0)->voll = (u16)(volume < 0 ? 0 : volume > MX_SND_VOL_MAX ? MX_SND_VOL_MAX : volume);
  send();
}

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


u32 mx_snd_encode_loop(const s16 *pcm, u32 n, u8 *out, u32 loop_start) {
  u32 bytes = mx_snd_encode(pcm, n, out, 0), blocks = bytes / 16;
  u32 k = loop_start / 28 < blocks ? loop_start / 28 : blocks - 1;
  out[(blocks - 1) * 16 + 1] = 3; /* loop end, repeat */
  out[k * 16 + 1] |= 4;           /* loop start */
  return bytes;
}

/* ---- samples ----------------------------------------------------------------- */

#define ADSR1 0x000F /* attack at once (+lin, rate 0), sustain level 15: full */
#define ADSR2 0x1FC0 /* sustain unchanged (+lin, rate 0x7F), release at once (-lin, rate 0) */

static u16 pitch(u32 rate) {
  u32 p = (rate * 4096u + MX_SND_RATE / 2) / MX_SND_RATE;
  return (u16)(p > 0x3FFF ? 0x3FFF : p);
}

/* SPU RAM: the samples' blocks, by address */
#define MAX_BLOCKS 1024
static struct {
  u32 at, size;
} blk[MAX_BLOCKS];
static int nblk;

static u32 spu_alloc(u32 size) {
  u32 at = MXSND_SPU_START;
  int i = 0;
  for (; i < nblk; i++) {
    if (blk[i].at - at >= size)
      break;
    at = blk[i].at + blk[i].size;
  }
  if (nblk == MAX_BLOCKS || (i == nblk && MXSND_SPU_END - at < size))
    return 0;
  memmove(&blk[i + 1], &blk[i], (u32)(nblk - i) * sizeof blk[0]);
  blk[i].at = at, blk[i].size = size;
  nblk++;
  return at;
}

static void spu_free(u32 at) {
  for (int i = 0; i < nblk; i++)
    if (blk[i].at == at) {
      memmove(&blk[i], &blk[i + 1], (u32)(nblk - i - 1) * sizeof blk[0]);
      nblk--;
      return;
    }
}

/* the voices as the EE left them: what plays, from which command on */
static struct {
  const struct mx_snd_sample *s;
  u32 kon;   /* the count of commands up to its PLAY: done when cmd_r reaches it */
  u16 gen;   /* a channel's handle has it: the voice was taken since when it differs */
  u8 on, prio, dry;
  u16 vl, vr, pitch;
} vs[MXSND_VOICES];

/* mxsnd silenced them (INIT): none on, all into the reverb */
static void voices_reset(void) { memset(vs, 0, sizeof vs); }

/* the voice still sounds: on, and (a loop-less sample) not past its end */
static int sounding(int v, const struct mxsnd_status *st) {
  if (!vs[v].on)
    return 0;
  if (vs[v].s && vs[v].s->loop)
    return 1;
  if ((s32)(st->cmd_r - vs[v].kon) < 0)
    return 1; /* its PLAY is not done yet */
  u32 endx = v < 24 ? st->endx0 >> v : st->endx1 >> (v - 24);
  if (endx & 1) {
    vs[v].on = 0;
    return 0;
  }
  return 1;
}

static void voice_play(int v, const struct mx_snd_sample *s, u32 rate, int l, int r, int prio) {
  struct mxsnd_cmd *c = cmd(MXSND_C_PLAY, v);
  vs[v].kon = cmd_w + (u32)ncmd;
  vs[v].s = s, vs[v].on = 1, vs[v].prio = (u8)prio, vs[v].gen++;
  vs[v].vl = (u16)l, vs[v].vr = (u16)r, vs[v].pitch = pitch(rate);
  c->addr = s->spu, c->pitch = vs[v].pitch, c->voll = vs[v].vl, c->volr = vs[v].vr;
  c->adsr1 = ADSR1, c->adsr2 = ADSR2;
  if (vs[v].dry) { /* into the reverb again, the default */
    cmd(MXSND_C_WET, v)->voll = 1;
    vs[v].dry = 0;
  }
}

static int vol(int v) { return v < 0 ? 0 : v > MX_SND_VOL_MAX ? MX_SND_VOL_MAX : v; }

/* mxsnd takes the ADPCM in pieces of its upload buffer, padded to 64 bytes
   (the SPU2 DMA's block) with silence */
int mx_snd_load(struct mx_snd_sample *s, const void *adpcm, u32 size, u32 rate) {
  if (!ready || !size)
    return -1;
  u32 total = (size + 63) & ~63u;
  u32 at = spu_alloc(total);
  if (!at)
    return -1;
  const u8 *src = adpcm;
  for (u32 o = 0; o < total; o += MXSND_UPLOAD_BYTES) {
    u32 n = total - o < MXSND_UPLOAD_BYTES ? total - o : MXSND_UPLOAD_BYTES;
    u32 have = size - o < n ? size - o : n;
    memcpy(bounce, src + o, have);
    memset(bounce + have, 0, n - have);
    if (mx_iop_write(AREA(upload), bounce, n) < 0 || call(MXSND_UPLOAD, at + o, n) != 0) {
      spu_free(at);
      return -1;
    }
  }
  const u8 *last = src + size - 16;
  s->rate = rate;
  s->size = size;
  s->spu = at;
  s->loop = (src[1] & 4) || (last[1] & 3) == 3;
  return 0;
}

int mx_snd_free(struct mx_snd_sample *s) {
  if (!ready || !s->spu)
    return -1;
  for (int v = 0; v < MXSND_VOICES; v++)
    if (vs[v].on && vs[v].s == s) {
      cmd(MXSND_C_STOP, v);
      vs[v].on = 0, vs[v].s = 0;
    }
  send(); /* the voices stop before the RAM is used again */
  spu_free(s->spu);
  s->spu = 0;
  return 0;
}

int mx_snd_play(const struct mx_snd_sample *s, int voice) {
  if (!ready || !s->spu)
    return -1;
  if (voice < 0) {
    struct mxsnd_status st = status();
    for (int v = 0; v < MXSND_VOICES && voice < 0; v++)
      if (!sounding(v, &st))
        voice = v;
    if (voice < 0)
      return -1;
  } else if (voice >= MXSND_VOICES)
    return -1;
  voice_play(voice, s, s->rate, MX_SND_VOL_MAX, MX_SND_VOL_MAX, 255);
  send();
  return voice;
}

int mx_snd_playing(int voice, const struct mx_snd_sample *s) {
  if (!ready || voice < 0 || voice >= MXSND_VOICES || vs[voice].s != s)
    return 0;
  struct mxsnd_status st = status();
  return sounding(voice, &st);
}

void mx_snd_voice_volume(int voice, int left, int right) {
  if (!ready || voice < 0 || voice >= MXSND_VOICES)
    return;
  vs[voice].vl = (u16)vol(left), vs[voice].vr = (u16)vol(right);
  struct mxsnd_cmd *c = cmd(MXSND_C_SET, voice);
  c->pitch = vs[voice].pitch, c->voll = vs[voice].vl, c->volr = vs[voice].vr;
  send();
}

/* ---- channels ------------------------------------------------------------------ */

/* a handle: the voice + 1 in the low byte, its gen above */
static int ch_voice(int ch) {
  int v = (ch & 0xFF) - 1;
  if (!ready || v < 0 || v >= MXSND_VOICES || (u16)(ch >> 8) != vs[v].gen || !vs[v].on)
    return -1;
  return v;
}

int mx_snd_ch_play(const struct mx_snd_sample *s, u32 rate, int left, int right, int priority) {
  if (!ready || !s->spu)
    return 0;
  left = vol(left), right = vol(right);
  int loud = left > right ? left : right;
  priority = priority < 0 ? 0 : priority > 255 ? 255 : priority;
  struct mxsnd_status st = status();
  int v = -1, worst = -1;
  for (int i = 0; i < MXSND_VOICES; i++) {
    if (!sounding(i, &st)) {
      v = i;
      break;
    }
    /* the one to take when none is free: the lowest priority, then the quietest */
    if (worst < 0 || vs[i].prio < vs[worst].prio ||
        (vs[i].prio == vs[worst].prio &&
         (vs[i].vl > vs[i].vr ? vs[i].vl : vs[i].vr) < (vs[worst].vl > vs[worst].vr ? vs[worst].vl : vs[worst].vr)))
      worst = i;
  }
  if (v < 0) {
    int wl = vs[worst].vl > vs[worst].vr ? vs[worst].vl : vs[worst].vr;
    if (priority < vs[worst].prio || (priority == vs[worst].prio && loud <= wl))
      return 0;
    v = worst;
  }
  voice_play(v, s, rate, left, right, priority);
  return (int)vs[v].gen << 8 | (v + 1);
}

int mx_snd_ch_set(int ch, u32 rate, int left, int right) {
  int v = ch_voice(ch);
  if (v < 0)
    return -1;
  u16 l = (u16)vol(left), r = (u16)vol(right), p = pitch(rate);
  if (l != vs[v].vl || r != vs[v].vr || p != vs[v].pitch) {
    vs[v].vl = l, vs[v].vr = r, vs[v].pitch = p;
    struct mxsnd_cmd *c = cmd(MXSND_C_SET, v);
    c->pitch = p, c->voll = l, c->volr = r;
  }
  return 0;
}

void mx_snd_ch_stop(int ch) {
  int v = ch_voice(ch);
  if (v < 0)
    return;
  cmd(MXSND_C_STOP, v);
  vs[v].on = 0;
}

int mx_snd_ch_playing(int ch) {
  int v = ch_voice(ch);
  if (v < 0)
    return 0;
  struct mxsnd_status st = status();
  return sounding(v, &st);
}

void mx_snd_ch_reverb(int ch, int on) {
  int v = ch_voice(ch);
  if (v < 0 || vs[v].dry == !on)
    return;
  vs[v].dry = !on;
  cmd(MXSND_C_WET, v)->voll = (u16)(on != 0);
}

void mx_snd_reverb(int mode, int depth, int delay, int feedback, int stream) {
  if (!ready || mode < 0 || mode > MX_SND_REVERB_PIPE)
    return;
  struct mxsnd_cmd *c = cmd(MXSND_C_REVERB, stream != 0);
  c->addr = (u32)mode;
  c->voll = (u16)(depth < 0 ? 0 : depth > 0x7FFF ? 0x7FFF : depth);
  delay = delay < 0 ? 0 : delay > 127 ? 127 : delay;
  feedback = feedback < 0 ? 0 : feedback > 127 ? 127 : feedback;
  c->volr = (u16)(delay | feedback << 8);
  send();
}

/* SPU RAM at spu (bytes, 64-byte multiples, up to MXSND_UPLOAD_BYTES) into
   dst (tests: the voices' mix, MXSND_SPU_MIX0 / MIX1) */
int mx__snd_peek(u32 spu, void *dst, u32 bytes) {
  if (!ready || call(MXSND_PEEK, spu, bytes) != 0)
    return -1;
  return mx_iop_read(AREA(upload), dst, bytes);
}

/* frames played and frames the stream starved, at 48 kHz (for tests) */
void mx__snd_counts(u32 *frames, u32 *starved) {
  struct mxsnd_status st = status();
  *frames = st.frames, *starved = st.starved;
}
