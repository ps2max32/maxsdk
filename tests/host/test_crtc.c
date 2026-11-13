/* <mx/crtc.h>: the descriptions of the BIOS's modes give the register values its
   SetGsCrt writes (the last SMODE1 write; a SCPH-70004's BIOS code run in an
   emulator). */

#include <mx/bits.h>
#include <mx/crtc.h>
#include <mx/hw/regs.h>
#include "test.h"

struct ref {
  const struct mx_crtc *c;
  unsigned long long smode1, smode2, srfsh, synch1, synch2, syncv;
};

// clang-format off
static const struct ref bios[] = {
    {&mx_crtc_ntsc, 0x742814504ull, 0x1ull, 0x8ull, 0x7f5b61f06f040ull, 0x33a4d8ull, 0xc7800601a01801ull},
    {&mx_crtc_ntsc_p, 0x742814504ull, 0x0ull, 0x8ull, 0x7f5b61f06f040ull, 0x33a4d8ull, 0xc7800601a01802ull},
    {&mx_crtc_pal, 0x742816504ull, 0x1ull, 0x8ull, 0x7f5c21fc83030ull, 0x3484bcull, 0xa9000502101401ull},
    {&mx_crtc_pal_p, 0x742816504ull, 0x0ull, 0x8ull, 0x7f5c21fc83030ull, 0x3484bcull, 0xa9000502101404ull},
    {&mx_crtc_480p, 0x1742410504ull, 0x0ull, 0x4ull, 0x402da2003282full, 0x195a75ull, 0xc7800001e00009ull},
    /* ROM 2.20's (a SCPH-90006's BIOS code, read the same way) */
    {&mx_crtc_576p, 0x1742490504ull, 0x0ull, 0x4ull, 0x402e02003c827ull, 0x19ca67ull, 0xa9000002700005ull},
    {&mx_crtc_720p, 0x13422004b2ull, 0x0ull, 0x4ull, 0x343110a061847ull, 0x17920eull, 0xab400001400005ull},
    {&mx_crtc_1080i, 0x3422004b2ull, 0x1ull, 0x4ull, 0x344200b04182dull, 0x20fb61ull, 0x150e00201c00005ull},
    {&mx_crtc_vesa_1a, 0x134040047aull, 0x0ull, 0x4ull, 0x602603002882full, 0x178a0full, 0x4780000210000aull},
    {&mx_crtc_vesa_1b, 0x13404004e3ull, 0x0ull, 0x4ull, 0x282f01407883full, 0x1809ffull, 0x67800001c00009ull},
    {&mx_crtc_vesa_1c, 0x13404004e3ull, 0x0ull, 0x4ull, 0x402c82007082full, 0x18c9e7ull, 0x67800001000001ull},
    {&mx_crtc_vesa_2a, 0x1340400083ull, 0x0ull, 0x4ull, 0x483702407883full, 0x1e0a7full, 0x49600001600001ull},
    {&mx_crtc_vesa_2b, 0x134040063eull, 0x0ull, 0x4ull, 0x803204005085full, 0x1e0a7full, 0x89600001700001ull},
    {&mx_crtc_vesa_2c, 0x1340400655ull, 0x0ull, 0x4ull, 0x783203c03887full, 0x1c8aafull, 0xc9600001700025ull},
    {&mx_crtc_vesa_2d, 0x1340400563ull, 0x0ull, 0x4ull, 0x503802809882full, 0x1f8a4full, 0x69600001500001ull},
    {&mx_crtc_vesa_3b, 0x13404000ebull, 0x0ull, 0x4ull, 0x884304409883full, 0x280affull, 0xcc000001d00003ull},
    {&mx_crtc_vesa_3c, 0x134020061eull, 0x0ull, 0x2ull, 0x4421022040827ull, 0x13898full, 0xcc000001d00003ull},
    {&mx_crtc_vesa_3d, 0x134020051bull, 0x0ull, 0x2ull, 0x302301805081bull, 0x13a98full, 0x6c000001c00001ull},
    {&mx_crtc_vesa_3e, 0x1340200039ull, 0x0ull, 0x2ull, 0x302501806083full, 0x13898full, 0x6c000002400001ull},
    {&mx_crtc_vesa_4a, 0x1340200041ull, 0x0ull, 0x2ull, 0x382dc1c07483full, 0x1869f3ull, 0x70000002600001ull},
    {&mx_crtc_vesa_4b, 0x1340200051ull, 0x0ull, 0x2ull, 0x482bc2407481full, 0x1969d3ull, 0x70000002600001ull},
};
// clang-format on

void test_crtc(void) {
  struct mx_crtc_regs r;
  for (unsigned i = 0; i < sizeof bios / sizeof bios[0]; i++) {
    const struct ref *b = &bios[i];
    CHECK_EQ(mx_crtc_regs(b->c, &r), 0);
    CHECK_EQ(r.smode1, b->smode1);
    CHECK_EQ(r.smode2, b->smode2);
    CHECK_EQ(r.srfsh, b->srfsh);
    CHECK_EQ(r.synch1, b->synch1);
    CHECK_EQ(r.synch2, b->synch2);
    CHECK_EQ(r.syncv, b->syncv);
    CHECK_EQ(mx_crtc_vck(r.smode1), b->c->vck_hz);
  }

  /* the modes the BIOS does not have: the CEA-861 line and frame totals */
  const struct mx_crtc *own[] = {&mx_crtc_720p50, &mx_crtc_1080i50, &mx_crtc_1080p30};
  const unsigned htotal[] = {1980, 2640, 2200}, vtotal[] = {750, 1125, 1125};
  for (unsigned i = 0; i < 3; i++) {
    const struct mx_crtc *c = own[i];
    CHECK_EQ(mx_crtc_regs(c, &r), 0);
    CHECK_EQ(c->hs + c->hbp + c->hact + c->hfp, htotal[i]);
    CHECK_EQ(c->vs + c->vbpe + c->vbp + c->vdp + c->vfpe + c->vfp, vtotal[i]);
    CHECK_EQ(mx_crtc_vck(r.smode1), 74250000);
  }

  /* a clock no BIOS mode has: the PLL search finds it */
  struct mx_crtc c = mx_crtc_1080p30;
  c.vck_hz = 148500000;
  CHECK_EQ(mx_crtc_regs(&c, &r), 0);
  CHECK_EQ(mx_crtc_vck(r.smode1), 148500000);
  CHECK_EQ(MX_GET(GS_SRFSH_RFSH, r.srfsh), 2);

  /* a line too short for its sync and back porch is refused */
  c.hact = 100;
  CHECK_EQ(mx_crtc_regs(&c, &r), -1);
}
