/* Music (mx/midi.h) and a child in front: the parent's song pauses while the
   child has the sound, and goes on after it; a child with music of its own
   ends, and the parent's bank and song are handed over again; a child that
   ends with its song playing leaves nothing playing.
     k90_music.elf     (status 0 expected) */
#include "midi_t.h"
#include <mx/kernel.h>
#include <mx/proc.h>

static int spawn(const char *self, const char *a) {
  static char args[256];
  u32 n = (u32)strlen(self) + 1, m = (u32)strlen(a) + 1;
  memcpy(args, self, n), memcpy(args + n, a, m);
  return mx_proc_spawn(2, args, n + m, 0);
}

static int start(void) {
  make_bank();
  begin();
  PUT(0, 0x90, 69, 127, 96, 69, 127, 96, 69, 0);
  end();
  return mx_midi_load(&bank, sizeof bank) == 0 && mx_midi_song(song, len) == 0 && mx_midi_play(1) == 0;
}

static int loudest(void) {
  int p = 0;
  for (int i = 0; i < 12; i++, ms(50)) {
    int q = peak();
    p = q > p ? q : p;
  }
  return p;
}

int main(int argc, char **argv) {
  if (argc > 1) {
    if (mx_snd_init() < 0)
      return 9;
    ms(300);
    int p = loudest();
    mx_printf("k90: child %s: the parent's song %d\n", argv[1], p);
    if (p > 100)
      return 10;
    if (argv[1][0] == 'm') {
      if (!start())
        return 11;
      ms(300);
      p = loudest();
      mx_printf("k90: the child's song %d\n", p);
      if (p < 1000)
        return 12;
    }
    return 5;
  }
  CHECK(mx_snd_init() == 0);
  CHECK(start());
  ms(300);
  CHECK(loudest() > 1000);
  const char *kinds[2] = {"plain", "music"};
  for (int k = 0; k < 2; k++) {
    int es = -1, id = spawn(argv[0], kinds[k]);
    CHECK(id > 0);
    CHECK_EQ(mx_proc_wait(id, &es, 0), 1);
    CHECK_EQ(es, 5);
    mx_snd_commit();
    ms(300);
    int p = loudest();
    mx_printf("k90: after child %s: %d, playing %d\n", kinds[k], p, mx_midi_playing());
    CHECK(p > 1000);
    CHECK(mx_midi_playing());
  }
  mx_midi_free();
  ms(50);
  CHECK(loudest() < 100);
  return t_done("k90");
}
