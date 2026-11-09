/* mx/snd.h: sound through mxsnd. Listen for, in turn:
     a 375 Hz tone, the PCM stream (48 kHz 16-bit stereo), 3 seconds (or the
       seconds of the argument): the stream has to take 192000 bytes a
       second;
     a 440 Hz tone, a sample, one-shot, 2 seconds: it has to end by itself
       after 2 s;
     a short 880 Hz blip, a sample at a 24 kHz rate, fading out;
     the channels: 48 quiet looping 660 Hz tones (every voice), then a
       louder 990 Hz one of a higher priority takes the quietest's voice,
       for half a second;
     the reverb (hall) on the blip, dry and then wet.
   Before that the encoder is checked on the EE: a sine and a sweep, encoded
   and decoded again (as the SPU does), have to keep a signal to noise ratio
   of 25 dB (300), all filters better than filter 0 alone. */

#include "t.h"
#include <mx/snd.h>

#define LONG_N (2 * 48000)
#define BLIP_N (24000 / 5)

static s16 pcm[LONG_N];
static u8 adpcm[MX_SND_ADPCM_SIZE(LONG_N)];
static s16 dec[LONG_N + 28];
/* amp * sin(2 pi freq t / rate) from i = 0, by a rotating vector */
static void sine(s16 *x, u32 n, float freq, float rate, float amp) {
  float w = 6.2831853f * freq / rate, w2 = w * w;
  float c = 1 - w2 / 2 + w2 * w2 / 24, s = w - w * w2 / 6 + w * w2 * w2 / 120;
  float re = 1, im = 0;
  for (u32 i = 0; i < n; i++) {
    x[i] = (s16)(im * amp);
    float nr = re * c - im * s, ni = re * s + im * c;
    re = nr, im = ni;
    if ((i & 1023) == 1023) {
      float m = 1 / __builtin_sqrtf(re * re + im * im);
      re *= m, im *= m;
    }
  }
}

/* the SPU's decoding (docs: mx_snd_encode) */
static void decode(const u8 *b, u32 blocks, s16 *out) {
  static const int f[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
  int s1 = 0, s2 = 0;
  for (u32 k = 0; k < blocks; k++, b += 16) {
    int sh = b[0] & 15, fi = b[0] >> 4;
    for (int i = 0; i < 28; i++) {
      int nib = (b[2 + i / 2] >> (i & 1 ? 4 : 0)) & 15;
      int d = (s16)(nib << 12) >> sh;
      d += (s1 * f[fi][0] + s2 * f[fi][1] + 32) >> 6;
      d = d < -32768 ? -32768 : d > 32767 ? 32767 : d;
      *out++ = (s16)d;
      s2 = s1;
      s1 = d;
    }
  }
}

static u32 snr(const s16 *x, u32 n) {
  u32 blocks = mx_snd_encode(x, n, adpcm, 0) / 16;
  decode(adpcm, blocks, dec);
  u64 sig = 0, err = 0;
  for (u32 i = 0; i < n; i++) {
    sig += (u64)(x[i] * x[i]);
    err += (u64)((x[i] - dec[i]) * (x[i] - dec[i]));
  }
  return err ? (u32)(sig / err) : ~0u;
}

static u32 filters_used(u32 n) {
  u32 used = 0;
  for (u32 k = 0; k < MX_SND_ADPCM_SIZE(n) / 16; k++)
    used |= 1u << (adpcm[k * 16] >> 4);
  return used;
}

/* the zero crossings and the peak of the left side of a core's voice mix
   (the last 512 samples, 10.7 ms) */
static void mix(int core, u32 *crossings, int *peak) {
  extern int mx__snd_peek(u32 spu, void *dst, u32 bytes);
  static s16 m[512];
  *crossings = 0, *peak = 0;
  if (mx__snd_peek(core ? 0x3000 : 0x2000, m, sizeof m) < 0)
    return;
  for (int i = 0; i < 512; i++) {
    int a = m[i] < 0 ? -m[i] : m[i];
    *peak = a > *peak ? a : *peak;
    if (i && (m[i - 1] < 0) != (m[i] < 0))
      (*crossings)++;
  }
}

/* the peak of 4 KB of core c's reverb work area (hall: the 0xADE0 bytes below
   its end, 0x1DFFFF for core 0, 0x1FFFFF for core 1) */
static int work(int c) {
  extern int mx__snd_peek(u32 spu, void *dst, u32 bytes);
  static s16 m[2048];
  int peak = 0;
  if (mx__snd_peek(c ? 0x1FA000 : 0x1DA000, m, sizeof m) < 0)
    return -1;
  for (int i = 0; i < 2048; i++)
    peak = (m[i] < 0 ? -m[i] : m[i]) > peak ? (m[i] < 0 ? -m[i] : m[i]) : peak;
  return peak;
}

static void wait_ms(u32 ms) {
  u32 t0 = mx_count();
  while (mx_count() - t0 < ms * (MX_EE_HZ / 1000))
    ;
}

int main(int argc, char **argv) {
  int seconds = 3;
  if (argc > 1) {
    seconds = 0;
    for (const char *c = argv[1]; *c >= '0' && *c <= '9'; c++)
      seconds = seconds * 10 + *c - '0';
  }

  /* the encoder */
  sine(pcm, 4800, 440, 48000, 12000);
  u32 r1 = snr(pcm, 4800);
  u32 u1 = filters_used(4800);
  float ph = 0;
  for (u32 i = 0; i < 4800; i++) { /* a sweep: 100 Hz .. 4 kHz */
    float f = 100 + 3900.0f * i / 4800;
    ph += 6.2831853f * f / 48000;
    if (ph > 3.14159265f)
      ph -= 6.2831853f;
    float p2 = ph * ph; /* sin by its series, |ph| <= pi */
    float sn = ph * (1 - p2 / 6 * (1 - p2 / 20 * (1 - p2 / 42 * (1 - p2 / 72 * (1 - p2 / 110)))));
    pcm[i] = (s16)(sn * 12000);
  }
  u32 r2 = snr(pcm, 4800);
  mx_printf("snd: encoder SNR sine %u (filters %02x), sweep %u (filters %02x)\n", r1, u1, r2,
            filters_used(4800));
  CHECK(r1 > 300 && r2 > 300);
  CHECK(u1 & ~1u);

  u32 t0 = mx_count();
  CHECK(mx_snd_init() == 0);
  mx_printf("snd: init %u ms\n", (mx_count() - t0) / (MX_EE_HZ / 1000));
  if (t_fail)
    return t_done("sndapi");

  /* the stream: a period of the tone, 128 frames, queued as there is room */
  static s16 period[128 * 2];
  sine(pcm, 128, 375, 48000, 10000);
  for (int i = 0; i < 128; i++)
    period[2 * i] = period[2 * i + 1] = pcm[i];
  CHECK(mx_snd_stream_format(48000, 16, 2) == 0);
  u32 room = mx_snd_stream_room();
  mx_printf("snd: stream room %u bytes\n", room);
  u32 total = 0, pos = 0, calls = 0;
  t0 = mx_count();
  while (mx_count() - t0 < (u32)seconds * MX_EE_HZ) {
    room = mx_snd_stream_room() & ~3u;
    while (room) {
      u32 n = sizeof period - pos < room ? sizeof period - pos : room;
      u32 took = mx_snd_stream_queue((const u8 *)period + pos, n);
      total += took;
      pos = (pos + took) % sizeof period;
      room -= took;
      calls++;
      if (took < n)
        break;
    }
    wait_ms(10);
  }
  u32 ms = (mx_count() - t0) / (MX_EE_HZ / 1000);
  u32 queued = mx_snd_stream_queued();
  u32 rate = (u32)((u64)(total - queued) * 1000 / ms);
  mx_printf("snd: stream took %u bytes in %u ms (%u calls), %u queued: %u bytes/s (192000)\n",
            total, ms, calls, queued, rate);
  CHECK(rate > 182000 && rate < 202000);
  /* fed every 10 ms with 100 ms queued at most: never short */
  extern void mx__snd_counts(u32 *frames, u32 *starved);
  u32 frames, starved;
  mx__snd_counts(&frames, &starved);
  mx_printf("snd: mxsnd played %u frames, the stream starved %u\n", frames, starved);
  CHECK_EQ(starved, 0);
  mx_snd_stream_stop();
  wait_ms(300);

  /* 2 s one-shot sample */
  static struct mx_snd_sample lng, blip;
  sine(pcm, LONG_N, 440, 48000, 10000);
  u32 size = mx_snd_encode(pcm, LONG_N, adpcm, 0);
  t0 = mx_count();
  CHECK(mx_snd_load(&lng, adpcm, size, 48000) == 0);
  mx_printf("snd: sample %u bytes loaded in %u ms\n", size, (mx_count() - t0) / (MX_EE_HZ / 1000));
  t0 = mx_count();
  int v = mx_snd_play(&lng, -1);
  CHECK(v >= 0);
  /* what the voice gives: 440 Hz, 9.4 crossings in 512 samples */
  wait_ms(100);
  u32 cr;
  int pk;
  mix(v / 24, &cr, &pk);
  mx_printf("snd: voice %d mix: %u zero crossings in 512 samples (440 Hz: 9), peak %d\n", v, cr, pk);
  CHECK(cr >= 7 && cr <= 12 && pk > 2000);
  while (mx_snd_playing(v, &lng) && mx_count() - t0 < 4 * MX_EE_HZ)
    wait_ms(10);
  ms = (mx_count() - t0) / (MX_EE_HZ / 1000);
  mx_printf("snd: sample played %u ms (2000) on voice %d\n", ms, v);
  CHECK(ms > 1900 && ms < 2200);

  /* blip at 24 kHz */
  sine(pcm, BLIP_N, 880, 24000, 10000);
  for (u32 i = 0; i < BLIP_N; i++) /* fade out */
    pcm[i] = (s16)(pcm[i] * (int)(BLIP_N - i) / (int)BLIP_N);
  size = mx_snd_encode(pcm, BLIP_N, adpcm, 0);
  CHECK(mx_snd_load(&blip, adpcm, size, 24000) == 0);
  t0 = mx_count();
  v = mx_snd_play(&blip, -1);
  CHECK(v >= 0);
  while (mx_snd_playing(v, &blip) && mx_count() - t0 < 2 * MX_EE_HZ)
    wait_ms(5);
  ms = (mx_count() - t0) / (MX_EE_HZ / 1000);
  mx_printf("snd: blip played %u ms (200) on voice %d\n", ms, v);
  CHECK(ms > 150 && ms < 300);

  /* channels: every voice a quiet loop at priority 1 */
  static struct mx_snd_sample tone, hi;
  sine(pcm, 2240, 660, 48000, 10000); /* 30.8 periods in 2240: close enough for a loop */
  size = mx_snd_encode(pcm, 2240, adpcm, 1);
  CHECK(mx_snd_load(&tone, adpcm, size, 48000) == 0 && tone.loop);
  sine(pcm, 2240, 990, 48000, 10000);
  size = mx_snd_encode(pcm, 2240, adpcm, 1);
  CHECK(mx_snd_load(&hi, adpcm, size, 48000) == 0);
  static int ch[MX_SND_VOICES];
  int got = 0;
  for (int i = 0; i < MX_SND_VOICES; i++)
    got += (ch[i] = mx_snd_ch_play(&tone, 48000, 100 + i, 100 + i, 1)) > 0;
  mx_snd_commit();
  CHECK_EQ(got, MX_SND_VOICES);
  /* lower priority: not played; higher: the quietest (ch[0]) gives its voice */
  CHECK_EQ(mx_snd_ch_play(&tone, 48000, 5000, 5000, 0), 0);
  int h = mx_snd_ch_play(&hi, 48000, 4000, 4000, 2);
  mx_snd_commit();
  CHECK(h > 0);
  CHECK(!mx_snd_ch_playing(ch[0]) && mx_snd_ch_set(ch[0], 48000, 0, 0) < 0);
  CHECK(mx_snd_ch_playing(ch[1]) && mx_snd_ch_playing(h));
  wait_ms(250);
  /* the loud channel's 990 Hz over the quiet 660 Hz ones: 21 crossings */
  mix(((h & 0xFF) - 1) / 24, &cr, &pk);
  mx_printf("snd: channel mix: %u zero crossings in 512 samples (990 Hz: 21), peak %d\n", cr, pk);
  CHECK(cr >= 18 && cr <= 25 && pk > 1000);
  CHECK(mx_snd_ch_set(h, 44100, 6000, 2000) == 0); /* a little lower, to the left */
  mx_snd_commit();
  wait_ms(250);
  for (int i = 1; i < MX_SND_VOICES; i++)
    mx_snd_ch_stop(ch[i]);
  mx_snd_ch_stop(h);
  mx_snd_commit();
  CHECK(!mx_snd_ch_playing(h));
  wait_ms(50);

  /* reverb (hall): its work area, cleared when it is set, has the blip in it
     after a wet one, stays silent after a dry one */
  mx_snd_reverb(MX_SND_REVERB_HALL, 0x3000, 0, 0, 0);
  wait_ms(50);
  int dry = work(0) + work(1);
  h = mx_snd_ch_play(&blip, 24000, MX_SND_VOL_MAX, MX_SND_VOL_MAX, 1);
  mx_snd_ch_reverb(h, 0);
  mx_snd_commit();
  wait_ms(300);
  int after_dry = work(0) + work(1);
  h = mx_snd_ch_play(&blip, 24000, MX_SND_VOL_MAX, MX_SND_VOL_MAX, 1);
  mx_snd_commit();
  wait_ms(300);
  int after_wet = work(0) + work(1);
  mx_printf("snd: reverb work area peak: set %d, after a dry blip %d, after a wet one %d\n", dry,
            after_dry, after_wet);
  CHECK(dry < 100 && after_dry < 100 && after_wet > 1000);
  mx_snd_reverb(MX_SND_REVERB_OFF, 0, 0, 0, 0);
  mx_snd_commit();

  CHECK(mx_snd_free(&lng) == 0 && mx_snd_free(&blip) == 0);
  CHECK(mx_snd_free(&tone) == 0 && mx_snd_free(&hi) == 0);
  extern volatile u32 mx__snd_dropped;
  CHECK_EQ(mx__snd_dropped, 0);
  return t_done("sndapi");
}
