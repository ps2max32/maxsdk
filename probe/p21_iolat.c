/* Probe: what one call of the IOP costs, in µs: the file server on host:
   (a seek, a 16 KiB and a 32 KiB READ, a 32 KiB WRITE) and audsrv (the
   room of the sound stream), 100 of each, first with the IOP otherwise idle,
   then while a sound stream plays. The VM pager's swap transfers are such
   calls (docs/vm.md, "Costs"): a swap in a seek and the READs of 32 KiB, a
   write out a seek and a WRITE.
   A 32 KiB READ is one call here (mx__fio_read_chunk, ee/src/base/sif.c), not
   the two of 16 KiB that mx_fio_read makes: its data is checked, as such
   reads once stalled under ps2link. A third run waits spinning
   (mx__sif_spin), as the SIF layer did before its waits slept. */

#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/snd.h>
#include <mx/timer.h>

#define N 100
#define KB 1024u
#define FILE_BYTES (512 * KB)

extern u32 mx__fio_read_chunk;
extern volatile u32 mx__fio_calls;
extern int mx__sif_spin;

static u32 buf[32 * KB / 4] MX_ALIGNED(64);
static s16 tone[2048] MX_ALIGNED(64);
static int fd, streaming;

static u32 W(u32 i) { return i * 2654435761u ^ 0x5Au; }

struct t {
  u32 sum, max, n, calls;
};

static void top_up(void) {
  if (!streaming)
    return;
  u32 room = mx_snd_stream_room();
  while (room >= sizeof tone) {
    mx_snd_stream_queue(tone, sizeof tone);
    room -= sizeof tone;
  }
}

static void add(struct t *t, u32 cycles, u32 calls) {
  u32 us = cycles / (MX_EE_HZ / 1000000u);
  t->sum += us, t->n++, t->calls += calls;
  if (us > t->max)
    t->max = us;
}

static void show(const char *what, const struct t *t) {
  mx_printf("p21:   %-26s mean %6u us, max %6u us, %u calls each\n", what,
            t->n ? t->sum / t->n : 0, t->max, t->n ? t->calls / t->n : 0);
}

static u32 x = 1;
static u32 slot(void) {
  x = x * 1103515245u + 12345u;
  return (x >> 12) % (FILE_BYTES / (32 * KB));
}

static int run(const char *phase) {
  struct t seek = {0}, r16 = {0}, r32 = {0}, w32 = {0}, room = {0};
  u32 bad = 0;
  for (u32 n = 0; n < N; n++) {
    u32 s = slot(), t0, c0;

    top_up();
    c0 = mx__fio_calls, t0 = mx_count();
    mx_fio_lseek(fd, (s32)(s * 32 * KB), MX_FIO_SEEK_SET);
    add(&seek, mx_count() - t0, mx__fio_calls - c0);

    top_up();
    mx__fio_read_chunk = 16 * KB;
    c0 = mx__fio_calls, t0 = mx_count();
    int got = mx_fio_read(fd, buf, 16 * KB);
    add(&r16, mx_count() - t0, mx__fio_calls - c0);
    bad += got != (int)(16 * KB);

    top_up();
    mx_fio_lseek(fd, (s32)(s * 32 * KB), MX_FIO_SEEK_SET);
    mx__fio_read_chunk = 32 * KB;
    c0 = mx__fio_calls, t0 = mx_count();
    got = mx_fio_read(fd, buf, 32 * KB);
    add(&r32, mx_count() - t0, mx__fio_calls - c0);
    mx__fio_read_chunk = 16 * KB;
    bad += got != (int)(32 * KB);
    for (u32 i = 0; i < 32 * KB / 4; i++)
      bad += buf[i] != W(s * 8 * KB + i);

    top_up();
    mx_fio_lseek(fd, (s32)(s * 32 * KB), MX_FIO_SEEK_SET);
    c0 = mx__fio_calls, t0 = mx_count();
    mx_fio_write(fd, buf, 32 * KB); /* the same data back */
    add(&w32, mx_count() - t0, mx__fio_calls - c0);

    if (streaming) {
      t0 = mx_count();
      (void)mx_snd_stream_room();
      add(&room, mx_count() - t0, 0);
    }
  }
  mx_printf("p21: %s (%u of each)\n", phase, N);
  show("seek", &seek);
  show("READ 16 KiB", &r16);
  show("READ 32 KiB (one call)", &r32);
  show("WRITE 32 KiB", &w32);
  if (streaming)
    show("audsrv room", &room);
  mx_printf("p21:   %u reads short or wrong\n", bad);
  return bad != 0;
}

int main(void) {
  fd = mx_fio_open("host:p21.bin", 3 /* RDWR */ | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0) {
    mx_printf("p21: cannot open host:p21.bin\n");
    MX_TEST_END(1);
    return 1;
  }
  for (u32 at = 0; at < FILE_BYTES; at += 32 * KB) {
    for (u32 i = 0; i < 32 * KB / 4; i++)
      buf[i] = W(at / 4 + i);
    mx_fio_write(fd, buf, 32 * KB);
  }
  int bad = run("the IOP otherwise idle");
  mx__sif_spin = 1;
  bad |= run("waits spinning (as before sleeping waits)");
  mx__sif_spin = 0;

  for (u32 i = 0; i < 2048; i++)
    tone[i] = (s16)((i & 64) ? 3000 : -3000);
  if (mx_snd_init() == 0 && mx_snd_stream_format(48000, 16, 1) == 0) {
    streaming = 1;
    mx_snd_stream_volume(0x1000);
    top_up();
    bad |= run("a sound stream playing");
    mx_snd_stream_stop();
  } else
    mx_printf("p21: no sound (mx_snd_init)\n");
  mx_fio_close(fd);
  MX_TEST_END(bad ? 1 : 0);
  return bad;
}
