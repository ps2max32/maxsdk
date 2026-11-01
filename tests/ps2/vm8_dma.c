/* VM step 8: DMA from and to VM (<mx/vm.h>, <mx/dma.h>) with a swap file
   on host: and every frame in use, so pins page in and the gather moves
   pages out. 2 MiB of frames (64 pages). About a minute on the PS2; at the
   end the screen is bright green with 900D0000 in slot 0, or bright red
   with BAD000nn.

   Slot 0 and the colour: the step. Slot 1: the part's own progress, slot
   2: words wrong in the last check, slot 3: mx__vm_check after each part
   (0: the tables and the TLB consistent). Slots 4-7: swap ins, swap outs, page
   faults, pinned pages; slot 8: where the slow path is (1 entered, 2 has
   the lock, 3 done, 0 back), slot 9: the last swap operation (op << 31 |
   slot << 16 | frame << 4 | 1 sought, 2 done). Heartbeat in slots 30 and
   31.
   Slots 28 and 29 (if not blank): the VM, or a wait for the IOP, found
   interrupts off where they must be on (where << 24 | Status; see
   mx__vm_irq_lost in src/vm/vm.c).
     1 mx_vm_init with host:vm8swap.bin, then 128 pages written (the frames
       full, half the pages in the file)
     2 12 KiB over a page boundary to the scratchpad and back (pinned for
       writing, sent a piece at a time)
     3 a VIF1 packet over two page boundaries (UNPACK to VU1 memory), sent
       as REF tags a piece
     4 a GIF IMAGE packet over a page boundary, read back from GS memory
     5 72000 bytes over three pages pinned in one physical piece
       (MX_VM_PIN_DMA, the gather), sent by a chain's REF
     6 a DMA queue segment pinned in one piece, unpinned when it has
       passed (mx_vm_unpin_after)
     7 host: file written from VM and read into VM */

#include "t.h"
#include <mx/sif.h>
#include <mx/vm.h>

#define PAGE MX_VM_PAGE

extern volatile u32 mx__vm_where, mx__vm_swaplog[8], mx__vm_swapn;
u32 mx__vm_check(void);

static u32 val(u32 a) { return a * 2654435761u ^ 0x5a5a5a5au; }
static u8 *vmp(u32 off) { return (u8 *)mx_vm_base() + off; }

static void stats(void) {
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_dbg_word(4, s.swap_ins);
  mx_dbg_word(5, s.swap_outs);
  mx_dbg_word(6, s.faults);
  mx_dbg_word(7, s.pinned);
  mx_dbg_word(8, mx__vm_where);
  mx_dbg_word(9, mx__vm_swaplog[(mx__vm_swapn - 1) & 7]);
}

static void step(u32 n, const char *what) {
  stats();
  mx_dbg_step(n);
  mx_printf("vm8: %u %s\n", n, what);
}

static void sub(u32 n) {
  mx_dbg_word(1, n);
  stats();
}

static u32 wrong(u32 bad) {
  mx_dbg_word(2, bad);
  return bad;
}

/* Pages [a, a + n) made resident in reverse order, so their frames are not
   in a row. */
static void scatter(u32 a, u32 n) {
  for (u32 i = n; i-- > 0;)
    *(volatile u32 *)vmp((a + i) * PAGE) += 0;
}

#define SPR ((u32 *)0x70000000u)

static void spr_sg(void) {
  u32 at = 40 * PAGE - 4096, n = 12288;
  scatter(39, 3);
  sub(1);
  CHECK(mx_vm_pin(vmp(at), n, MX_VM_PIN_WRITE) == 0);
  struct mx_vm_piece pc[4];
  u32 k = mx_vm_pieces(vmp(at), n, pc, 4);
  CHECK(k >= 2);
  u32 *s = (u32 *)vmp(at);
  for (u32 i = 0; i < n / 4; i++)
    s[i] = val(i) + 1;
  mx_dcache_writeback(s, n);
  sub(2);
  mx_dma_to_spr(SPR, s, n / 16);
  mx_dma_wait(MX_CH_TOSPR);
  u32 bad = 0;
  for (u32 i = 0; i < n / 4; i++)
    bad += SPR[i] != val(i) + 1;
  CHECK_EQ(wrong(bad), 0);
  for (u32 i = 0; i < n / 4; i++)
    SPR[i] = val(i) + 2;
  mx_dcache_writeback(s, n);
  mx_dcache_invalidate(s, n);
  sub(3);
  mx_dma_from_spr(s, SPR, n / 16);
  mx_dma_wait(MX_CH_FROMSPR);
  mx_dcache_invalidate(s, n);
  bad = 0;
  for (u32 i = 0; i < n / 4; i++)
    bad += s[i] != val(i) + 2;
  CHECK_EQ(wrong(bad), 0);
  mx_vm_unpin(vmp(at), n);
  mx_printf("vm8: %u pieces\n", k);
}

static void vif_sg(void) {
  u32 at = 50 * PAGE - 1024, nq = 200, qwc = nq + 1; /* the codes, then the data */
  scatter(49, 3);
  u32 *p = (u32 *)vmp(at);
  sub(1);
  CHECK(mx_vm_pin(p, qwc * 16, 0) == 0);
  p[0] = mx_vif_stcycl(1, 1);
  p[1] = 0;
  p[2] = 0;
  p[3] = mx_vif_unpack(MX_UNPACK_V4_32, 0, nq, 0);
  for (u32 i = 0; i < nq * 4; i++)
    p[4 + i] = val(i) ^ 0x1111;
  mx_dcache_writeback(p, qwc * 16);
  sub(2);
  mx_dma_send(MX_CH_VIF1, p, qwc);
  CHECK(mx_dma_wait(MX_CH_VIF1) == 0);
  const volatile u32 *vu = (const volatile u32 *)MX_VU1_DATA;
  u32 bad = 0;
  for (u32 i = 0; i < nq * 4; i++)
    bad += vu[i] != (val(i) ^ 0x1111);
  CHECK_EQ(wrong(bad), 0);
  mx_vm_unpin(p, qwc * 16);
}

static u32 back[64 * 64] MX_ALIGNED(64);

static void gif_sg(void) {
  u32 w = 64, h = 64, dbp = 0x3000, iq = w * h * 4 / 16;
  u32 at = 60 * PAGE - 2048, qwc = 6 + iq;
  scatter(59, 3);
  u64 *q = (u64 *)vmp(at);
  sub(1);
  CHECK(mx_vm_pin(q, qwc * 16, 0) == 0);
  q[0] = mx_giftag(4, 0, -1, MX_GIF_FLG_PACKED, 1), q[1] = MX_GIF_REG_AD;
  q[2] = mx_gs_bitbltbuf(0, 0, 0, dbp, 1, MX_GS_PSM_CT32), q[3] = MX_GS_REG_BITBLTBUF;
  q[4] = mx_gs_trxpos(0, 0, 0, 0, 0), q[5] = MX_GS_REG_TRXPOS;
  q[6] = mx_gs_trxreg(w, h), q[7] = MX_GS_REG_TRXREG;
  q[8] = 0, q[9] = MX_GS_REG_TRXDIR;
  q[10] = mx_giftag(iq, 1, -1, MX_GIF_FLG_IMAGE, 0), q[11] = 0;
  u32 *d = (u32 *)&q[12];
  for (u32 i = 0; i < w * h; i++)
    d[i] = val(i) | 0x80000000u;
  mx_dcache_writeback(q, qwc * 16);
  sub(2);
  mx_dma_send(MX_CH_GIF, q, qwc);
  CHECK(mx_dma_wait(MX_CH_GIF) == 0);
  sub(3);
  CHECK(mx_gs_download(back, dbp, 1, MX_GS_PSM_CT32, 0, 0, w, h) == 0);
  u32 bad = 0;
  for (u32 i = 0; i < w * h; i++)
    bad += back[i] != (val(i) | 0x80000000u);
  CHECK_EQ(wrong(bad), 0);
  mx_vm_unpin(q, qwc * 16);
}

static u64 tags[8][2] MX_ALIGNED(64);

static void pin_dma(void) {
  u32 at = 70 * PAGE + 512, nq = 4500; /* 72000 bytes over three pages */
  scatter(70, 3);
  u32 *p = (u32 *)vmp(at);
  for (u32 i = 0; i < nq * 4; i++)
    p[i] = val(i) ^ 0x2222;
  sub(1);
  CHECK(mx_vm_pin(p, nq * 16, MX_VM_PIN_DMA) == 0);
  sub(2);
  struct mx_vm_piece pc[4];
  CHECK_EQ(mx_vm_pieces(p, nq * 16, pc, 4), 1);
  u32 bad = 0;
  for (u32 i = 0; i < nq * 4; i++)
    bad += p[i] != (val(i) ^ 0x2222);
  CHECK_EQ(wrong(bad), 0);
  mx_dcache_writeback(p, nq * 16);
  u64 *c = &tags[0][0];
  c = mx_e_ref(c, p, 250, mx_vif_stcycl(1, 1), mx_vif_unpack(MX_UNPACK_V4_32, 0, 250, 0));
  c = mx_e_end(c, 0, 0, 0);
  mx_dcache_writeback(tags, sizeof tags);
  sub(3);
  mx_dma_chain(MX_CH_VIF1, tags, 1);
  CHECK(mx_dma_wait(MX_CH_VIF1) == 0);
  const volatile u32 *vu = (const volatile u32 *)MX_VU1_DATA;
  bad = 0;
  for (u32 i = 0; i < 250 * 4; i++)
    bad += vu[i] != (val(i) ^ 0x2222);
  CHECK_EQ(wrong(bad), 0);
  mx_vm_unpin(p, nq * 16);
}

static void queue_unpin(void) {
  u32 at = 100 * PAGE + 4096, qwc = 3000; /* 48000 bytes over two pages */
  scatter(100, 3);
  u32 *p = (u32 *)vmp(at);
  sub(1);
  CHECK(mx_vm_pin(p, qwc * 16, MX_VM_PIN_DMA) == 0);
  p[0] = mx_vif_stcycl(1, 1), p[1] = 0, p[2] = 0;
  p[3] = mx_vif_unpack(MX_UNPACK_V4_32, 0, 255, 0);
  for (u32 i = 4; i < 256 * 4; i++)
    p[i] = val(i) ^ 0x3333;
  for (u32 i = 256 * 4; i < qwc * 4; i++)
    p[i] = 0; /* VIF NOPs */
  mx_dcache_writeback(p, qwc * 16);
  struct mx_vm_stats s0, s1;
  mx_vm_stats(&s0);
  static struct mx_dmaq q;
  mx_dmaq_init(&q, MX_CH_VIF1);
  sub(2);
  mx_event e = mx_dmaq_send(&q, p, qwc);
  mx_vm_unpin_after(p, qwc * 16, e);
  mx_event_wait(e);
  sub(3);
  CHECK_EQ(mx_vm_unpin_done(), 0);
  mx_vm_stats(&s1);
  CHECK_EQ(s0.pinned - s1.pinned, (at + qwc * 16 - 1) / PAGE - at / PAGE + 1);
  mx_dmaq_release(&q);
  const volatile u32 *vu = (const volatile u32 *)MX_VU1_DATA;
  u32 bad = 0;
  for (u32 i = 0; i < 255 * 4; i++)
    bad += vu[i] != (val(i + 4) ^ 0x3333);
  CHECK_EQ(wrong(bad), 0);
}

static void fio(void) {
  u32 n = 40000;
  u8 *src = vmp(80 * PAGE + 5), *dst = vmp(90 * PAGE + 3);
  scatter(80, 3);
  for (u32 i = 0; i < n; i++)
    src[i] = (u8)(val(i) >> 7);
  sub(1);
  int fd = mx_fio_open("host:vm8fio.bin", MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  CHECK(mx_fio_write(fd, src, n) == 0);
  mx_fio_close(fd);
  sub(2);
  fd = mx_fio_open("host:vm8fio.bin", MX_FIO_RDONLY);
  CHECK(fd >= 0);
  CHECK_EQ(mx_fio_read(fd, dst, n), n);
  mx_fio_close(fd);
  u32 bad = 0;
  for (u32 i = 0; i < n; i++)
    bad += dst[i] != (u8)(val(i) >> 7);
  CHECK_EQ(wrong(bad), 0);
}

int main(void) {
  mx_dbg_screen();
  mx_dbg_heartbeat(1);

  step(1, "mx_vm_init, 128 pages written");
  static const struct mx_vm_config cfg = {.swap = "host:vm8swap.bin", .frames = 64 * PAGE};
  int r = mx_vm_init(&cfg);
  CHECK_EQ(r, 0);
  if (r)
    return t_done("vm8");
  for (u32 k = 0; k < 128; k++) {
    sub(k);
    for (u32 i = 0; i < PAGE; i += 64)
      *(u32 *)vmp(k * PAGE + i) = val(k * PAGE + i);
  }
  u32 bad = 0;
  for (u32 k = 0; k < 128; k++) {
    sub(k);
    for (u32 i = 0; i < PAGE; i += 64)
      bad += *(u32 *)vmp(k * PAGE + i) != val(k * PAGE + i);
  }
  CHECK_EQ(wrong(bad), 0);

  static const struct {
    const char *name;
    void (*fn)(void);
  } parts[] = {{"spr_sg", spr_sg}, {"vif_sg", vif_sg},           {"gif_sg", gif_sg},
               {"pin_dma", pin_dma}, {"queue_unpin", queue_unpin}, {"fio", fio}};
  for (u32 i = 0; i < sizeof parts / sizeof parts[0]; i++) {
    step(2 + i, parts[i].name);
    parts[i].fn();
    u32 c = mx__vm_check();
    mx_dbg_word(3, c);
    if (c)
      mx_printf("vm8: after %s, mx__vm_check %08x\n", parts[i].name, c);
    CHECK_EQ(c, 0);
  }

  stats();
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_printf("vm8: %u swap ins, %u outs, %u drops, %u faults, %u retries, %u pinned\n",
            s.swap_ins, s.swap_outs, s.drops, s.faults, s.retries, s.pinned);
  CHECK_EQ(s.pinned, 0);
  mx_dbg_heartbeat(0);
  return t_done("vm8");
}
