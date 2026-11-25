/* mx/midi.h: songs handed over to mxsnd, read back from the SPU's voice mix by
   their zero crossings and peak. The song: A 440 for a second, bent up to B,
   A 880, then quieter; listen for it. Also a loop, a pause, the end, bad files,
   and the voices music leaves to mx/snd.h. */

#include "midi_t.h"

int main(void) {
  CHECK(mx_snd_init() == 0);
  if (t_fail)
    return t_done("midi");
  make_bank();
  bank.h.magic = 0;
  CHECK(mx_midi_load(&bank, sizeof bank) < 0);
  bank.h.magic = MIDI_BANK_MAGIC;
  bank.z[1].sample = 7;
  CHECK(mx_midi_load(&bank, sizeof bank) < 0); /* mxsnd's check */
  bank.z[1].sample = 1;
  CHECK(mx_midi_song(song, 100) < 0); /* no bank */
  CHECK(mx_midi_load(&bank, sizeof bank) == 0);
  CHECK(mx_midi_play(0) < 0); /* no song */
  memset(song, 0, sizeof song);
  CHECK(mx_midi_song(song, 100) < 0);

  begin();
  PUT(0, 0x90, 69, 127);                   /* 0 s */
  PUT(96, 0xE0, 0x7F, 0x7F);               /* 1 s: two semitones up */
  PUT(96, 0xE0, 0, 0x40, 0, 0x80, 69, 0);  /* 2 s */
  PUT(0, 0x90, 81, 100);
  PUT(96, 0xB0, 7, 30);                    /* 3 s */
  PUT(96, 0x80, 81, 0, 0, 0xB0, 7, 100);   /* 4 s */
  PUT(96, 0x99, 38, 127);                  /* 5 s: a drum */
  PUT(96, 0x89, 38, 0);
  end();
  CHECK(mx_midi_song(song, len) == 0);
  CHECK(!mx_midi_playing());
  CHECK(mx_midi_play(0) == 0);
  CHECK(mx_midi_playing());
  ms(500);
  CHECK(sounds("A 440", 8, 11));
  ms(1000);
  CHECK(sounds("B 494", 10, 12));
  ms(1000);
  CHECK(sounds("A 880", 17, 21));
  int loud = peak();
  ms(1000);
  int quiet = peak();
  mx_printf("midi: peak %d, at volume 30 of 100 %d\n", loud, quiet);
  CHECK(quiet < loud / 8 && quiet > loud / 16); /* 1/11 */
  ms(1000); /* 4.5 s: released */
  CHECK(peak() < 100);
  ms(530);
  CHECK(peak() > 1000); /* the drum */
  ms(1500);
  CHECK(!mx_midi_playing());

  /* a loop of 0.5 s on, 0.5 s off, running while the EE sleeps */
  begin();
  PUT(0, 0x90, 69, 127, 48, 69, 0, 48, 0xB0, 7, 100);
  end();
  CHECK(mx_midi_song(song, len) == 0);
  CHECK(mx_midi_play(1) == 0);
  int on = 0, off = 0;
  for (int i = 0; i < 60; i++) {
    ms(100);
    int p = peak();
    on += p > 1000, off += p < 100;
  }
  mx_printf("midi: a loop: %d on, %d off of 60\n", on, off);
  CHECK(on >= 24 && on <= 42 && off >= 12);
  CHECK(mx_midi_playing());
  mx_midi_pause(1);
  ms(700);
  for (int i = 0; i < 12; i++, ms(100))
    CHECK(peak() < 100);
  CHECK(mx_midi_playing());
  mx_midi_pause(0);
  on = 0; /* the rest of a note cut off is silent: up to a second */
  for (int i = 0; i < 20; i++, ms(100))
    on += peak() > 1000;
  mx_printf("midi: after the pause: %d on of 20\n", on);
  CHECK(on >= 5);

  /* mx/snd.h beside it: its voices stay below 24 */
  static struct mx_snd_sample s;
  CHECK(mx_snd_load(&s, bank.adpcm, bank.s[0].size, 48000) == 0);
  int most = 0;
  for (int i = 0; i < 40; i++) {
    int ch = mx_snd_ch_play(&s, 48000, 10, 10, i);
    most = (ch & 0xFF) > most ? ch & 0xFF : most;
  }
  mx_snd_commit();
  CHECK_EQ(most, 24);
  CHECK(mx_snd_play(&s, 30) < 0);
  mx_midi_stop();
  ms(100);
  CHECK(!mx_midi_playing());
  mx_midi_free();
  CHECK(mx_midi_play(0) < 0);
  CHECK(mx_snd_play(&s, 30) == 30);
  ms(30);
  CHECK(mx_snd_free(&s) == 0);
  CHECK(mx_midi_load(&bank, sizeof bank) == 0); /* again */
  mx_midi_free();
  extern volatile u32 mx__snd_dropped;
  CHECK_EQ(mx__snd_dropped, 0);
  return t_done("midi");
}
