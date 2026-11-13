/* The IPU (<mx/ipu.h>): every command on streams made here. The macroblocks
   are intra ones with DC coefficients only, so each 8 x 8 block is one
   value and the result is known exactly: the manual's colour conversion
   (8.6.1) of it. */

#include <mx/ipu.h>

#include "t.h"

static u8 bs[512] MX_ALIGNED(64) MX_CORE_BSS;
static u32 out[4 * 256] MX_ALIGNED(64) MX_CORE_BSS;  /* 4 macroblocks RGB32 */
static u32 out2[4 * 256] MX_ALIGNED(64) MX_CORE_BSS; /* PACK's output */
static u8 raw8[2 * 384] MX_ALIGNED(64) MX_CORE_BSS;

/* ---- a bit stream writer ------------------------------------------------ */

static u32 wpos; /* in bits */
static void put(u32 v, u32 n) {
  for (u32 i = n; i-- > 0; wpos++)
    if (v >> i & 1)
      bs[wpos >> 3] |= (u8)(0x80 >> (wpos & 7));
}
static void put_start(u32 code) {
  wpos = (wpos + 7) & ~7u;
  put(0x00000100u | code, 32);
}
static void bs_begin(void) {
  memset(bs, 0, sizeof bs);
  wpos = 0;
}
static void bs_end(void) { mx_dcache_writeback(bs, sizeof bs); }

/* dct_dc_size codes (ISO 13818-2 tables B.12, B.13): {code, length} */
static const u16 dc_luma[12][2] = {{4, 3},  {0, 2},  {1, 2},   {5, 3},   {6, 3},   {14, 4},
                                   {30, 5}, {62, 6}, {126, 7}, {254, 8}, {510, 9}, {511, 9}};
static const u16 dc_chroma[12][2] = {{0, 2},   {1, 2},   {2, 2},   {6, 3},    {14, 4},   {30, 5},
                                     {62, 6}, {126, 7}, {254, 8}, {510, 9}, {1022, 10}, {1023, 10}};

static int pred[3];
static void put_dc(int cc, int dc) {
  int diff = dc - pred[cc], a = diff < 0 ? -diff : diff, size = 0;
  pred[cc] = dc;
  while (a >> size)
    size++;
  if (cc == 0)
    put(dc_luma[size][0], dc_luma[size][1]);
  else
    put(dc_chroma[size][0], dc_chroma[size][1]);
  if (size)
    put((u32)(diff < 0 ? diff + (1 << size) - 1 : diff), (u32)size);
  put(2, 2); /* end of block (table B.14) */
}

/* The six blocks of an intra macroblock: four luma values (the quadrants),
   Cb, Cr. */
struct mb {
  u8 y[4], cb, cr;
};
static void put_blocks(const struct mb *m) {
  for (int i = 0; i < 4; i++)
    put_dc(0, m->y[i]);
  put_dc(1, m->cb);
  put_dc(2, m->cr);
}
static void slice_begin(void) { pred[0] = pred[1] = pred[2] = 128; }

/* ---- the manual's conversion (8.6.1) -------------------------------------- */

/* "The lower 6 bits are rounded down": of the product's magnitude, so
   towards 0 (the PS2's answer; docs/ipu.md) */
static int down(int v) { return v < 0 ? -(-v & ~0x3F) : v & ~0x3F; }
static u32 clip8(int v) { return (u32)(v < 0 ? 0 : v > 255 ? 255 : v); }
static u32 csc_ref(int y, int cb, int cr, u32 th0, u32 th1) {
  int cr1 = cr - 128, cb1 = cb - 128;
  int cr4 = down(0xCC * cr1), cr5 = down(0x68 * cr1);
  int cb4 = down(0x32 * cb1), cb5 = down(0x102 * cb1);
  int y3 = (0x95 * (y - 16)) & ~0x3F;
  /* s12.1 in units of 1/128: round at bit 0 of the .1 value */
  u32 r = clip8(((y3 + cr4) + 64) >> 7);
  u32 g = clip8(((y3 - cb4 - cr5) + 64) >> 7);
  u32 b = clip8(((y3 + cb5) + 64) >> 7);
  if (r < th0 && g < th0 && b < th0)
    return 0; /* the colour too */
  u32 a = r < th1 && g < th1 && b < th1 ? 0x40 : 0x80;
  return a << 24 | b << 16 | g << 8 | r;
}
static u32 mb_ref(const struct mb *m, int x, int y, u32 th0, u32 th1) {
  return csc_ref(m->y[(y >> 3) * 2 + (x >> 3)], m->cb, m->cr, th0, th1);
}
static u32 to16(u32 p) {
  return (p >> 3 & 31) | (p >> 11 & 31) << 5 | (p >> 19 & 31) << 10 | ((p >> 24) == 0x40) << 15;
}

/* The largest difference of a colour of n macroblocks RGB32 at o to the
   reference; alpha must be equal. */
static int diff_rgb32(const u32 *o, const struct mb *m, int n, u32 th0, u32 th1) {
  int max = 0;
  for (int i = 0; i < n; i++)
    for (int p = 0; p < 256; p++) {
      u32 got = o[i * 256 + p], want = mb_ref(&m[i], p & 15, p >> 4, th0, th1);
      for (int c = 0; c < 24; c += 8) {
        int d = (int)(got >> c & 255) - (int)(want >> c & 255);
        if (d < 0)
          d = -d;
        if (d > max) {
          if (!max)
            mx_printf("  first difference: Y %d Cb %d Cr %d is %06x, by the manual %06x\n",
                      m[i].y[(p >> 7) * 2 + (p >> 3 & 1)], m[i].cb, m[i].cr, got & 0xFFFFFF,
                      want & 0xFFFFFF);
          max = d;
        }
      }
      if ((got ^ want) >> 24) {
        if (max < 256)
          mx_printf("alpha: mb %d pixel %d = %08x, expected %08x\n", i, p, got, want);
        max = 256;
      }
    }
  return max;
}

static const struct mb mbs[3] = {
    {{16, 235, 81, 145}, 90, 240},
    {{41, 210, 170, 106}, 240, 110},
    {{128, 128, 20, 20}, 128, 128},
};

/* ---- the tests ------------------------------------------------------------ */

static void fdec(void) {
  bs_begin();
  for (int i = 0; i < 64; i++)
    bs[i] = (u8)(0x11 * (i & 15) + (i >> 4));
  bs_end();
  mx_ipu_input(bs, 4, 0);
  CHECK_EQ(mx_ipu_fdec(0), 0x00112233u);
  CHECK_EQ(mx_ipu_pos(), 0);
  CHECK_EQ(mx_ipu_fdec(8), 0x11223344u);
  CHECK_EQ(mx_ipu_top(), 0x11223344u);
  CHECK_EQ(mx_ipu_pos(), 8);
  CHECK_EQ(mx_ipu_fdec(32), 0x55667788u);
  CHECK_EQ(mx_ipu_fdec(4), 0x56677889u);
  CHECK_EQ(mx_ipu_pos(), 44);
  /* across the first quadword */
  for (int i = 0; i < 3; i++)
    mx_ipu_fdec(32);
  CHECK_EQ(mx_ipu_pos(), 140);
  CHECK_EQ(mx_ipu_fdec(0), 0x22334455u);
  /* a start at a bit of the first quadword */
  mx_ipu_input(bs, 4, 100);
  CHECK_EQ(mx_ipu_fdec(0), 0xCDDEEFF0u);
  CHECK_EQ(mx_ipu_pos(), 100);
  /* the address increment's table: '1' is 1, '011' is 2 (manual 8.5.3) */
  bs_begin();
  put(1, 1);
  put(3, 3);
  bs_end();
  mx_ipu_input(bs, 4, 0);
  u32 v = mx_ipu_vdec(MX_IPU_VLC_MBAI, 0);
  CHECK_EQ(v, 0x00010001u);
  v = mx_ipu_vdec(MX_IPU_VLC_MBAI, 0);
  CHECK_EQ(v, 0x00030002u);
  CHECK_EQ(mx_ipu_pos(), 4);
}

/* A slice as in a picture: start code, quantiser_scale_code, the extra bit,
   the first address increment; then the macroblocks; then another start
   code. */
static u32 make_slice(int n, u32 next) {
  bs_begin();
  put_start(0x01);
  put(2, 5); /* quantiser_scale_code */
  put(0, 1); /* extra_bit_slice */
  slice_begin();
  for (int i = 0; i < n; i++) {
    put(1, 1); /* macroblock_address_increment 1 */
    put(1, 1); /* macroblock_type: intra */
    put_blocks(&mbs[i]);
  }
  put_start(next);
  u32 end = wpos - 32;
  bs_end();
  return end;
}

static void idec(void) {
  u32 end = make_slice(3, 0xB7);
  mx_ipu_setth(0, 0);
  mx_ipu_input(bs, sizeof bs / 16, 0);
  CHECK_EQ(mx_ipu_fdec(0), 0x00000101u);
  u32 v = mx_ipu_fdec(32);
  CHECK_EQ(v >> 25, 2 << 2 | 1); /* the code, the extra bit, the increment */
  memset(out, 0x55, sizeof out);
  mx_dcache_writeback(out, sizeof out);
  int n = mx_ipu_idec(7, v >> 27, 0, out, 4);
  mx_dcache_invalidate(out, sizeof out);
  CHECK_EQ(n, 3);
  u32 st = mx_ipu_status();
  CHECK_EQ(MX_GET(IPU_CTRL_SCD, st), 1);
  CHECK_EQ(MX_GET(IPU_CTRL_ECD, st), 0);
  CHECK_EQ(mx_ipu_top(), 0x000001B7u);
  CHECK_EQ(mx_ipu_pos(), end);
  int d = diff_rgb32(out, mbs, 3, 0, 0);
  mx_printf("ipu: IDEC RGB32, largest difference to the manual's conversion: %d\n", d);
  CHECK(d <= 1);
  CHECK_EQ(out[3 * 256], 0x55555555u); /* nothing past the slice */

  /* RGB16, no dither; and the thresholds: mbs[2]'s dark half (20: 5, 5, 5)
     below th0 8, its other half (128: 130, 130, 130) below th1 140 */
  mx_ipu_setth(8, 140);
  mx_ipu_input(bs, sizeof bs / 16, 0);
  n = mx_ipu_idec(39, 2, MX_IPU_RGB16, out, 4);
  mx_dcache_invalidate(out, sizeof out);
  CHECK_EQ(n, 3);
  const u16 *o16 = (const u16 *)out;
  int bad = 0;
  for (int i = 0; i < 3; i++)
    for (int p = 0; p < 256; p++) {
      /* the alpha bit only: a colour one off (the IDCT) may be at a step of 8 */
      u32 want = to16(mb_ref(&mbs[i], p & 15, p >> 4, 8, 140));
      bad += ((o16[i * 256 + p] ^ want) & 0x8000) != 0;
    }
  CHECK_EQ(bad, 0);
  CHECK_EQ(o16[2 * 256], 0x8000 | 16 << 10 | 16 << 5 | 16); /* 130 >> 3, alpha 0x40 */
  CHECK_EQ(o16[2 * 256 + 255], 0);                           /* 5 >> 3, alpha 0 */
  mx_ipu_input(bs, sizeof bs / 16, 0);
  n = mx_ipu_idec(39, 2, 0, out, 4);
  mx_dcache_invalidate(out, sizeof out);
  CHECK_EQ(n, 3);
  d = diff_rgb32(out, mbs, 3, 8, 140);
  if (d > 1)
    mx_printf("thresholds: difference %d\n", d);
  CHECK(d <= 1);
  mx_ipu_setth(0, 0);
}

/* The stream comes in two parts: the command waits for the second. */
static void starved(void) {
  make_slice(3, 0xB7);
  mx_ipu_input(bs, 1, 0);
  int n = mx_ipu_idec(39, 2, 0, out, 4);
  CHECK_EQ(n, MX_IPU_STARVED);
  mx_ipu_more(bs + 16, sizeof bs / 16 - 1);
  n = mx_ipu_wait();
  mx_dcache_invalidate(out, sizeof out);
  CHECK_EQ(n, 3);
  CHECK(diff_rgb32(out, mbs, 3, 0, 0) <= 1);
  CHECK_EQ(mx_ipu_top(), 0x000001B7u);

  /* more macroblocks than the buffer takes */
  mx_ipu_input(bs, sizeof bs / 16, 0);
  n = mx_ipu_idec(39, 2, 0, out, 2);
  CHECK_EQ(n, MX_IPU_FULL);
  /* and the IPU works after it */
  mx_ipu_input(bs, sizeof bs / 16, 0);
  CHECK_EQ(mx_ipu_fdec(0), 0x00000101u);
}

static void bdec(void) {
  bs_begin();
  slice_begin();
  put_blocks(&mbs[1]);
  put(1, 1); /* the next macroblock: no start code follows */
  bs_end();
  static s16 mb16[384] MX_ALIGNED(64) MX_CORE_BSS;
  mx_ipu_input(bs, sizeof bs / 16, 0);
  int n = mx_ipu_bdec(0, 2, MX_IPU_BDEC_INTRA | MX_IPU_BDEC_DCR, mb16);
  mx_dcache_invalidate(mb16, sizeof mb16);
  CHECK_EQ(n, 1);
  CHECK_EQ(MX_GET(IPU_CTRL_ECD, mx_ipu_status()), 0);
  int bad = 0;
  for (int p = 0; p < 256; p++)
    bad += mb16[p] != mbs[1].y[(p >> 7) * 2 + (p >> 3 & 1)];
  for (int p = 0; p < 64; p++)
    bad += mb16[256 + p] != mbs[1].cb || mb16[320 + p] != mbs[1].cr;
  CHECK_EQ(bad, 0);
  if (bad)
    mx_printf("bdec: Y %d %d %d %d Cb %d Cr %d\n", mb16[0], mb16[8], mb16[128], mb16[136],
              mb16[256], mb16[320]);
}

static void fill_raw8(u8 *r, const struct mb *m) {
  for (int p = 0; p < 256; p++)
    r[p] = m->y[(p >> 7) * 2 + (p >> 3 & 1)];
  memset(r + 256, m->cb, 64);
  memset(r + 320, m->cr, 64);
}

static void csc_pack(void) {
  fill_raw8(raw8, &mbs[0]);
  fill_raw8(raw8 + 384, &mbs[1]);
  mx_dcache_writeback(raw8, sizeof raw8);
  int n = mx_ipu_csc(raw8, 2, 0, out);
  mx_dcache_invalidate(out, sizeof out);
  CHECK_EQ(n, 2);
  int d = diff_rgb32(out, mbs, 2, 0, 0);
  mx_printf("ipu: CSC RGB32, largest difference to the manual's conversion: %d\n", d);
  CHECK(d <= mx_ipu_qsc_is_scale()); /* PCSX2 is one off for some inputs (docs/pcsx2.md) */

  /* PACK to RGB16: the colours' upper five bits */
  mx_dcache_writeback(out, sizeof out);
  n = mx_ipu_pack(out, 2, MX_IPU_PACK_RGB16, out2);
  mx_dcache_invalidate(out2, sizeof out2);
  CHECK_EQ(n, 2);
  int bad = 0;
  for (int p = 0; p < 512; p++)
    bad += ((const u16 *)out2)[p] != to16(out[p]);
  CHECK_EQ(bad, 0);

  /* PACK to INDX4: a CLUT with the macroblock's own four colours at 3, 7, 9
     and 12, the others far off */
  u16 clut[16];
  for (int i = 0; i < 16; i++)
    clut[i] = (u16)(i & 1 ? 0x7C1F : 0x03E0);
  clut[3] = (u16)to16(out[0]);
  clut[7] = (u16)to16(out[8]);
  clut[9] = (u16)to16(out[128]);
  clut[12] = (u16)to16(out[136]);
  mx_ipu_setvq(clut);
  n = mx_ipu_pack(out, 1, 0, out2);
  mx_dcache_invalidate(out2, sizeof out2);
  CHECK_EQ(n, 1);
  static const u8 want[4] = {3, 7, 9, 12};
  bad = 0;
  for (int p = 0; p < 256; p++)
    bad += (((const u8 *)out2)[p >> 1] >> ((p & 1) * 4) & 15) != want[(p >> 7) * 2 + (p >> 3 & 1)];
  CHECK_EQ(bad, 0);
}

int main(void) {
  CHECK_EQ(mx_ipu_init(), 0);
  mx_printf("ipu: IDEC takes QSC as the %s\n", mx_ipu_qsc_is_scale() ? "scale (PCSX2)" : "code (the manual)");
  fdec();
  idec();
  starved();
  bdec();
  csc_pack();
  mx_printf("ipu: interrupts: IPU %u, fromIPU %u, toIPU %u\n", mx_irq_intc_count(MX_INTC_IPU),
            mx_irq_dmac_count(MX_CH_FROMIPU), mx_irq_dmac_count(MX_CH_TOIPU));
  mx_ipu_exit();
  return t_done("ipu");
}
