/* iop/sound/mxsnd/src/midi_fix.h against the same in floating point: the
   pitch within a cent (or the register's step), the volumes within a step
   and half a cent (0.03 %: the attenuation is rounded to whole cents). */

#include <stdlib.h>
#include "test.h"
#include "../../iop/sound/mxsnd/src/midi_fix.h"

/* 2^x without libm */
static double exp2d(double x) {
  int n = (int)x;
  if (n > x)
    n--;
  double t = (x - n) * 0.69314718055994531, term = 1, sum = 1;
  for (int i = 1; i < 20; i++)
    sum += term *= t / i;
  for (; n > 0; n--)
    sum *= 2;
  for (; n < 0; n++)
    sum /= 2;
  return sum;
}
static double absd(double x) { return x < 0 ? -x : x; }
static double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

static double volume_worst, pitch_worst;

static void pitch_one(unsigned rate, int key, int root, int scale, int tune, int bend, int range) {
  double cents = (key - root) * scale + tune + (double)bend * range / 8192;
  double want = rate * exp2d(cents / 1200) * 4096 / 48000;
  double got = midi_pitch(midi_base(rate), midi_cents(key, root, scale, tune, bend, range));
  if (want > MIDI_PITCH_MAX)
    want = MIDI_PITCH_MAX;
  /* a cent is 0.058 %; the register's rounding half a step */
  double err = absd(got - want) - 0.5;
  if (err > 0 && want > 0 && err / want > pitch_worst)
    pitch_worst = err / want;
}

static void volume_one(int vel, int atten, int vol, int expr, int master, int zpan, int cpan) {
  double v = vel / 127.0, c = vol / 127.0, e = expr / 127.0;
  double g = 0.6 * v * v * exp2d(-atten * 0.4 / 200 * 3.321928094887) * c * c * e * e * master / 256 * MIDI_VOL_MAX;
  double x = clampd(zpan / 1000.0 + (cpan - 64) / 128.0, -0.5, 0.5);
  double wl = g * (x > 0 ? 1 - 2 * x : 1), wr = g * (x < 0 ? 1 + 2 * x : 1);
  unsigned l, r;
  midi_volumes(midi_note_gain(vel, atten), vol, expr, master, zpan, cpan, &l, &r);
  double dl = absd(l - wl) - 0.0003 * wl, dr = absd(r - wr) - 0.0003 * wr, d = dl > dr ? dl : dr;
  if (d > volume_worst)
    volume_worst = d;
}

static int in(int lo, int hi) { return lo + rand() % (hi - lo + 1); }

void test_midi(void) {
  /* known: A 440 at its root, an octave up, two down, a bend of two semitones */
  CHECK_EQ(midi_pitch(midi_base(48000), 0), 4096);
  CHECK_EQ(midi_pitch(midi_base(48000), 1200), 8192);
  CHECK_EQ(midi_pitch(midi_base(48000), -2400), 1024);
  CHECK_EQ(midi_pitch(midi_base(48000), 200), 4598);
  CHECK_EQ(midi_pitch(midi_base(48000), 12000), MIDI_PITCH_MAX);
  CHECK_EQ(midi_pitch(midi_base(48000), -120000), 0);
  CHECK_EQ(midi_note_gain(0, 0), 0);
  CHECK_EQ(midi_note_gain(127, 65535), 0);
  srand(1);
  for (int i = 0; i < 1000000; i++) {
    pitch_one((unsigned)in(4000, 192000), in(0, 127), in(0, 127), in(0, 3) ? 100 : in(0, 200), in(-1200, 1200),
              in(0, 1) ? 0 : in(-8192, 8191), in(0, 3) ? 200 : in(0, 2400));
    volume_one(in(0, 127), in(0, 1) ? in(0, 200) : in(0, 1440), in(0, 127), in(0, 127), in(0, 256), in(-500, 500),
               in(0, 127));
  }
  printf("midi: pitch at most %.4f cents off beside rounding, volume %.2f beside 0.03 %%\n", pitch_worst * 1731.2, volume_worst);
  CHECK_EQ(pitch_worst < 0.00058, 1); /* a cent */
  CHECK_EQ(volume_worst <= 1, 1);
}
