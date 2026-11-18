/* Beam racing's display stays off until a frame was drawn (2026-11-18: a
   game started from the shell showed the shell's old strips and its own
   undrawn framebuffer for a field or two after mx_hires_init): PMODE's
   circuits are off after mx_hires_init and on once a record was played a
   whole field. PMODE is write-only: maxsdk's copy (mx_gs.dregs) is read.
     k78_hiresdisp.elf     (status 0 expected; 1080i a moment) */
#include "t.h"
#include <mx/display.h>
#include <mx/frame.h>
#include <mx/gs.h>
#include <mx/hires.h>
#include <mx/hw/formats.h>
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/video.h>

MX_CORE_HEAP(512 << 10);

static u8 rec[3 * 128 * 1024] MX_FAST_BSS MX_ALIGNED(64);

static int on(void) {
  u64 p = mx_gs.dregs[MX_DISP_PMODE];
  return (MX_GET(GS_PMODE_EN1, p) | MX_GET(GS_PMODE_EN2, p)) != 0;
}

int main(void) {
  CHECK(mx__kernel && mx__kernel->version >= 41);
  CHECK(mx_hires_init(&mx_video_1080i, 960, 540, MX_GS_PSM_CT16S, rec, sizeof rec, (void *)0x70002000,
                      0x2000) >= 0);
  int at_init = on(), first_on = -1;
  for (int i = 0; i < 30; i++) {
    mx_frame_begin_n();
    if (first_on < 0 && on())
      first_on = i;
    mx_hires_clear(0x20, 0x40, 0x20);
    mx_frame_end();
  }
  mx_printf("k78: display on at init %d, first on at frame %d (fields %u)\n", at_init, first_on,
            mx_hires_stats.fields);
  CHECK_EQ(at_init, 0);
  CHECK(first_on >= 1 && first_on < 10);
  return t_done("k78");
}
