/* A bank made here (a looping sine at A 440 for every program, a one-shot noise
   burst as the drum kit) and songs, for midi.c and k90_music.c. */
#include "t.h"
#include <mx/midi.h>
#include <mx/snd.h>
#include <mx/string.h>
#include <mx/thread.h>
#include "../../iop/sound/mxsnd/src/midi.h"

#define SINE_N 336 /* 12 blocks, 3 periods: 428.57 Hz at 48 kHz */
#define NOISE_N (28 * 200)

static struct {
  struct midi_bank_head h;
  struct midi_bank_sample s[2];
  struct midi_bank_preset p[2];
  struct midi_bank_zone z[2];
  u8 adpcm[MX_SND_ADPCM_SIZE(SINE_N) + MX_SND_ADPCM_SIZE(NOISE_N)] __attribute__((aligned(16)));
} bank __attribute__((aligned(64)));
static s16 pcm[NOISE_N];

static void ms(u32 n) { mx_sleep_us(n * 1000); }

/* the zero crossings and peak of the left side of both cores' voice mixes
   (the last 512 samples, 10.7 ms): the core with the louder one */
static void mix(u32 *crossings, int *peak) {
  extern int mx__snd_peek(u32 spu, void *dst, u32 bytes);
  static s16 m[512];
  *crossings = 0, *peak = 0;
  for (int core = 0; core < 2; core++) {
    u32 c = 0;
    int p = 0;
    if (mx__snd_peek(core ? 0x3000 : 0x2000, m, sizeof m) < 0)
      continue;
    for (int i = 0; i < 512; i++) {
      int a = m[i] < 0 ? -m[i] : m[i];
      p = a > p ? a : p;
      if (i && (m[i - 1] < 0) != (m[i] < 0))
        c++;
    }
    if (p > *peak)
      *crossings = c, *peak = p;
  }
}

__attribute__((unused)) static int sounds(const char *what, u32 lo, u32 hi) {
  /* the middle one of five: a read may meet the half the SPU writes, a jump in
     the wave that adds or takes a crossing */
  u32 c[5], m;
  int p = 0;
  for (int i = 0; i < 5; i++) {
    mix(&c[i], &p);
    for (int k = i; k > 0 && c[k] < c[k - 1]; k--)
      m = c[k], c[k] = c[k - 1], c[k - 1] = m;
    ms(3);
  }
  m = c[2];
  mx_printf("midi: %s: %u zero crossings in 512 samples (%u .. %u), peak %d\n", what, m, lo, hi, p);
  return m >= lo && m <= hi && p > 1000;
}

static int peak(void) {
  u32 c;
  int p;
  mix(&c, &p);
  return p;
}

static void make_bank(void) {
  /* the sine: 3 periods, looping */
  for (int i = 0; i < SINE_N; i++) {
    float ph = 6.2831853f * 3 * (float)i / SINE_N, x = ph;
    while (x > 3.14159265f)
      x -= 6.2831853f;
    float x2 = x * x;
    pcm[i] = (s16)(12000 * x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110))))));
  }
  u32 a = mx_snd_encode(pcm, SINE_N, bank.adpcm, 1);
  u32 seed = 1;
  for (int i = 0; i < NOISE_N; i++) {
    seed = seed * 1103515245 + 12345;
    pcm[i] = (s16)((int)(seed >> 16 & 0x7FFF) - 0x4000);
  }
  u32 b = mx_snd_encode(pcm, NOISE_N, bank.adpcm + a, 0);
  bank.h = (struct midi_bank_head){MIDI_BANK_MAGIC, 1, 2, 2, 2, a + b, (u32)((u8 *)bank.adpcm - (u8 *)&bank), 0};
  bank.s[0] = (struct midi_bank_sample){0, a, 48000, 1};
  bank.s[1] = (struct midi_bank_sample){a, b, 48000, 0};
  bank.p[0] = (struct midi_bank_preset){0, 0, 0, 1};
  bank.p[1] = (struct midi_bank_preset){128, 0, 1, 1};
  /* A 440 at key 69: 428.57 Hz + 46 cents; attack at once, full sustain, a
     release of about 0.2 s (exp, 20 dB in 55 ms: rate 11) */
  bank.z[0] = (struct midi_bank_zone){0, 127, 0, 127, 0, 69, 0, 46, 100, 0, 0, 0x000F, 0x1FC0 | 1 << 5 | 11};
  bank.z[1] = (struct midi_bank_zone){35, 81, 0, 127, 1, 38, 0, 0, 100, 0, 0, 0x000F, 0x1FC0 | 1 << 5 | 11};
}


/* ---- songs ---- division 48 at 120 a minute: 96 ticks a second */
static u8 song[512];
static u32 len, tstart;
static void put(const void *d, u32 n) { memcpy(song + len, d, n), len += n; }
#define PUT(...)                                                                           \
  do {                                                                                     \
    const u8 d_[] = {__VA_ARGS__};                                                         \
    put(d_, sizeof d_);                                                                    \
  } while (0)
static void begin(void) {
  len = 0;
  PUT('M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 48, 'M', 'T', 'r', 'k', 0, 0, 0, 0);
  tstart = len;
}
static void end(void) {
  PUT(0, 0xFF, 0x2F, 0);
  song[tstart - 2] = (u8)((len - tstart) >> 8), song[tstart - 1] = (u8)(len - tstart);
}
