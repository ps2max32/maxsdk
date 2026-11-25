/* mxsnd belongs to one process at a time, the one in front (kernel version
   48):
   - a child opens the sound and plays; this one, beside it, plays too: that
     does not sound and does not disturb the child's;
   - the child ended, this one's status goes on (frames), none of its commands
     is dropped, its sample (the child wrote SPU RAM) is lost and does not
     play until it is loaded again, in mx_snd_on_lost's function;
   - a second child opens the sound and lets this one in front; this one
     takes the sound back, and ending the child leaves it its.
   With "end": this one, which has the sound, ends beside its child, which
   then opens it.
     k89_sndowner.elf [end]     (status 0 expected) */
#include "t.h"
#include <mx/kernel.h>
#include <mx/proc.h>
#include <mx/sif.h>
#include <mx/snd.h>
#include <mx/string.h>
#include <mx/thread.h>
#include "../../common/include/mxsnd.h"

extern volatile u32 mx__snd_dropped;
void mx__snd_counts(u32 *frames, u32 *starved);

#define N 2800
static s16 pcm[N];
static u8 adpcm[MX_SND_ADPCM_SIZE(N)] MX_ALIGNED(16);
static struct mx_snd_sample s;
static struct mx_sif_client srv;
static u32 rpc[16] MX_DMA_LINES;

static int loop_start(void) {
  for (u32 i = 0; i < N; i++)
    pcm[i] = (i / 25) & 1 ? 6000 : -6000;
  u32 bytes = mx_snd_encode(pcm, N, adpcm, 1);
  if (mx_snd_load(&s, adpcm, bytes, 22050) < 0)
    return 0;
  int ch = mx_snd_ch_play(&s, 22050, MX_SND_VOL_MAX / 4, MX_SND_VOL_MAX / 4, 100);
  mx_snd_commit();
  return ch;
}

static int fronts; /* mx__snd_front's calls */
void mx__snd_front(void) { fronts++; }
static int reloads, reload_res = -1;
static void on_lost(void) {
  reloads++;
  reload_res = mx_snd_lost(&s) ? mx_snd_reload(&s, adpcm) : -2;
}

/* frames move, nothing dropped, the loop plays: after "what" */
static int alive(const char *what) {
  u32 f1, f2, st, d0 = mx__snd_dropped;
  int ch = mx_snd_ch_play(&s, 22050, MX_SND_VOL_MAX / 4, MX_SND_VOL_MAX / 4, 100);
  mx_snd_commit();
  mx_sleep_us(100000);
  mx__snd_counts(&f1, &st);
  for (int i = 0; i < 1200; i++) { /* more than the ring holds (512) */
    mx_snd_voice_volume(1, 100 + (i & 63), 100);
    if (i % 100 == 99)
      mx_sleep_us(50000);
  }
  mx__snd_counts(&f2, &st);
  int on = ch > 0 && mx_snd_ch_playing(ch);
  mx_printf("k89: %s: frames %u to %u, %u dropped, playing %d, %d reloads\n", what, f1, f2,
            mx__snd_dropped - d0, on, reloads);
  return f2 > f1 && mx__snd_dropped == d0 && on;
}

static int spawn(const char *self, const char *a, const char *b) {
  static char args[256];
  u32 n = 0;
  const char *w[3] = {self, a, b};
  for (int i = 0; i < 3; i++) {
    u32 l = (u32)strlen(w[i]) + 1;
    memcpy(args + n, w[i], l);
    n += l;
  }
  return mx_proc_spawn(3, args, n, 0);
}

int main(int argc, char **argv) {
  if (argc > 2) {
    if (!strcmp(argv[1], "late")) {
      /* the parent is gone, and its sound with it: both cores' mix silent */
      mx_sleep_us(800000);
      if (mx_sif_bind_tries(&srv, MXSND_SID, 10) < 0 || !srv.server)
        return 13;
      volatile u32 *a = MX_UNCACHED(rpc);
      a[0] = MXSND_ALL, a[1] = a[2] = a[3] = 0;
      mx_sif_call(&srv, MXSND_STOP, (void *)a, 16, MX_UNCACHED(rpc), 16);
      mx_printf("k89: the sound had an owner after the parent's end: %u\n", a[0]);
      if (a[0])
        return 14;
    }
    if (mx_snd_init() < 0)
      return 9;
    int ch = loop_start();
    if (!ch)
      return 10;
    for (int i = 0; i < 100; i++) {
      mx_snd_voice_volume(1, 100 + i, 100);
      mx_sleep_us(4000);
    }
    u32 f, st;
    mx__snd_counts(&f, &st);
    if (!f || mx__snd_dropped || !mx_snd_ch_playing(ch))
      return 11;
    if (!strcmp(argv[1], "front")) {
      int parent = 0;
      for (const char *c = argv[2]; *c; c++)
        parent = parent * 10 + (*c - '0');
      mx_proc_front(parent);
      mx_sleep_us(10000000); /* ended by the parent */
      return 12;
    }
    return argv[1][0] == 'l' ? t_done("k89 late") : 5;
  }
  CHECK(mx__kernel && mx__kernel->version >= 48);
  CHECK_EQ(mx_snd_init(), 0);
  mx_snd_on_lost(on_lost);
  CHECK(loop_start() > 0);
  CHECK(alive("alone"));
  if (argc > 1) {
    CHECK(spawn(argv[0], "late", "0") > 0);
    return 3;
  }

  int id = spawn(argv[0], "child", "0"), es = -1, r = 0;
  CHECK(id > 0);
  for (int i = 0; i < 2000 && !(r = mx_proc_wait(id, &es, 1)); i++) { /* beside it */
    mx_snd_ch_play(&s, 22050, MX_SND_VOL_MAX / 4, MX_SND_VOL_MAX / 4, 100);
    mx_snd_voice_volume(2, 50 + (i & 63), 50);
    mx_sleep_us(2000);
  }
  CHECK_EQ(r, 1);
  CHECK_EQ(es, 5);
  CHECK(alive("after a child"));
  CHECK_EQ(reloads, 1);
  CHECK_EQ(reload_res, 0);
  CHECK_EQ(fronts, 1);

  char me[12];
  mx_snprintf(me, sizeof me, "%d", mx_proc_self());
  id = spawn(argv[0], "front", me);
  CHECK(id > 0);
  while (!mx__kernel->proc_in_front())
    mx_sleep_us(5000);
  mx_sleep_us(50000);
  /* in front at the child's start still: until it lets this one in again */
  mx_printf("k89: in front\n");
  for (int i = 0; i < 400 && reloads < 2; i++) {
    mx_snd_commit();
    mx_sleep_us(5000);
  }
  CHECK_EQ(reloads, 2);
  CHECK(alive("the child paused"));
  CHECK_EQ(mx_proc_kill(id, 6), 0);
  CHECK_EQ(mx_proc_wait(id, &es, 0), 1);
  CHECK(alive("the paused child ended"));
  CHECK_EQ(reloads, 2);
  CHECK_EQ(fronts, 2);
  return t_done("k89");
}
