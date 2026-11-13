/* The CRTC timing computed from a description (<mx/crtc.h>). The field layout of
   the sync registers is Linux's (drivers/ps2, frno7); the values the BIOS writes
   were read from its SetGsCrt (tests/host/test_crtc.c has them). */

#include <mx/bits.h>
#include <mx/crtc.h>
#include <mx/hw/regs.h>

#define MHZ_13_5 13500000ull

u32 mx_crtc_vck(u64 smode1) {
  u32 lc = MX_GET(GS_SMODE1_LC, smode1), rc = MX_GET(GS_SMODE1_RC, smode1);
  u32 t = MX_GET(GS_SMODE1_T1248, smode1);
  return rc ? (u32)(MHZ_13_5 * lc / ((u64)rc << t)) : 0;
}

/* The PLL settings the BIOS uses, tried first: of equal results the BIOS's wins. */
static const u8 bios_pll[][3] = {
    /* lc, rc, t1248 */
    {32, 4, 1}, /* 54 MHz: NTSC, PAL, 480p */
    {22, 2, 1}, /* 74.25 MHz: 720p, 1080i */
    {15, 2, 1}, /* 50.625 */
    {28, 3, 1}, /* 63 */
    {16, 3, 0}, /* 72 */
    {71, 6, 1}, /* 79.875 */
    {74, 5, 1}, /* 99.9 */
    {44, 3, 1}, /* 99 */
    {29, 3, 0}, /* 130.5 */
    {67, 6, 1}, /* 75.375 */
    {35, 3, 1}, /* 78.75 */
    {7, 1, 0},  /* 94.5 */
    {8, 1, 0},  /* 108 */
    {10, 1, 0}, /* 135 */
};

static u64 pll(u32 hz) {
  u32 best = ~0u, blc = 0, brc = 0, bt = 0;
  for (u32 i = 0; i < sizeof bios_pll / sizeof bios_pll[0]; i++) {
    u32 lc = bios_pll[i][0], rc = bios_pll[i][1], t = bios_pll[i][2];
    u32 v = (u32)(MHZ_13_5 * lc / ((u64)rc << t)), d = v > hz ? v - hz : hz - v;
    if (d < best)
      best = d, blc = lc, brc = rc, bt = t;
  }
  for (u32 t = 0; t < 2 && best; t++)
    for (u32 rc = 1; rc <= 6; rc++)
      for (u32 lc = 1; lc < 128; lc++) {
        u32 v = (u32)(MHZ_13_5 * lc / ((u64)rc << t)), d = v > hz ? v - hz : hz - v;
        if (d < best)
          best = d, blc = lc, brc = rc, bt = t;
      }
  return MX_SET(GS_SMODE1_LC, blc) | MX_SET(GS_SMODE1_RC, brc) | MX_SET(GS_SMODE1_T1248, bt);
}

static int fits(u32 v, u64 mask) { return v <= mask; }

int mx_crtc_regs(const struct mx_crtc *c, struct mx_crtc_regs *r) {
  u32 tv = c->kind == MX_CRTC_NTSC || c->kind == MX_CRTC_PAL;
  u32 hseq = c->hseq ? c->hseq : c->hs;
  u32 total = c->hs + c->hbp + c->hact + c->hfp, half = total / 2;
  u32 hf = half - c->hs - c->hbp, hb = c->hact - hf;
  u32 hsvs = half - (tv ? c->hs : hseq);
  u32 pck = c->vck_hz / c->spml;
  if (!c->spml || c->hs + c->hbp >= half || hf >= c->hact)
    return -1;
  if (!fits(c->hfp, MX_GS_SYNCH1_HFP_MASK) || !fits(c->hbp, MX_GS_SYNCH1_HBP_MASK) ||
      !fits(hseq, MX_GS_SYNCH1_HSEQ_MASK) || !fits(hsvs, MX_GS_SYNCH1_HSVS_MASK) ||
      !fits(hf, MX_GS_SYNCH2_HF_MASK) || !fits(hb, MX_GS_SYNCH2_HB_MASK) ||
      !fits(c->vfp, MX_GS_SYNCV_VFP_MASK) || !fits(c->vfpe, MX_GS_SYNCV_VFPE_MASK) ||
      !fits(c->vbp, MX_GS_SYNCV_VBP_MASK) || !fits(c->vbpe, MX_GS_SYNCV_VBPE_MASK) ||
      !fits(c->vdp, MX_GS_SYNCV_VDP_MASK) || !fits(c->vs, MX_GS_SYNCV_VS_MASK))
    return -1;

  r->smode1 = pll(c->vck_hz) | MX_SET(GS_SMODE1_CMOD, tv ? c->kind : 0) |
              MX_SET(GS_SMODE1_SPML, c->spml) |
              MX_SET(GS_SMODE1_PCK2, c->kind == MX_CRTC_HD && c->vdp == 576) |
              MX_SET(GS_SMODE1_GCONT, c->kind != MX_CRTC_VESA) | MX_SET(GS_SMODE1_CLKSEL, 1) |
              MX_SET(GS_SMODE1_NVCK, 1) | MX_SET(GS_SMODE1_SLCK2, 1) |
              MX_SET(GS_SMODE1_VCKSEL, c->kind != MX_CRTC_VESA && c->vdp <= 576) |
              MX_SET(GS_SMODE1_VHP, !tv && !c->interlace);
  /* the BIOS leaves PRST set for the 54 MHz TV modes (NTSC, PAL, 480p) */
  if (MX_GET(GS_SMODE1_VCKSEL, r->smode1))
    r->smode1 |= MX_SET(GS_SMODE1_PRST, 1);
  r->smode2 = MX_SET(GS_SMODE2_INT, c->interlace) | MX_SET(GS_SMODE2_FFMD, c->interlace && c->ffmd);
  r->srfsh = MX_SET(GS_SRFSH_RFSH, pck < 20000000 ? 8 : pck < 75000000 ? 4 : 2);
  r->synch1 = MX_SET(GS_SYNCH1_HFP, c->hfp) | MX_SET(GS_SYNCH1_HBP, c->hbp) |
              MX_SET(GS_SYNCH1_HSEQ, hseq) | MX_SET(GS_SYNCH1_HSVS, hsvs) |
              MX_SET(GS_SYNCH1_HS, c->hs);
  r->synch2 = MX_SET(GS_SYNCH2_HF, hf) | MX_SET(GS_SYNCH2_HB, hb);
  r->syncv = MX_SET(GS_SYNCV_VFP, c->vfp) | MX_SET(GS_SYNCV_VFPE, c->vfpe) |
             MX_SET(GS_SYNCV_VBP, c->vbp) | MX_SET(GS_SYNCV_VBPE, c->vbpe) |
             MX_SET(GS_SYNCV_VDP, c->vdp) | MX_SET(GS_SYNCV_VS, c->vs);
  return 0;
}

// clang-format off
#define TV(k, i, hs, hbp, hfp, hseq, vs, vbp, vdp, vfp) \
  {54000000, k, 4, i, 0, hs, hbp, 2892, hfp, hseq, vs, vs, vbp, vdp, vs, vfp}
/*                                              kind          int  hs   hbp hfp hseq vs vbp vdp vfp */
const struct mx_crtc mx_crtc_ntsc   = TV(MX_CRTC_NTSC, 1, 254, 222, 64, 124, 6, 26, 480, 1);
const struct mx_crtc mx_crtc_ntsc_p = TV(MX_CRTC_NTSC, 0, 254, 222, 64, 124, 6, 26, 480, 2);
const struct mx_crtc mx_crtc_pal    = TV(MX_CRTC_PAL,  1, 254, 262, 48, 127, 5, 33, 576, 1);
const struct mx_crtc mx_crtc_pal_p  = TV(MX_CRTC_PAL,  0, 254, 262, 48, 127, 5, 33, 576, 4);

/*                                   vck        kind         spml int ffmd hs   hbp  hact  hfp  hseq vs vbpe vbp vdp  vfpe vfp */
const struct mx_crtc mx_crtc_480p   = {54000000, MX_CRTC_HD,   2, 0, 0, 128, 101, 1440,  47, 128,  6, 0, 30,  480, 0,  9};
/* 576p: in ROM 2.20's BIOS (0x53), not in ROM 2.00's; the one mode with PCK2 set */
const struct mx_crtc mx_crtc_576p   = {54000000, MX_CRTC_HD,   2, 0, 0, 128, 121, 1440,  39, 128,  5, 0, 39,  576, 0,  5};
const struct mx_crtc mx_crtc_720p   = {74250000, MX_CRTC_HD,   1, 0, 0, 104, 195, 1280,  71,  40,  5, 0, 20,  720, 0,  5};
const struct mx_crtc mx_crtc_1080i  = {74250000, MX_CRTC_HD,   1, 1, 0, 104, 131, 1920,  45,  44, 10, 2, 28, 1080, 0,  5};
const struct mx_crtc mx_crtc_vesa_1a = {50625000, MX_CRTC_VESA, 2, 0, 0, 192,  81, 1280,  47,   0,  2, 0, 33,  480, 0, 10};
const struct mx_crtc mx_crtc_vesa_1b = {63000000, MX_CRTC_VESA, 2, 0, 0,  80, 241, 1280,  63,   0,  3, 0, 28,  480, 0,  9};
const struct mx_crtc mx_crtc_vesa_1c = {63000000, MX_CRTC_VESA, 2, 0, 0, 128, 225, 1280,  47,   0,  3, 0, 16,  480, 0,  1};
const struct mx_crtc mx_crtc_vesa_2a = {72000000, MX_CRTC_VESA, 2, 0, 0, 144, 241, 1600,  63,   0,  2, 0, 22,  600, 0,  1};
const struct mx_crtc mx_crtc_vesa_2b = {79875000, MX_CRTC_VESA, 2, 0, 0, 256, 161, 1600,  95,   0,  4, 0, 23,  600, 0,  1};
const struct mx_crtc mx_crtc_vesa_2c = {99900000, MX_CRTC_VESA, 2, 0, 0, 240, 113, 1600, 127,   0,  6, 0, 23,  600, 0, 37};
const struct mx_crtc mx_crtc_vesa_2d = {99000000, MX_CRTC_VESA, 2, 0, 0, 160, 305, 1600,  47,   0,  3, 0, 21,  600, 0,  1};
const struct mx_crtc mx_crtc_vesa_3b = {130500000, MX_CRTC_VESA, 2, 0, 0, 272, 305, 2048,  63,  0,  6, 0, 29,  768, 0,  3};
const struct mx_crtc mx_crtc_vesa_3c = {75375000, MX_CRTC_VESA, 1, 0, 0, 136, 129, 1024,  39,   0,  6, 0, 29,  768, 0,  3};
const struct mx_crtc mx_crtc_vesa_3d = {78750000, MX_CRTC_VESA, 1, 0, 0,  96, 161, 1028,  27,   0,  3, 0, 28,  768, 0,  1};
const struct mx_crtc mx_crtc_vesa_3e = {94500000, MX_CRTC_VESA, 1, 0, 0,  96, 193, 1024,  63,   0,  3, 0, 36,  768, 0,  1};
const struct mx_crtc mx_crtc_vesa_4a = {108000000, MX_CRTC_VESA, 1, 0, 0, 112, 233, 1280,  63,  0,  3, 0, 38, 1024, 0,  1};
const struct mx_crtc mx_crtc_vesa_4b = {135000000, MX_CRTC_VESA, 1, 0, 0, 144, 233, 1280,  31,  0,  3, 0, 38, 1024, 0,  1};

/* the BIOS's 60 Hz modes with the line made longer to the 50 Hz total (CEA-861:
   1980 for 720p50, 2640 for 1080i50), and 1080p at 30 Hz on 1080i's clock */
const struct mx_crtc mx_crtc_720p50  = {74250000, MX_CRTC_HD,  1, 0, 0, 104, 195, 1280, 401,  40,  5, 0, 20,  720, 0,  5};
const struct mx_crtc mx_crtc_1080i50 = {74250000, MX_CRTC_HD,  1, 1, 0, 104, 131, 1920, 485,  44, 10, 2, 28, 1080, 0,  5};
const struct mx_crtc mx_crtc_1080p30 = {74250000, MX_CRTC_HD,  1, 0, 0, 104, 131, 1920,  45,  44,  5, 0, 36, 1080, 0,  4};
// clang-format on

const struct mx_crtc *mx_crtc_for_code(int code, int interlace) {
  static const struct {
    u8 code;
    const struct mx_crtc *c;
  } t[] = {
      {0x50, &mx_crtc_480p},    {0x51, &mx_crtc_1080i},   {0x52, &mx_crtc_720p},
      {0x53, &mx_crtc_576p},
      {0x1A, &mx_crtc_vesa_1a}, {0x1B, &mx_crtc_vesa_1b}, {0x1C, &mx_crtc_vesa_1c},
      {0x2A, &mx_crtc_vesa_2a}, {0x2B, &mx_crtc_vesa_2b}, {0x2C, &mx_crtc_vesa_2c},
      {0x2D, &mx_crtc_vesa_2d}, {0x3B, &mx_crtc_vesa_3b}, {0x3C, &mx_crtc_vesa_3c},
      {0x3D, &mx_crtc_vesa_3d}, {0x3E, &mx_crtc_vesa_3e}, {0x4A, &mx_crtc_vesa_4a},
      {0x4B, &mx_crtc_vesa_4b},
  };
  if (code == 0x02)
    return interlace ? &mx_crtc_ntsc : &mx_crtc_ntsc_p;
  if (code == 0x03)
    return interlace ? &mx_crtc_pal : &mx_crtc_pal_p;
  for (u32 i = 0; i < sizeof t / sizeof t[0]; i++)
    if (t[i].code == code)
      return t[i].c;
  return 0;
}
