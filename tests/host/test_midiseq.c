/* iop/sound/mxsnd/src/midi.c on a made bank and songs: when notes start, loops,
   EMIDI's tracks, the voices and what a bad file does. */

#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "../../iop/sound/mxsnd/src/midi.h"

static int step_no, ons, offs, on_step[512], on_voice[512];
static unsigned on_pitch[512];
void midi_voice_on(int v, unsigned spu, unsigned pitch, unsigned l, unsigned r, unsigned a1, unsigned a2) {
  (void)spu, (void)l, (void)r, (void)a1, (void)a2;
  if (ons < 512)
    on_step[ons] = step_no, on_voice[ons] = v, on_pitch[ons] = pitch;
  ons++;
}
static unsigned set_pitch, set_l;
static int sets;
void midi_voice_set(int v, unsigned pitch, unsigned l, unsigned r) { (void)v, (void)r, sets++, set_pitch = pitch, set_l = l; }
static int off_step[512];
void midi_voice_off(int v) {
  (void)v;
  if (offs < 512)
    off_step[offs] = step_no;
  offs++;
}
int midi_voice_ended(int v) { return (void)v, 0; }

static struct {
  struct midi_bank_head h;
  struct midi_bank_sample s[1];
  struct midi_bank_preset p[2];
  struct midi_bank_zone z[2];
} bank;

static unsigned char song[8192];
static unsigned len;
static void put(const void *d, unsigned n) { memcpy(song + len, d, n), len += n; }
#define PUT(...)                                                                                                       \
  do {                                                                                                                 \
    const unsigned char d_[] = {__VA_ARGS__};                                                                          \
    put(d_, sizeof d_);                                                                                                \
  } while (0)
static unsigned tstart;
/* 120 a minute, division 48: a tick is 10.4167 ms */
static void begin(int tracks) {
  len = 0;
  PUT('M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, (unsigned char)tracks, 0, 48);
}
static void tbegin(void) {
  PUT('M', 'T', 'r', 'k', 0, 0, 0, 0);
  tstart = len;
}
static void tend(void) {
  PUT(0, 0xFF, 0x2F, 0);
  unsigned n = len - tstart;
  song[tstart - 2] = (unsigned char)(n >> 8), song[tstart - 1] = (unsigned char)n;
}
static void run(int steps) {
  for (int i = 0; i < steps; i++, step_no++)
    midi_step();
}
static void start(int loop) {
  CHECK_EQ(midi_song(song, len), 0);
  midi_play(loop);
  step_no = ons = offs = sets = 0;
}

void test_midiseq(void) {
  bank.h = (struct midi_bank_head){MIDI_BANK_MAGIC, 1, 1, 2, 2, 160, sizeof bank, 0};
  bank.s[0] = (struct midi_bank_sample){0, 160, 48000, 1};
  bank.p[0] = (struct midi_bank_preset){0, 0, 0, 1};
  bank.p[1] = (struct midi_bank_preset){128, 0, 1, 1};
  bank.z[0] = (struct midi_bank_zone){0, 127, 0, 127, 0, 69, 0, 0, 100, 0, 0, 0xF, 0x1FC0};
  bank.z[1] = bank.z[0];
  bank.z[1].excl = 1;
  bank.h.magic++;
  CHECK_EQ(midi_bank(&bank, sizeof bank, 0x5040) < 0, 1);
  bank.h.magic--;
  bank.z[0].sample = 1;
  CHECK_EQ(midi_bank(&bank, sizeof bank, 0x5040) < 0, 1);
  bank.z[0].sample = 0;
  CHECK_EQ(midi_bank(&bank, sizeof bank - 1, 0x5040) < 0, 1);
  CHECK_EQ(midi_bank(&bank, sizeof bank, 0x5040), 0);

  /* a note at 0, off after 48 ticks (500 ms), another there; the end stops */
  begin(1), tbegin();
  PUT(0, 0x90, 69, 100, 48, 69, 0, 0, 81, 100, 48, 81, 0);
  tend(), start(0);
  run(200);
  CHECK_EQ(ons, 2);
  CHECK_EQ(on_step[0], 0); /* a tick is shorter than a step */
  CHECK_EQ(on_pitch[0], 4096);
  CHECK_EQ(on_pitch[1], 8192);
  CHECK_EQ(on_step[1] >= 46 && on_step[1] <= 48, 1); /* 500 ms / 10.67: 46.9 */
  CHECK_EQ(midi_playing(), 0);
  CHECK_EQ(offs, 2);

  /* looping: the same again and again, 96 ticks (1 s) each; 10 s */
  start(1);
  run(938);
  CHECK_EQ(ons >= 19 && ons <= 21, 1);
  CHECK_EQ(midi_playing(), 1);

  /* a pause: quiet, and it goes on where it was */
  int before = ons, quiet_offs = offs;
  midi_pause(1);
  run(500);
  CHECK_EQ(ons, before);
  CHECK_EQ(offs >= quiet_offs, 1);
  midi_pause(0);
  run(200);
  CHECK_EQ(ons > before, 1);
  midi_stop();
  CHECK_EQ(midi_playing(), 0);

  /* on and off in one tick: the off comes a step later */
  begin(1), tbegin();
  PUT(0, 0x90, 69, 100, 0, 69, 0, 48, 69, 0);
  tend(), start(0);
  run(20);
  CHECK_EQ(ons, 1);
  CHECK_EQ(offs, 1);
  CHECK_EQ(off_step[0], on_step[0] + 1);

  /* EMIDI: a track for another card is left out, its loop plays 3 times */
  begin(2), tbegin();
  PUT(0, 0xB0, 110, 4, 0, 0x90, 60, 100, 1, 60, 0);
  tend(), tbegin();
  PUT(0, 0xB0, 110, 0, 0, 116, 2, 0, 0x90, 69, 100, 4, 69, 0, 0, 0xB0, 117, 127);
  tend(), start(0);
  run(100);
  CHECK_EQ(ons, 3);
  CHECK_EQ(on_pitch[0], 4096);
  CHECK_EQ(midi_playing(), 0);
  /* a loop without time in it does not hang: the song is stopped */
  begin(1), tbegin();
  PUT(0, 0xB0, 116, 0, 0, 0xB0, 117, 127);
  tend(), start(1);
  run(10);
  CHECK_EQ(midi_playing(), 0);

  /* (the bank again: no voice of the songs before is left) */
  CHECK_EQ(midi_bank(&bank, sizeof bank, 0x5040), 0);
  /* the bend and the volume reach a note that sounds */
  begin(1), tbegin();
  PUT(0, 0x90, 69, 127, 1, 0xE0, 0x7F, 0x7F, 1, 0xB0, 7, 0);
  PUT(100, 0x80, 69, 0);
  tend(), start(0);
  run(2);
  run(1);
  run(1);
  CHECK_EQ(set_pitch >= 4596 && set_pitch <= 4599, 1);
  CHECK_EQ(set_l, 0);
  midi_stop();

  /* 40 notes at once: 24 voices, each taken; drums' exclusive class */
  begin(1), tbegin();
  for (int i = 0; i < 40; i++)
    PUT(0, 0x90, (unsigned char)(40 + i), 100);
  PUT(10, 0x99, 42, 100, 10, 46, 100);
  tend(), start(0);
  run(3);
  CHECK_EQ(ons, 40);
  unsigned used = 0;
  for (int i = 0; i < 40; i++)
    used |= 1u << on_voice[i];
  CHECK_EQ(used, 0xFFFFFF);
  int was = offs;
  run(30);
  CHECK_EQ(offs > was, 1);
  midi_stop();

  /* broken files: refused, or played without a read outside (ASan) */
  CHECK_EQ(midi_song("MThd", 4) < 0, 1);
  begin(1), tbegin();
  PUT(0, 0x90, 69, 100, 48, 69, 0, 0, 0xFF, 0x51, 3, 0, 0, 1);
  tend();
  srand(2);
  for (int i = 0; i < 20000; i++) {
    unsigned n = (unsigned)rand() % (len + 1);
    unsigned char *c = malloc(n ? n : 1);
    memcpy(c, song, n);
    for (int k = rand() % 4; k > 0 && n; k--)
      c[(unsigned)rand() % n] = (unsigned char)rand();
    if (midi_song(c, n) == 0) {
      midi_play(rand() & 1);
      run(20);
    }
    midi_song(0, 0);
    free(c);
  }
  midi_bank(0, 0, 0);
}
