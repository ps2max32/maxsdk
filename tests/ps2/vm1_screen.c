/* VM step 1: the debug screen (<mx/debug.h>) that the next steps show their
   progress on. About 15 s; at the end the screen is bright green with
   900D0000 in slot 0 (top left), or bright red with BAD000nn.

   What it shows, a background colour (mx_dbg_step) and slot 0 at a time:
     1..8  one a second, the eight step colours; slot 1 counts, slots 4-7
           hold 01234567 89ABCDEF 00000000 FFFFFFFF
     9     3 s with interrupts off: slot 2 counts every 0.5 s while the
           heartbeat (slots 30, 31: bottom right) stands still
     10    2 s of a timer interrupt drawing slot 3
   The heartbeat runs all the time apart from step 9.

   vm1_screen.elf save: also writes the debug screen to host:vm1.ppm. */

#include "t.h"
#include <mx/sif.h>

static void wait_ms(u32 ms) {
  u32 t0 = mx_count();
  while (mx_count() - t0 < ms * (MX_EE_HZ / 1000))
    ;
}

static volatile u32 ticks;
static void tick(void *arg) {
  (void)arg;
  mx_dbg_word(3, ++ticks);
}

static u32 px[128 * 10] MX_ALIGNED(64);

static void save(void) {
  static u32 img[640 * 128] MX_ALIGNED(64);
  CHECK(mx_gs_download(img, MX_GS_PAGE(472), 10, MX_GS_PSM_CT32, 0, 0, 640, 128) == 0);
  int fd = mx_fio_open("host:vm1.ppm", MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  if (fd < 0)
    return;
  static const char hdr[] = "P6\n640 128\n255\n";
  mx_fio_write(fd, hdr, sizeof hdr - 1);
  static u8 line[640 * 3];
  for (u32 y = 0; y < 128; y++) {
    for (u32 x = 0; x < 640; x++) {
      u32 p = img[y * 640 + x];
      line[x * 3] = (u8)p, line[x * 3 + 1] = (u8)(p >> 8), line[x * 3 + 2] = (u8)(p >> 16);
    }
    mx_fio_write(fd, line, sizeof line);
  }
  mx_fio_close(fd);
}

int main(int argc, char **argv) {
  mx_printf("vm1: debug screen\n");
  mx_dbg_screen();
  mx_dbg_heartbeat(1);
  static const u32 pat[4] = {0x01234567, 0x89ABCDEF, 0, 0xFFFFFFFF};
  for (u32 i = 0; i < 4; i++)
    mx_dbg_word(4 + i, pat[i]);
  for (u32 s = 1; s <= 8; s++) {
    mx_dbg_step(s);
    mx_dbg_word(1, s);
    wait_ms(1000);
  }

  /* slot 4's first digit (0): its top left cell lit, the middle one not */
  CHECK(mx_gs_download(px, MX_GS_PAGE(472), 10, MX_GS_PSM_CT32, 16, 19, 128, 10) == 0);
  CHECK_EQ(px[0], 0x80FFFFFFu);
  CHECK_EQ(px[2 * 128 + 4], 0);
  /* its second (1, 0x2C97: 010 110 010 010 111): the top middle cell */
  CHECK_EQ(px[16 + 4], 0x80FFFFFFu);
  CHECK_EQ(px[16 + 0], 0);

  u32 beats = mx_irq_intc_count(MX_INTC_VBOF);
  CHECK(beats > 0);
  mx_printf("vm1: steps shown, %u vblank ends\n", beats);

  mx_dbg_step(9);
  int was = mx_di();
  u32 before = mx_irq_intc_count(MX_INTC_VBOF);
  for (u32 k = 1; k <= 6; k++) {
    mx_dbg_word(2, k);
    wait_ms(500);
  }
  CHECK_EQ(mx_irq_intc_count(MX_INTC_VBOF), before);
  mx_irq_restore(was);
  wait_ms(200);
  CHECK(mx_irq_intc_count(MX_INTC_VBOF) > before);
  mx_printf("vm1: interrupts off shown\n");

  mx_dbg_step(10);
  mx_timer_periodic(1, 57600, tick, 0); /* 100 ms */
  wait_ms(2000);
  mx_timer_periodic(1, 0, 0, 0);
  CHECK(ticks >= 15);
  mx_printf("vm1: %u timer interrupts drew\n", ticks);

  if (argc >= 2 && argv[1][0] == 's')
    save();
  mx_dbg_heartbeat(0);
  return t_done("vm1");
}
