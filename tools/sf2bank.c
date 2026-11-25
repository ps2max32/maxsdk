/* sf2bank: a SoundFont 2 file to a bank for maxsdk's MIDI synth (mx/midi.h):
   its samples as SPU ADPCM in one blob, its presets' zones with the SPU2's
   envelope (ADSR1/ADSR2) worked out here.

     cc -O2 -o sf2bank tools/sf2bank.c -lm
     sf2bank [-r maxrate] in.sf2 out.bnk

   What the SPU2 cannot do is left out: filters, LFOs, the modulation
   envelope, modulators, delay and hold of the volume envelope. A loop must
   start on a block of 28 samples and be a whole number of them long: the
   sample is unrolled and resampled (a little) to make it so, and silence
   put before it. -r resamples samples above maxrate down to it (less SPU
   RAM). The format is in ee/include/mx/midi.h. */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *sf;
static size_t sflen;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

static void die(const char *m) {
  fprintf(stderr, "sf2bank: %s\n", m);
  exit(1);
}

/* the chunks of pdta and smpl */
static const uint8_t *phdr, *pbag, *pgen, *inst, *ibag, *igen, *shdr, *smpl;
static uint32_t nphdr, npbag, npgen, ninst, nibag, nigen, nshdr, nsmpl;

static void walk(size_t o, size_t e) {
  while (o + 8 <= e) {
    const uint8_t *id = sf + o;
    uint32_t sz = rd32(sf + o + 4);
    if (o + 8 + sz > e)
      die("truncated chunk");
    const uint8_t *d = sf + o + 8;
    if (!memcmp(id, "LIST", 4))
      walk(o + 12, o + 8 + sz);
#define CH(name, var, n, size)                                                                     \
  else if (!memcmp(id, name, 4)) var = d, n = sz / size;
    CH("phdr", phdr, nphdr, 38)
    CH("pbag", pbag, npbag, 4)
    CH("pgen", pgen, npgen, 4)
    CH("inst", inst, ninst, 22)
    CH("ibag", ibag, nibag, 4)
    CH("igen", igen, nigen, 4)
    CH("shdr", shdr, nshdr, 46)
    CH("smpl", smpl, nsmpl, 2)
    o += 8 + sz + (sz & 1);
  }
}

/* generators */
enum {
  G_START = 0, G_END = 1, G_LSTART = 2, G_LEND = 3, G_START_C = 4, G_END_C = 12, G_PAN = 17,
  G_DELAY = 33, G_ATTACK = 34, G_HOLD = 35, G_DECAY = 36, G_SUSTAIN = 37, G_RELEASE = 38,
  G_INST = 41, G_KEYS = 43, G_VELS = 44, G_LSTART_C = 45, G_ATTEN = 48, G_LEND_C = 50,
  G_COARSE = 51, G_FINE = 52, G_SAMPLE = 53, G_MODES = 54, G_SCALE = 56, G_EXCL = 57,
  G_ROOT = 58, G_N = 61
};

typedef struct {
  int v[G_N];
  int set[G_N];
} gens;

static void gens_default(gens *g) {
  memset(g, 0, sizeof *g);
  g->v[G_KEYS] = 127 << 8, g->v[G_VELS] = 127 << 8;
  g->v[G_DELAY] = g->v[G_ATTACK] = g->v[G_HOLD] = g->v[G_DECAY] = g->v[G_RELEASE] = -12000;
  g->v[G_SCALE] = 100, g->v[G_ROOT] = -1;
}

static void gens_read(gens *g, const uint8_t *gen, uint32_t from, uint32_t to) {
  for (uint32_t i = from; i < to; i++) {
    int op = rd16(gen + i * 4);
    if (op >= G_N)
      continue;
    g->v[op] = op == G_KEYS || op == G_VELS ? rd16(gen + i * 4 + 2) : (int16_t)rd16(gen + i * 4 + 2);
    g->set[op] = 1;
  }
}

/* ---- the SPU2's envelope (SPU2 Overview Manual 4.1) ------------------------ */

/* +lin, 0 -> 1, rate v (0 .. 127) */
static double t_lin_up(int v) { return 0.013 * pow(2, (v >> 2) - 8) * 7.0 / (7 - (v & 3)); }
/* -exp, 1 -> 0.1 (20 dB), rate v (0 .. 127; the decay and release rates are v / 4) */
static double t_exp_down(int v) { return 0.109 * pow(2, (v >> 2) - 10) * 8.0 / (8 - (v & 3)); }

static int best(double (*t)(int), int step, int max, double want) {
  int b = 0;
  double be = 1e30;
  for (int v = 0; v <= max; v += step) {
    double e = fabs(log(t(v) / want));
    if (e < be)
      be = e, b = v;
  }
  return b;
}

static double tc(int timecents) { return pow(2, timecents / 1200.0); }

/* SF2's decay and release are times for the whole range, 100 dB; the table's
   for 20 dB */
static void envelope(const gens *g, uint16_t *adsr1, uint16_t *adsr2) {
  double attack = tc(g->v[G_ATTACK]), decay = tc(g->v[G_DECAY]), release = tc(g->v[G_RELEASE]);
  int sus = g->v[G_SUSTAIN] < 0 ? 0 : g->v[G_SUSTAIN];
  double level = pow(10, -sus / 200.0);
  if (release < 0.01)
    release = 0.01; /* no click */
  int ar = best(t_lin_up, 1, 127, attack);
  int dr = best(t_exp_down, 4, 60, decay / 5) >> 2;
  int rr = best(t_exp_down, 4, 124, release / 5) >> 2;
  int sl, y, sr;
  if (level >= 0.75 / 16) { /* held at the sustain level */
    sl = (int)lround(level * 16) - 1;
    sl = sl < 0 ? 0 : sl > 15 ? 15 : sl;
    y = 0, sr = 0x7F; /* +lin, infinity */
  } else { /* decays to silence: down to 1/16, then on at the same rate */
    sl = 0;
    y = 6, sr = best(t_exp_down, 1, 107, decay / 5);
  }
  *adsr1 = (uint16_t)(ar << 8 | dr << 4 | sl);
  *adsr2 = (uint16_t)(y << 13 | sr << 6 | 1 << 5 | rr);
}

/* ---- samples ----------------------------------------------------------------- */

/* the ADPCM encoder of ee/src/base/snd.c (mx_snd_encode) */
static const int filt[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

static uint64_t try_block(const int16_t *x, int f, int sh, int *h1, int *h2, uint8_t *b) {
  int s1 = *h1, s2 = *h2, step = 1 << (12 - sh);
  uint64_t err = 0;
  memset(b + 2, 0, 14);
  for (int i = 0; i < 28; i++) {
    int pred = (s1 * filt[f][0] + s2 * filt[f][1] + 32) >> 6;
    int r = x[i] - pred;
    int q = (r + (r < 0 ? -step / 2 : step / 2)) / step;
    q = q < -8 ? -8 : q > 7 ? 7 : q;
    int d = clamp16(q * step + pred);
    err += (uint64_t)((x[i] - d) * (x[i] - d));
    s2 = s1, s1 = d;
    b[2 + i / 2] |= (uint8_t)((q & 15) << (i & 1 ? 4 : 0));
  }
  b[0] = (uint8_t)(f << 4 | sh);
  *h1 = s1, *h2 = s2;
  return err;
}

/* n samples (a multiple of 28) to n / 28 blocks; flags set by the caller */
static void encode(const int16_t *pcm, uint32_t n, uint8_t *out) {
  int h1 = 0, h2 = 0;
  for (uint32_t k = 0; k < n / 28; k++) {
    const int16_t *x = pcm + k * 28;
    uint64_t be = ~0ull;
    int b1 = 0, b2 = 0;
    uint8_t b[16];
    for (int f = 0; f < 5; f++) {
      int p1 = h1, p2 = h2, m = 0;
      for (int i = 0; i < 28; i++) {
        int r = x[i] - ((p1 * filt[f][0] + p2 * filt[f][1] + 32) >> 6);
        r = r < 0 ? -r : r;
        m = r > m ? r : m;
        p2 = p1, p1 = x[i];
      }
      int sh = 12;
      while (sh > 0 && (7 << (12 - sh)) < m)
        sh--;
      for (int s = sh; s >= 0 && s >= sh - 1; s--) {
        int t1 = h1, t2 = h2;
        uint64_t e = try_block(x, f, s, &t1, &t2, b);
        if (e < be) {
          be = e, b1 = t1, b2 = t2;
          memcpy(out + k * 16, b, 16);
        }
      }
    }
    h1 = b1, h2 = b2;
    out[k * 16 + 1] = 0;
  }
}

/* a sample as used by zones: the same source and loop make the same one */
typedef struct {
  uint32_t start, end, ls, le, loop, rate; /* the source */
  uint32_t offset, size, outrate;          /* the ADPCM */
  double ratio;                            /* outrate / rate */
  uint32_t pad;                            /* silence put before it, in output samples */
} sample;

static sample *smp;
static int nsmp, capsmp;
static uint8_t *blob;
static size_t nblob, capblob;
static int maxrate;

static int16_t src_at(uint32_t i) { return i < nsmpl ? (int16_t)rd16(smpl + i * 2) : 0; }

/* the source at position t (from its start), looping when it loops */
static double src_lerp(const sample *s, double t, uint32_t len, uint32_t lstart, uint32_t llen) {
  if (llen && t >= lstart + llen)
    t = lstart + fmod(t - lstart, llen);
  uint32_t i = (uint32_t)t;
  double f = t - i, a, b;
  uint32_t j = i + 1;
  if (llen && j >= lstart + llen)
    j = lstart + (j - lstart) % llen;
  a = i < len ? src_at(s->start + i) : 0;
  b = j < len ? src_at(s->start + j) : 0;
  return a + (b - a) * f;
}

static int add_sample(uint32_t start, uint32_t end, uint32_t ls, uint32_t le, int loop, uint32_t rate) {
  if (end > nsmpl)
    end = nsmpl;
  if (start >= end)
    return -1;
  if (loop && (ls < start || le > end || le <= ls + 1))
    loop = 0;
  for (int i = 0; i < nsmp; i++)
    if (smp[i].start == start && smp[i].end == end && smp[i].loop == (uint32_t)loop &&
        (!loop || (smp[i].ls == ls && smp[i].le == le)) && smp[i].rate == rate)
      return i;
  if (nsmp == capsmp)
    smp = realloc(smp, (capsmp = capsmp * 2 + 64) * sizeof *smp);
  sample *s = &smp[nsmp];
  memset(s, 0, sizeof *s);
  s->start = start, s->end = end, s->ls = ls, s->le = le, s->loop = (uint32_t)loop, s->rate = rate;

  double r0 = maxrate && rate > (uint32_t)maxrate ? (double)maxrate / rate : 1;
  uint32_t len = end - start, lstart = 0, llen = 0, k = 1;
  double ratio = r0;
  uint32_t outlen, outls = 0, outll = 0;
  if (loop) {
    lstart = ls - start, llen = le - ls;
    /* unroll the loop k times so that k loops resampled are close to whole blocks */
    double be = 1e9;
    for (uint32_t kk = 1; kk * llen <= 4096 || kk == 1; kk++) {
      double want = kk * llen * r0;
      double blocks = floor(want / 28 + 0.5);
      if (blocks < 1)
        blocks = 1;
      double e = fabs(blocks * 28 / want - 1);
      if (e < be - 1e-9)
        be = e, k = kk;
      if (e < 0.001)
        break;
    }
    double blocks = floor(k * llen * r0 / 28 + 0.5);
    if (blocks < 1)
      blocks = 1;
    outll = (uint32_t)blocks * 28;
    ratio = outll / (double)(k * llen);
    outls = (uint32_t)lround(lstart * ratio);
    s->pad = (28 - outls % 28) % 28;
    outlen = s->pad + outls + outll;
    llen *= 1; /* the source's loop: llen, unrolled k times = the same signal */
  } else {
    outlen = (uint32_t)ceil(len * ratio);
    outlen = (outlen + 27) / 28 * 28;
  }
  int16_t *pcm = calloc(outlen, 2);
  uint32_t body = loop ? outls + outll : outlen;
  for (uint32_t i = 0; i < body && s->pad + i < outlen; i++) {
    double t = i / ratio;
    if (!loop && t >= len)
      break;
    double v = src_lerp(s, t, len, lstart, llen);
    pcm[s->pad + i] = (int16_t)clamp16((int)lround(v));
  }
  uint32_t bytes = outlen / 28 * 16;
  if (nblob + bytes > capblob)
    blob = realloc(blob, capblob = (nblob + bytes) * 2);
  uint8_t *out = blob + nblob;
  encode(pcm, outlen, out);
  if (loop) {
    out[(s->pad + outls) / 28 * 16 + 1] |= 4; /* loop start */
    out[bytes - 16 + 1] |= 3;                 /* loop end, repeat */
  } else {
    out[bytes - 16 + 1] |= 1; /* end */
  }
  free(pcm);
  s->offset = (uint32_t)nblob, s->size = bytes;
  s->ratio = ratio, s->outrate = (uint32_t)lround(rate * ratio);
  nblob += bytes;
  return nsmp++;
}

/* ---- zones ------------------------------------------------------------------- */

typedef struct {
  uint8_t keylo, keyhi, vello, velhi;
  uint16_t sample;
  uint8_t root, excl;
  int16_t tune, scale, pan;
  uint16_t atten, adsr1, adsr2;
} zone;

typedef struct {
  uint16_t bank, program, zone, nzones;
} preset;

static zone *zn;
static int nzn, capzn;
static preset pr[256];
static int npr;

static int is_global(const uint8_t *bag, const uint8_t *gen, uint32_t b, uint32_t nbag, int last_op) {
  uint32_t from = rd16(bag + b * 4), to = b + 1 < nbag ? rd16(bag + (b + 1) * 4) : from;
  return from == to || rd16(gen + (to - 1) * 4) != last_op;
}

static void add_zone(const gens *pg, const gens *ig) {
  int lo = ig->v[G_KEYS] & 255, hi = ig->v[G_KEYS] >> 8;
  int plo = pg->v[G_KEYS] & 255, phi = pg->v[G_KEYS] >> 8;
  int vlo = ig->v[G_VELS] & 255, vhi = ig->v[G_VELS] >> 8;
  int pvlo = pg->v[G_VELS] & 255, pvhi = pg->v[G_VELS] >> 8;
  lo = lo > plo ? lo : plo, hi = hi < phi ? hi : phi;
  vlo = vlo > pvlo ? vlo : pvlo, vhi = vhi < pvhi ? vhi : pvhi;
  if (lo > hi || vlo > vhi)
    return;
  int si = ig->v[G_SAMPLE];
  if (si < 0 || (uint32_t)si + 1 >= nshdr)
    return;
  const uint8_t *sh = shdr + si * 46;
  uint16_t type = rd16(sh + 44);
  if (type & 0x8000)
    return; /* ROM */
  if (type & 2)
    return; /* the right half of a stereo pair: the left one plays, centred */
  /* the sum: the instrument's values, the preset's added */
  gens g = *ig;
  static const int add[] = {G_PAN, G_ATTACK, G_DECAY, G_SUSTAIN, G_RELEASE, G_ATTEN, G_COARSE, G_FINE, G_SCALE};
  for (unsigned i = 0; i < sizeof add / sizeof add[0]; i++)
    if (pg->set[add[i]])
      g.v[add[i]] += pg->v[add[i]];
  uint32_t start = rd32(sh + 20) + (uint32_t)(g.v[G_START] + 32768 * g.v[G_START_C]);
  uint32_t end = rd32(sh + 24) + (uint32_t)(g.v[G_END] + 32768 * g.v[G_END_C]);
  uint32_t ls = rd32(sh + 28) + (uint32_t)(g.v[G_LSTART] + 32768 * g.v[G_LSTART_C]);
  uint32_t le = rd32(sh + 32) + (uint32_t)(g.v[G_LEND] + 32768 * g.v[G_LEND_C]);
  uint32_t rate = rd32(sh + 36);
  int loop = (g.v[G_MODES] & 1) != 0;
  int s = add_sample(start, end, ls, le, loop, rate);
  if (s < 0)
    return;
  if (nzn == capzn)
    zn = realloc(zn, (capzn = capzn * 2 + 256) * sizeof *zn);
  zone *z = &zn[nzn++];
  z->keylo = (uint8_t)lo, z->keyhi = (uint8_t)hi, z->vello = (uint8_t)vlo, z->velhi = (uint8_t)vhi;
  z->sample = (uint16_t)s;
  z->root = (uint8_t)(g.v[G_ROOT] >= 0 ? g.v[G_ROOT] : sh[40]);
  z->excl = (uint8_t)g.v[G_EXCL];
  z->tune = (int16_t)(g.v[G_COARSE] * 100 + g.v[G_FINE] + (int8_t)sh[41]);
  z->scale = (int16_t)g.v[G_SCALE];
  z->pan = (int16_t)(type & 4 ? 0 : g.v[G_PAN]);
  z->atten = (uint16_t)(g.v[G_ATTEN] < 0 ? 0 : g.v[G_ATTEN]);
  envelope(&g, &z->adsr1, &z->adsr2);
}

static void do_preset(uint32_t p) {
  const uint8_t *h = phdr + p * 38;
  uint16_t prog = rd16(h + 20), bank = rd16(h + 22);
  uint32_t b0 = rd16(h + 24), b1 = rd16(h + 38 + 24);
  if (npr == 256)
    die("too many presets");
  preset *P = &pr[npr++];
  P->bank = bank, P->program = prog, P->zone = (uint16_t)nzn;
  gens pglobal;
  memset(&pglobal, 0, sizeof pglobal);
  pglobal.v[G_KEYS] = 127 << 8, pglobal.v[G_VELS] = 127 << 8;
  for (uint32_t b = b0; b < b1; b++) {
    uint32_t g0 = rd16(pbag + b * 4), g1 = rd16(pbag + (b + 1) * 4);
    if (b == b0 && is_global(pbag, pgen, b, npbag, G_INST)) {
      gens_read(&pglobal, pgen, g0, g1);
      continue;
    }
    gens pg = pglobal;
    gens_read(&pg, pgen, g0, g1);
    if (!pg.set[G_INST])
      continue;
    int i = pg.v[G_INST];
    if (i < 0 || (uint32_t)i + 1 >= ninst)
      continue;
    uint32_t ib0 = rd16(inst + i * 22 + 20), ib1 = rd16(inst + (i + 1) * 22 + 20);
    gens iglobal;
    gens_default(&iglobal);
    for (uint32_t ib = ib0; ib < ib1; ib++) {
      uint32_t ig0 = rd16(ibag + ib * 4), ig1 = rd16(ibag + (ib + 1) * 4);
      if (ib == ib0 && is_global(ibag, igen, ib, nibag, G_SAMPLE)) {
        gens_read(&iglobal, igen, ig0, ig1);
        continue;
      }
      gens ig = iglobal;
      gens_read(&ig, igen, ig0, ig1);
      if (ig.set[G_SAMPLE])
        add_zone(&pg, &ig);
    }
  }
  P->nzones = (uint16_t)(nzn - P->zone);
}

static int cmp_preset(const void *a, const void *b) {
  const preset *x = a, *y = b;
  return x->bank != y->bank ? x->bank - y->bank : x->program - y->program;
}

static void put16(FILE *f, unsigned v) { fputc(v & 255, f), fputc(v >> 8 & 255, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFF), put16(f, v >> 16); }

int main(int argc, char **argv) {
  int a = 1;
  if (a + 1 < argc && !strcmp(argv[a], "-r"))
    maxrate = atoi(argv[a + 1]), a += 2;
  if (argc - a != 2) {
    fprintf(stderr, "usage: sf2bank [-r maxrate] in.sf2 out.bnk\n");
    return 64;
  }
  FILE *f = fopen(argv[a], "rb");
  if (!f)
    die("cannot open the SF2");
  fseek(f, 0, SEEK_END);
  sflen = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  sf = malloc(sflen);
  if (fread(sf, 1, sflen, f) != sflen)
    die("cannot read the SF2");
  fclose(f);
  if (sflen < 12 || memcmp(sf, "RIFF", 4) || memcmp(sf + 8, "sfbk", 4))
    die("not a SoundFont 2 file");
  walk(12, sflen);
  if (!phdr || !pbag || !pgen || !inst || !ibag || !igen || !shdr || !smpl)
    die("a chunk is missing");
  for (uint32_t p = 0; p + 1 < nphdr; p++)
    do_preset(p);
  qsort(pr, (size_t)npr, sizeof pr[0], cmp_preset);

  /* MXGM, version 1: head, samples, presets, zones, then the ADPCM (64-byte aligned) */
  uint32_t head = 32, ssz = 16, psz = 8, zsz = 20;
  uint32_t at = head + (uint32_t)nsmp * ssz + (uint32_t)npr * psz + (uint32_t)nzn * zsz;
  uint32_t data = (at + 63) & ~63u;
  f = fopen(argv[a + 1], "wb");
  if (!f)
    die("cannot write the bank");
  fwrite("MXGM", 1, 4, f);
  put32(f, 1);
  put32(f, (uint32_t)nsmp), put32(f, (uint32_t)npr), put32(f, (uint32_t)nzn);
  put32(f, (uint32_t)nblob), put32(f, data), put32(f, 0);
  for (int i = 0; i < nsmp; i++)
    put32(f, smp[i].offset), put32(f, smp[i].size), put32(f, smp[i].outrate), put32(f, smp[i].loop);
  for (int i = 0; i < npr; i++)
    put16(f, pr[i].bank), put16(f, pr[i].program), put16(f, pr[i].zone), put16(f, pr[i].nzones);
  for (int i = 0; i < nzn; i++) {
    zone *z = &zn[i];
    fputc(z->keylo, f), fputc(z->keyhi, f), fputc(z->vello, f), fputc(z->velhi, f);
    put16(f, z->sample), fputc(z->root, f), fputc(z->excl, f);
    put16(f, (uint16_t)z->tune), put16(f, (uint16_t)z->scale), put16(f, (uint16_t)z->pan);
    put16(f, z->atten), put16(f, z->adsr1), put16(f, z->adsr2);
  }
  for (; at < data; at++)
    fputc(0, f);
  fwrite(blob, 1, nblob, f);
  fclose(f);
  fprintf(stderr, "sf2bank: %d presets, %d zones, %d samples, %zu bytes of ADPCM\n", npr, nzn, nsmp, nblob);
  return 0;
}
