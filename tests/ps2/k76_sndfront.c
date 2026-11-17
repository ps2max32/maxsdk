/* The sound is the process in front's (mx OS plan step 5): this program plays
   a looping sample, then starts itself as a child, which is in front; at
   that handover its voices and stream are silenced (mx_proc_spawn; the home
   button the same), so the loop is not heard beside the child. Back in
   front it plays again. Before, only the kernel's own home menu silenced it:
   home to the shell and a 1080i game's home left the game's sound on.
     k76_sndfront.elf     (status 0 expected; a tone, a pause, the tone) */
#include "t.h"
#include <mx/kernel.h>
#include <mx/proc.h>
#include <mx/snd.h>
#include <mx/string.h>
#include <mx/thread.h>

#define N 2800 /* 100 ADPCM blocks of 28 samples */
static s16 pcm[N];
static u8 adpcm[MX_SND_ADPCM_SIZE(N)] MX_ALIGNED(16);

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "child")) {
    mx_sleep_us(500000);
    return 5;
  }
  CHECK(mx__kernel && mx__kernel->version >= 40);
  CHECK_EQ(mx_snd_init(), 0);
  for (u32 i = 0; i < N; i++) { /* a triangle at about 441 Hz */
    u32 p = (i * 441 * 4 / 22050) & 3, f = (i * 441 * 4) % 22050;
    s32 v = (s32)(f * 8000 / 22050);
    pcm[i] = (s16)(p == 0 ? v : p == 1 ? 8000 - v : p == 2 ? -v : v - 8000);
  }
  u32 bytes = mx_snd_encode(pcm, N, adpcm, 1);
  static struct mx_snd_sample s;
  CHECK_EQ(mx_snd_load(&s, adpcm, bytes, 22050), 0);
  int ch = mx_snd_ch_play(&s, 22050, MX_SND_VOL_MAX / 4, MX_SND_VOL_MAX / 4, 100);
  mx_snd_commit();
  CHECK(ch > 0);
  mx_sleep_us(500000);
  int before = mx_snd_ch_playing(ch);

  static char args[256];
  u32 n = (u32)strlen(argv[0]) + 1;
  memcpy(args, argv[0], n);
  memcpy(args + n, "child", 6);
  int id = mx_proc_spawn(2, args, n + 6, 0), st = -1;
  CHECK(id > 0);
  CHECK_EQ(mx_proc_wait(id, &st, 0), 1);
  CHECK_EQ(st, 5);
  int after = mx_snd_ch_playing(ch);

  /* in front again: it plays */
  ch = mx_snd_ch_play(&s, 22050, MX_SND_VOL_MAX / 4, MX_SND_VOL_MAX / 4, 100);
  mx_snd_commit();
  mx_sleep_us(500000);
  int again = ch > 0 && mx_snd_ch_playing(ch);
  mx_snd_ch_stop(ch);
  mx_snd_commit();
  mx_snd_free(&s);
  mx_printf("k76: the loop played %d before the child, %d after it, %d played again\n", before, after, again);
  CHECK_EQ(before, 1);
  CHECK_EQ(after, 0);
  CHECK_EQ(again, 1);
  return t_done("k76");
}
