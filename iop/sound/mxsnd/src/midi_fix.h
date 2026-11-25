/* The MIDI synthesiser's numbers in fixed point (the IOP has no FPU): the
   voice's pitch and volumes of a note. No division but by constants, except
   midi_base's, once a sample. Checked on the host (tests/host/test_midi.c). */
#ifndef MIDI_FIX_H
#define MIDI_FIX_H

#define MIDI_VOL_MAX 0x3FFF
#define MIDI_PITCH_MAX 0x3FFF
#define MIDI_GAIN 2576980378u /* 0.6: the full mix of a few loud notes within the SPU's range */

/* 2^(n/12) and 2^(n/1200), Q16 */
static const unsigned int midi_semi[12] = {
  65536, 69433, 73562, 77936, 82570, 87480,
  92682, 98193, 104032, 110218, 116772, 123715,
};
static const unsigned int midi_cent[100] = {
  65536, 65574, 65612, 65650, 65688, 65726, 65764, 65802, 65840, 65878,
  65916, 65954, 65992, 66030, 66068, 66106, 66144, 66183, 66221, 66259,
  66297, 66336, 66374, 66412, 66451, 66489, 66528, 66566, 66605, 66643,
  66682, 66720, 66759, 66797, 66836, 66874, 66913, 66952, 66990, 67029,
  67068, 67107, 67145, 67184, 67223, 67262, 67301, 67340, 67378, 67417,
  67456, 67495, 67534, 67573, 67612, 67651, 67691, 67730, 67769, 67808,
  67847, 67886, 67926, 67965, 68004, 68043, 68083, 68122, 68161, 68201,
  68240, 68280, 68319, 68359, 68398, 68438, 68477, 68517, 68556, 68596,
  68635, 68675, 68715, 68755, 68794, 68834, 68874, 68914, 68953, 68993,
  69033, 69073, 69113, 69153, 69193, 69233, 69273, 69313, 69353, 69393,
};

/* a * b / 2^32 */
static unsigned int midi_mulhi(unsigned int a, unsigned int b) {
  return (unsigned int)(((unsigned long long)a * b) >> 32);
}
/* 2^(cents/1200): the fraction in Q16 (65536 ..), the octave apart */
static unsigned int midi_exp2(int cents, int *oct) {
  int o = cents / 1200, r = cents % 1200;
  if (r < 0)
    r += 1200, o--;
  *oct = o;
  return (unsigned int)(((unsigned long long)midi_semi[r / 100] * midi_cent[r % 100]) >> 16);
}

/* a sample's rate (at most 192000 Hz) as a pitch in Q8 (4096: 48 kHz) */
static unsigned int midi_base(unsigned int rate) { return (rate * 8192u + 187u) / 375u; }
/* base at cents from the sample's own key: the SPU2's pitch */
static unsigned int midi_pitch(unsigned int base, int cents) {
  int o;
  unsigned long long p = (unsigned long long)base * midi_exp2(cents, &o); /* Q24 */
  int sh = 24 - o;
  if (sh > 63)
    return 0;
  if (sh < 1)
    return MIDI_PITCH_MAX;
  p = (p + (1ull << (sh - 1))) >> sh;
  return p > MIDI_PITCH_MAX ? MIDI_PITCH_MAX : (unsigned int)p;
}
/* the note's cents: its key, its zone's root, scale and tune, the bend
   (-8192 .. 8191) of range cents */
static int midi_cents(int key, int root, int scale, int tune, int bend, int range) {
  return (key - root) * scale + tune + ((bend * range + 4096) >> 13);
}

/* (v / 127)^2, Q32 */
static unsigned int midi_sq(int v) { return (unsigned int)(v * v) * 266288u; }
/* a note's gain, Q32: its velocity, its zone's attenuation (centibels, 0.4 of
   it counts, as in the other synthesisers) */
static unsigned int midi_note_gain(int vel, int atten) {
  int o;
  unsigned int f = midi_exp2(-(int)(((unsigned int)atten * 8164u + 512u) >> 10), &o); /* 7.9726 cents a centibel */
  unsigned int g = midi_mulhi(midi_sq(vel), MIDI_GAIN);
  if (o >= 0)
    return g;
  if (o < -31)
    return 0;
  return midi_mulhi(g, f << 15) >> (-o - 1);
}
/* its left and right volume: the channel's volume, expression and pan (0 ..
   127), the zone's pan (-500 .. 500), the synthesiser's volume (0 .. 256) */
static void midi_volumes(unsigned int gain, int vol, int expr, int master, int zpan, int cpan, unsigned int *l,
                         unsigned int *r) {
  unsigned int g = midi_mulhi(midi_mulhi(gain, midi_sq(vol)), midi_sq(expr));
  g = midi_mulhi(g, (unsigned int)(MIDI_VOL_MAX * master) << 8) >> 12; /* Q4 */
  int x = zpan * 16 + (cpan - 64) * 125; /* 8000: all to the right */
  x = x < -8000 ? -8000 : x > 8000 ? 8000 : x;
  *l = ((x > 0 ? g * (unsigned int)(8000 - x) / 8000u : g) + 8) >> 4;
  *r = ((x < 0 ? g * (unsigned int)(8000 + x) / 8000u : g) + 8) >> 4;
}
#endif
