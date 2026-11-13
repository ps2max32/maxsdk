/* MPEG audio layer II (<mx/mp2.h>) against ffmpeg's decoding of the same
   frames (tools/mpeg-fixtures.sh): its filter works in floating point, this
   one in integers, so the samples differ by a few of 32768. */

#include <mx/mp2.h>

#include "t.h"

static const u8 stereo[] = {
#embed "data/mp2_stereo.mp2"
};
static const u8 stereo_pcm[] = {
#embed "data/mp2_stereo.pcm"
};
static const u8 mono[] = {
#embed "data/mp2_mono.mp2"
};
static const u8 mono_pcm[] = {
#embed "data/mp2_mono.pcm"
};

static struct mx_mp2 dec;
static s16 out[MX_MP2_SAMPLES * 2];

/* channels: of the reference (the decoder gives a mono frame twice) */
static void stream(const char *name, const u8 *file, u32 size, const u8 *pcm, u32 pcm_size, u32 want_rate,
                   u32 channels) {
  mx_mp2_init(&dec);
  u32 pos = 0, frames = 0, worst = 0, cycles = 0;
  u64 sum = 0, n = 0;
  while (pos < size) {
    u32 got, rate = 0, t0 = mx_count();
    u32 used = mx_mp2_decode(&dec, file + pos, size - pos, out, &got, &rate);
    cycles += mx_count() - t0;
    if (!used)
      break;
    pos += used;
    if (!got)
      continue;
    CHECK_EQ(got, MX_MP2_SAMPLES);
    CHECK_EQ(rate, want_rate);
    for (u32 i = 0; i < MX_MP2_SAMPLES; i++)
      for (u32 c = 0; c < channels; c++) {
        u32 at = ((frames * MX_MP2_SAMPLES + i) * channels + c) * 2;
        if (at + 2 > pcm_size)
          continue;
        int want = (s16)(pcm[at] | pcm[at + 1] << 8), d = out[i * 2 + c] - want;
        d = d < 0 ? -d : d;
        worst = (u32)d > worst ? (u32)d : worst;
        sum += (u32)d, n++;
      }
    if (channels == 1)
      CHECK_EQ(out[101], out[100]);
    frames++;
  }
  u32 mean = n ? (u32)(sum * 100 / n) : 0;
  mx_printf("mp2: %s: %u frames, largest difference to ffmpeg's %u of 32768, mean %u.%02u; %u us a frame\n",
            name, frames, worst, mean / 100, mean % 100,
            frames ? (u32)((u64)cycles * 1000000 / 294912000 / frames) : 0);
  CHECK(frames >= 8);
  CHECK(n * 2 >= pcm_size - 2 * channels * MX_MP2_SAMPLES);
  CHECK(worst <= 12);
  CHECK(mean < 200);
}

int main(void) {
  stream("48 kHz stereo, 192 kbit/s", stereo, sizeof stereo, stereo_pcm, sizeof stereo_pcm, 48000, 2);
  stream("44.1 kHz mono, 96 kbit/s", mono, sizeof mono, mono_pcm, sizeof mono_pcm, 44100, 1);
  /* bytes that are no frame are passed over; a frame cut off waits */
  u32 got, rate;
  CHECK_EQ(mx_mp2_decode(&dec, stereo_pcm, 64, out, &got, &rate) > 0, 1);
  CHECK_EQ(got, 0);
  CHECK_EQ(mx_mp2_decode(&dec, stereo, 100, out, &got, &rate), 0);
  return t_done("mp2");
}
