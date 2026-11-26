/* Quake 3's way with a looping sound after another program wrote SPU RAM
   (apps/quake3/code/ps2/snd_ps2.c): the loop plays as a channel; a child
   with music ends; the loop's sample is lost, freed and loaded again by a
   thread of its own; the loop plays again, then goes quiet (volume 0) and
   stops. Then a copy that waits for the front over another switch: the
   thread beside takes the sound back while this one plays (one at a time,
   snd.c's own()). The voices' mix after each step (core 0's dry output).
     k91_sndreuse.elf     (status 0 expected) */
#include "t.h"
#include <mx/cpu.h>
#include <mx/proc.h>
#include <mx/snd.h>
#include <mx/string.h>
#include <mx/thread.h>

int mx__snd_peek(u32 spu, void *dst, u32 bytes);

#define N 28000
static s16 pcm[N];
static u8 adpcm[MX_SND_ADPCM_SIZE(N)] MX_ALIGNED(16);
static u32 bytes;
static struct mx_snd_sample s, job;
static volatile int job_state, job_delay;
static int sema;
static u8 stack[8192] MX_FAST_BSS MX_ALIGNED(16);

static int peak(void) {
  static s16 m[512] MX_ALIGNED(64);
  int p = 0;
  mx_snd_commit();
  mx_sleep_us(40000);
  for (int k = 0; k < 4; k++) {
    mx_sleep_us(30000);
    if (mx__snd_peek(0x2000, m, sizeof m) == 0)
      for (int i = 0; i < 512; i++)
        p = m[i] > p ? m[i] : -m[i] > p ? -m[i] : p;
  }
  return p;
}

static void copier(void *arg) {
  (void)arg;
  for (;;) {
    mx_sema_wait(sema);
    if (job_delay)
      mx_sleep_us((u32)job_delay);
    job_state = mx_snd_load(&job, adpcm, bytes, 22050) == 0 ? 1 : -1;
  }
}

int main(int argc, char **argv) {
  if (argc > 1) { /* the child: a sound of its own over the same SPU RAM */
    if (mx_snd_init() < 0)
      return 9;
    static u8 other[MX_SND_ADPCM_SIZE(N)] MX_ALIGNED(16);
    for (u32 i = 0; i < N; i++)
      pcm[i] = (s16)((i * 977) & 0x3FFF) - 0x2000;
    u32 n = mx_snd_encode(pcm, argv[1][0] == 's' ? 1000 : N, other, 1);
    struct mx_snd_sample o;
    CHECK(mx_snd_load(&o, other, n, 22050) == 0);
    mx_snd_ch_play(&o, 22050, MX_SND_VOL_MAX, MX_SND_VOL_MAX, 100);
    mx_snd_commit();
    mx_sleep_us(300000);
    return t_fail ? 1 : 5;
  }
  CHECK(mx_snd_init() == 0);
  for (u32 i = 0; i < N; i++)
    pcm[i] = (i / 25) & 1 ? 6000 : -6000;
  bytes = mx_snd_encode(pcm, N, adpcm, 1);
  CHECK(mx_snd_load(&s, adpcm, bytes, 22050) == 0);
  sema = mx_sema_create(0, 1);
  CHECK(mx_thread_start(copier, 0, stack, sizeof stack, mx_thread_prio(mx_thread_self()) - 1) > 0);
  int ch = mx_snd_ch_play(&s, 22050, MX_SND_VOL_MAX / 2, MX_SND_VOL_MAX / 2, 64);
  int p0 = peak();
  mx_snd_ch_set(ch, 22050, 0, 0);
  int p0q = peak();
  mx_printf("k91: before: loop %d, at volume 0 %d\n", p0, p0q);
  CHECK(p0 > 1000 && p0q < 50);
  mx_snd_ch_set(ch, 22050, MX_SND_VOL_MAX / 2, MX_SND_VOL_MAX / 2);
  mx_snd_commit();

  static const char args[] = "k91_sndreuse.elf\0child";
  int es = -1, id = mx_proc_spawn(2, args, sizeof args, 0);
  CHECK(id > 0 && mx_proc_wait(id, &es, 0) == 1 && es == 5);

  /* back: the channel is gone, the sample lost */
  int playing = mx_snd_ch_playing(ch), lost = mx_snd_lost(&s);
  mx_printf("k91: back: playing %d, lost %d, peak %d\n", playing, lost, peak());
  CHECK(!playing && lost);
  mx_snd_free(&s);
  memset(&s, 0, sizeof s);
  job_state = 0;
  mx_sema_signal(sema);
  for (int t = 0; !job_state && t < 3000; t++)
    mx_sleep_us(1000);
  mx_printf("k91: copier: %d\n", job_state);
  CHECK(job_state == 1);
  s = job;
  ch = mx_snd_ch_play(&s, 22050, MX_SND_VOL_MAX / 2, MX_SND_VOL_MAX / 2, 64);
  int p1 = peak();
  mx_snd_ch_set(ch, 22050, 0, 0);
  int p1q = peak();
  mx_snd_ch_set(ch, 22050, MX_SND_VOL_MAX / 2, MX_SND_VOL_MAX / 2);
  peak();
  mx_snd_ch_stop(ch);
  int p1s = peak();
  mx_printf("k91: again: loop %d, at volume 0 %d, stopped %d\n", p1, p1q, p1s);
  CHECK(p1 > 1000 && p1q < 50 && p1s < 50);

  /* a copy that waits for the front over another switch (Quake 3's thread
     beside the game): the child's small sample takes s's RAM only, s2 stays */
  static struct mx_snd_sample s2;
  CHECK(mx_snd_load(&s2, adpcm, bytes, 22050) == 0);
  mx_snd_free(&s);
  memset(&s, 0, sizeof s);
  static const char args2[] = "k91_sndreuse.elf\0small";
  job_state = 0, job_delay = 150000;
  id = mx_proc_spawn(2, args2, sizeof args2, 0);
  mx_sema_signal(sema);
  CHECK(id > 0 && mx_proc_wait(id, &es, 0) == 1 && es == 5);
  int lost2 = mx_snd_lost(&s2);
  mx_snd_free(&s2);
  CHECK(mx_snd_load(&s2, adpcm, bytes, 22050) == 0);
  int ch2 = mx_snd_ch_play(&s2, 22050, MX_SND_VOL_MAX / 2, MX_SND_VOL_MAX / 2, 64);
  mx_snd_commit();
  for (int t = 0; !job_state && t < 3000; t++)
    mx_sleep_us(1000);
  int p2 = peak();
  mx_snd_ch_stop(ch2);
  int p2s = peak();
  int p2a = peak();
  mx_printf("k91: copy over a switch: copier %d, s2 lost %d, ch %x, loop %d, stopped %d %d\n", job_state, lost2, ch2,
            p2, p2s, p2a);
  CHECK(job_state == 1 && p2s < 50 && p2a < 50);
  return t_done("k91");
}
