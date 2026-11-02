/* Does GS memory read back (local -> host, mx_gs_download) as 4-bit and
   8-bit texels? The GS manual (4.3) says local -> host can't be PSMT4,
   T4HL or T4HH; PCSX2 gives wrong bytes for T4. Each format is uploaded
   to a page (zeroed as CT32 first) at TBP + 0 and read back in the same
   format: the bytes that differ, and whether the readback is the upload
   with its nibbles swapped. T8 and T8H are the control. */

#include <mx/mx.h>

static u32 pk[1024] MX_ALIGNED(64);
static u8 zero[8192] MX_ALIGNED(64);
static u8 up[4096] MX_ALIGNED(64);
static u8 back[4096] MX_ALIGNED(64);

static void send(struct mx_pkt *p) {
  mx_pkt_end(p, 0, 0);
  mx_pkt_finish(p);
  mx_pkt_writeback(p);
  u32 f0 = mx_gs.finish;
  mx_gs_send(mx_pkt_start(p));
  mx_gs_wait_finish(f0, 1);
}

static int one(const char *name, u32 psm, u32 bits) {
  const u32 page = 300, w = 64, h = 32, bw = psm == MX_GS_PSM_T4 || psm == MX_GS_PSM_T8 ? 2 : 1;
  u32 bytes = w * h * bits / 8;
  for (u32 i = 0; i < bytes; i++)
    up[i] = (u8)(i * 37 + (i >> 7) * 11 + 1);
  mx_dcache_writeback(up, bytes);
  struct mx_pkt p;
  mx_pkt_begin(&p, pk, sizeof pk, 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_gs_upload(&p, MX_GS_PAGE(page), 1, MX_GS_PSM_CT32, 0, 0, 64, 32, zero);
  mx_gs_upload(&p, MX_GS_PAGE(page), bw, psm, 0, 0, w, h, up);
  mx_gs_finish_ad(&p);
  send(&p);
  int r = mx_gs_download(back, MX_GS_PAGE(page), bw, psm, 0, 0, w, h);
  u32 bad = 0, swapped = 0;
  for (u32 i = 0; i < bytes; i++) {
    bad += back[i] != up[i];
    swapped += back[i] != (u8)(up[i] << 4 | up[i] >> 4);
  }
  mx_printf("p24 %-5s download %d: %4u of %4u bytes differ (%4u from the nibble-swapped upload); "
            "first %02x %02x %02x %02x, uploaded %02x %02x %02x %02x\n",
            name, r, bad, bytes, swapped, back[0], back[1], back[2], back[3], up[0], up[1], up[2],
            up[3]);
  return bad != 0;
}

int main(void) {
  mx_gs_init(640, 224);
  mx_dcache_writeback(zero, sizeof zero);
  one("T8", MX_GS_PSM_T8, 8);
  one("T8H", MX_GS_PSM_T8H, 8);
  int t4 = one("T4", MX_GS_PSM_T4, 4);
  int t4hl = one("T4HL", MX_GS_PSM_T4HL, 4);
  int t4hh = one("T4HH", MX_GS_PSM_T4HH, 4);
  mx_printf("p24: 4-bit readback %s\n", t4 || t4hl || t4hh ? "does not work" : "works");
  MX_TEST_END(0);
  return 0;
}
