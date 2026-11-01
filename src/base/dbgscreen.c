/* The debug screen (<mx/debug.h>): a background colour and 32 hex words on
   the TV, for finding where the PS2 hangs when no text comes back.

   Everything after mx_dbg_screen() is plain stores to the GS registers and
   the GIF FIFO, so it works with interrupts off, in an interrupt handler
   and at EXL = 1: no DMA, no interrupts, no allocation, only this file's
   static data and the I/O registers (BIOS TLB entries 0-12, which the VM
   never replaces).

   The words are drawn into a 640 x 128 buffer at the top of GS memory
   (pages 472-511, above mx_gs_init's buffers and the textures of the
   tests) with context 2, shown alone on read circuit 1 over BGCOLOR: its
   pixels are alpha 0 (BGCOLOR shows) or white with alpha 0x80 (the text). */

#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/dma.h>
#include <mx/gif.h>
#include <mx/gs.h>
#include <mx/irq.h>

#define FBP 472
#define FBW 10 /* 640 pixels */
#define W 640
#define H 128

/* 3 x 5 digits, the top row in bits 14..12; b and d lower case (B and D
   look like 8 and 0) */
static const u16 font[16] = {0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249,
                             0x7BEF, 0x7BCF, 0x7BED, 0x49AE, 0x7927, 0x12EB, 0x79E7, 0x79E4};

static int on;

/* room in the GIF FIFO (16 quadwords), or 0 after about 30 ms */
static int fifo_room(void) {
  for (u32 i = 0; i < 1000000; i++)
    if ((MX_R32(MX_GIF_STAT) >> 24 & 31) < 16)
      return 1;
  return 0;
}

static int put(u64 lo, u64 hi) {
  if (!fifo_room())
    return 0;
  mx_w128(MX_GIF_FIFO, mx_qw(lo, hi));
  return 1;
}

/* PATH3 free for the CPU: the GIF channel stopped (waiting up to about
   30 ms for it, or not at all) and PATH3 not masked */
static int path3_free(int wait) {
  u32 i = 0;
  while (MX_R32(MX_DMA_GIF) & MX_CHCR_STR)
    if (++i > (wait ? 1000000u : 0u))
      return 0;
  return !(MX_R32(MX_GIF_STAT) & 3); /* M3R, M3P */
}

/* context 2 drawing into the buffer, and colour c (alpha a) */
static int setup(u32 c, u32 a) {
  static const u32 regs[] = {MX_GS_REG_FRAME_2,      MX_GS_REG_ZBUF_2,  MX_GS_REG_TEST_2,
                             MX_GS_REG_XYOFFSET_2,   MX_GS_REG_SCISSOR_2, MX_GS_REG_ALPHA_2,
                             MX_GS_REG_FBA_2,        MX_GS_REG_PRMODECONT, MX_GS_REG_DTHE,
                             MX_GS_REG_COLCLAMP,     MX_GS_REG_RGBAQ,   MX_GS_REG_PRIM};
  const u64 val[] = {mx_gs_frame(FBP, FBW, MX_GS_PSM_CT32, 0),
                     mx_gs_zbuf(0, 0, 1),
                     mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1),
                     0,
                     mx_gs_scissor(0, W - 1, 0, H - 1),
                     0,
                     0,
                     1,
                     0,
                     1,
                     mx_gs_rgbaq(c & 0xFF, c >> 8 & 0xFF, c >> 16 & 0xFF, a, 1.0f),
                     /* REGLIST ignores the GIF tag's PRIM */
                     mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 1, 0)};
  const u32 n = sizeof regs / sizeof regs[0];
  if (!put(mx_giftag(n, 1, -1, MX_GIF_FLG_PACKED, 1), MX_GIF_REG_AD))
    return 0;
  for (u32 i = 0; i < n; i++)
    if (!put(val[i], regs[i]))
      return 0;
  return 1;
}

static u64 xy(u32 x, u32 y) { return mx_gs_xyz(x << 4, y << 4, 0); }

/* sprites [x, x + w) x [y, y + h), n of them in r[] (x, y, w, h) */
static int sprites(const u16 (*r)[4], u32 n) {
  if (!n)
    return 1;
  if (!put(mx_giftag(n, 1, -1, MX_GIF_FLG_REGLIST, 2),
           mx_gif_regs2(MX_GS_REG_XYZ2, MX_GS_REG_XYZ2)))
    return 0;
  for (u32 i = 0; i < n; i++)
    if (!put(xy(r[i][0], r[i][1]), xy(r[i][0] + r[i][2], r[i][1] + r[i][3])))
      return 0;
  return 1;
}

static void word(u32 slot, u32 v) {
  /* 4 words a line, 8 lines; a digit is 3 x 5 cells of 4 x 2 pixels (the
     display stretches the buffer about 3.5 times vertically) */
  u32 x0 = 16 + (slot & 3) * 160, y0 = 3 + (slot >> 2 & 7) * 16;
  const u16 box[1][4] = {{(u16)x0, (u16)y0, 128, 10}};
  if (!setup(0, 0) || !sprites(box, 1) || !setup(0xFFFFFF, 0x80))
    return;
  static u16 r[8 * 15][4];
  u32 n = 0;
  for (u32 d = 0; d < 8; d++, v <<= 4) {
    u16 g = font[v >> 28];
    for (u32 c = 0; c < 15; c++)
      if (g >> (14 - c) & 1) {
        r[n][0] = (u16)(x0 + d * 16 + (c % 3) * 4);
        r[n][1] = (u16)(y0 + (c / 3) * 2);
        r[n][2] = 4;
        r[n][3] = 2;
        n++;
      }
  }
  sprites((const u16(*)[4])r, n);
}

void mx_dbg_color(u32 rgb) { MX_R64(MX_GS_BGCOLOR) = rgb; }

void mx_dbg_word(u32 slot, u32 v) {
  if (!on)
    return;
  int was = mx_di();
  if (path3_free(1))
    word(slot, v);
  mx_irq_restore(was);
}

void mx_dbg_step(u32 n) {
  static const u32 rgb[8] = {0x000080, 0x008000, 0x800000, 0x008080,
                             0x800080, 0x808000, 0x404040, 0x0040c0};
  mx_dbg_color(rgb[n & 7]);
  mx_dbg_word(0, n);
}

void mx_dbg_screen(void) {
  if (!mx_gs.hz)
    mx_gs_init(640, 448);
  static struct mx_display d;
  d.c[0].on = 1;
  d.c[0].fbp = FBP;
  d.c[0].fbw = FBW;
  d.c[0].psm = MX_GS_PSM_CT32;
  d.c[0].w = W;
  d.c[0].h = H;
  d.c[1].on = 0;
  d.mmod = 0;
  d.slbg = 1;
  d.alp = 0xFF;
  d.bgcolor = 0;
  /* PMODE also turns circuit 2 off */
  mx_display_apply(&d);
  int was = mx_di();
  if (path3_free(1) && setup(0, 0)) {
    static const u16 all[1][4] = {{0, 0, W, H}};
    sprites(all, 1);
  }
  mx_irq_restore(was);
  on = 1;
}

/* the heartbeat: at every 16th vblank end, slot 31 gets the vblank count
   and slot 30 the EPC the interrupt came at (where the program is) */
static u32 beats;
static void beat(void *arg) {
  (void)arg;
  extern volatile u32 mx__irq_epc;
  if (++beats & 15)
    return;
  if (path3_free(0)) { /* not while the program's GIF DMA runs */
    word(30, mx__irq_epc);
    word(31, beats);
  }
}

void mx_dbg_heartbeat(int enable) { mx_irq_intc(MX_INTC_VBOF, enable ? beat : 0, 0); }
