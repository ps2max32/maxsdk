/* The IPU: commands, its two DMA channels, and waiting for it asleep
   (<mx/ipu.h>; EE User's Manual ch. 8; docs/ipu.md). */

#include <mx/cpu.h>
#include <mx/dma.h>
#include <mx/ipu.h>
#include <mx/irq.h>
#include <mx/string.h>
#include <mx/thread.h>

#define CMD(code, option) (MX_R32(MX_IPU_CMD) = (u32)(MX_IPU_##code) << 28 | (u32)(option))

static MX_CORE_DATA int sema = -1;
static u32 fed; /* quadwords given to toIPU since mx_ipu_input */
/* the command with output under way */
static struct {
  u32 total, mbq; /* quadwords fromIPU was started with, and a macroblock's; 0: no output */
} cur;
static u8 table[64] MX_ALIGNED(64) MX_CORE_BSS; /* SETIQ's and SETVQ's, sent by DMA */
static u8 direct;                               /* mx_ipu_direct */
static u8 qsc_is_scale;                         /* IDEC's QSC is taken as the scale (PCSX2) */
static u8 q_type;                               /* IPU_CTRL.QST as last written */

static MX_CORE_FN void wake(void *arg) {
  (void)arg;
  mx_isema_signal(sema);
}

/* Stop a channel under way as the manual has it (5.6: DMA held through
   D_ENABLEW while STR is cleared). */
static void stop(int ch) {
  if (!mx_dma_busy(ch))
    return;
  int was = mx_di();
  u32 en = MX_R32(MX_D_ENABLER);
  MX_R32(MX_D_ENABLEW) = en | (u32)MX_BIT(D_ENABLEW_CPND);
  MX_DMA_REG(ch, CHCR) &= ~(u32)MX_CHCR_STR;
  MX_R32(MX_D_ENABLEW) = en;
  mx_irq_restore(was);
}

void mx_ipu_reset(void) {
  stop(MX_CH_TOIPU);
  stop(MX_CH_FROMIPU);
  MX_R32(MX_IPU_CTRL) = (u32)MX_BIT(IPU_CTRL_RST);
  CMD(BCLR, 0);
  fed = 0;
  cur.total = cur.mbq = 0;
}

/* Whether IDEC takes its QSC field as the quantiser_scale_code, as the
   manual says (the scale is then twice the code, IPU_CTRL.QST 0), or as the
   scale itself, as PCSX2 does (docs/pcsx2.md): a macroblock with one
   coefficient, 2 at the first place after DC, under a matrix of 16s and QSC
   31. Its corner comes out 21 above grey for a scale of 62, 11 for 31. */
static void probe_qsc(void) {
  static u32 px[256] MX_ALIGNED(64) MX_CORE_BSS;
  /* macroblock_type 1; Y0: DC difference 0, run 0 level 2, EOB; the other
     blocks DC difference 0, EOB; a start code */
  static const u8 mb[16] = {0xC4, 0x52, 0x94, 0x88, 0x80, 0x00, 0x00, 0x01, 0xB7};
  u8 flat[64];
  memset(flat, 16, 64);
  mx_ipu_setiq_mem(0, flat);
  mx_ipu_ctrl(0);
  mx_ipu_setth(0, 0);
  memcpy(MX_UNCACHED(table), mb, 16);
  mx_ipu_input(table, 1, 0);
  qsc_is_scale = 0;
  int n = mx_ipu_idec(0, 31, 0, px, 1);
  u32 r = *MX_UNCACHED(px) & 255; /* 1.164 x (Y - 16): 155 or 143 */
  qsc_is_scale = n == 1 && r < 149;
  mx_ipu_input_end();
}

int mx_ipu_qsc_is_scale(void) { return qsc_is_scale; }

int mx_ipu_init(void) {
  if (sema < 0)
    sema = mx_sema_create(0, 1);
  if (sema < 0)
    return -1;
  MX_R32(MX_D_CTRL) |= (u32)MX_BIT(D_CTRL_DMAE);
  mx_ipu_reset();
  mx_irq_intc(MX_INTC_IPU, wake, 0);
  mx_irq_dmac(MX_CH_FROMIPU, wake, 0);
  mx_irq_dmac(MX_CH_TOIPU, wake, 0);
  probe_qsc();
  return 0;
}

void mx_ipu_exit(void) {
  direct = 0;
  mx_ipu_reset();
  mx_irq_intc(MX_INTC_IPU, 0, 0);
  mx_irq_dmac(MX_CH_FROMIPU, 0, 0);
  mx_irq_dmac(MX_CH_TOIPU, 0, 0);
  if (sema >= 0)
    mx_sema_delete(sema);
  sema = -1;
}

void mx_ipu_direct(int on) {
  if (sema < 0 || direct == !!on)
    return;
  direct = !!on;
  mx_irq_intc(MX_INTC_IPU, on ? 0 : wake, 0);
  mx_irq_dmac(MX_CH_FROMIPU, on ? 0 : wake, 0);
  mx_irq_dmac(MX_CH_TOIPU, on ? 0 : wake, 0);
}

u32 mx_ipu_result(void) { return (u32)MX_R64(MX_IPU_CMD); }

void mx_ipu_ctrl(u32 bits) {
  q_type = (u8)MX_GET(IPU_CTRL_QST, bits);
  MX_R32(MX_IPU_CTRL) =
      bits & (u32)(MX_BIT(IPU_CTRL_IDP) | MX_BIT(IPU_CTRL_AS) | MX_BIT(IPU_CTRL_IVF) |
                   MX_BIT(IPU_CTRL_QST) | MX_BIT(IPU_CTRL_MP1) | MX_BIT(IPU_CTRL_PCT));
}

/* ---- waiting ----------------------------------------------------------- */

/* Until the command has ended. While the IPU and the channels it needs are
   at work an interrupt will come (the command's end, or a channel's): sleep
   on it. Once the input's transfer is over, or the output's, the command
   may never end and nothing would wake us: the IPU gets 2 ms to end with
   what it holds, looked at every 100 us. */
static int wait_cmd(int out) {
  int idle = 0;
  if (direct) {
    /* a command of microseconds: looked at until it has ended; if it has
       not by then it waits for the stream or its output */
    for (int i = 0; i < 2000; i++)
      if (!(MX_R32(MX_IPU_CTRL) & MX_BIT(IPU_CTRL_BUSY)))
        return MX_IPU_OK;
  }
  for (;;) {
    u32 c = MX_R32(MX_IPU_CTRL);
    if (!(c & MX_BIT(IPU_CTRL_BUSY)))
      return MX_IPU_OK;
    /* not "and the FIFO is empty": on the PS2 a command can wait for more
       with quadwords still counted in IFC (docs/ipu.md) */
    int in_idle = !mx_dma_busy(MX_CH_TOIPU);
    int out_idle = out && !mx_dma_busy(MX_CH_FROMIPU);
    if (in_idle || out_idle) {
      if (++idle > 20)
        return in_idle && !(out_idle && MX_GET(IPU_CTRL_OFC, c)) ? MX_IPU_STARVED : MX_IPU_FULL;
      mx_sleep_us(100);
      continue;
    }
    idle = 0;
    if (direct)
      mx_sleep_us(100);
    else
      mx_sema_wait(sema);
  }
}

/* The end of a command with output: what it wrote, in macroblocks. */
static int finish(void) {
  int r = wait_cmd(1);
  if (r == MX_IPU_STARVED)
    return r;
  if (r == MX_IPU_FULL) {
    mx_ipu_reset();
    return r;
  }
  /* the last quadwords, at most the FIFO's 8, are on their way */
  for (int i = 0; i < 4096 && mx_dma_busy(MX_CH_FROMIPU) && MX_GET(IPU_CTRL_OFC, MX_R32(MX_IPU_CTRL)); i++)
    ;
  stop(MX_CH_FROMIPU);
  u32 left = MX_DMA_REG(MX_CH_FROMIPU, QWC) & 0xFFFFu;
  u32 n = (cur.total - left) / cur.mbq;
  cur.total = 0;
  return (int)n;
}

int mx_ipu_wait(void) { return cur.total ? finish() : wait_cmd(0); }

/* ---- the bit stream ---------------------------------------------------- */

static void send(const void *mem, u32 qwc) {
  MX_DMA_REG(MX_CH_TOIPU, MADR) = mx_paddr_range(mem, qwc * 16).v;
  MX_DMA_REG(MX_CH_TOIPU, QWC) = qwc;
  mx_sync();
  MX_DMA_REG(MX_CH_TOIPU, CHCR) = MX_CHCR_NORMAL | MX_CHCR_FROM_MEM | MX_CHCR_STR;
}

void mx_ipu_input(const void *bs, u32 qwc, u32 bp) {
  stop(MX_CH_TOIPU);
  CMD(BCLR, bp & 127);
  fed = qwc;
  send(bs, qwc);
}

void mx_ipu_input_end(void) {
  stop(MX_CH_TOIPU);
  CMD(BCLR, 0);
  fed = 0;
}

void mx_ipu_more(const void *bs, u32 qwc) {
  while (mx_dma_busy(MX_CH_TOIPU)) {
    if (direct)
      mx_sleep_us(100);
    else
      mx_sema_wait(sema);
  }
  fed += qwc;
  send(bs, qwc);
}

u32 mx_ipu_pos(void) {
  u32 bp = MX_R32(MX_IPU_BP);
  u32 left = (MX_DMA_REG(MX_CH_TOIPU, QWC) & 0xFFFFu) + (u32)MX_GET(IPU_BP_IFC, bp) +
             (u32)MX_GET(IPU_BP_FP, bp);
  return (fed - left) * 128 + (u32)MX_GET(IPU_BP_BP, bp);
}

u32 mx_ipu_top(void) { return (u32)MX_R64(MX_IPU_TOP); }

u32 mx_ipu_fdec(u32 fb) {
  CMD(FDEC, fb & 63);
  if (wait_cmd(0))
    return 0;
  return (u32)MX_R64(MX_IPU_CMD);
}

u32 mx_ipu_vdec(u32 tbl, u32 fb) {
  CMD(VDEC, (tbl & 3) << 26 | (fb & 63));
  if (wait_cmd(0))
    return 0;
  return (u32)MX_R64(MX_IPU_CMD);
}

/* ---- tables ------------------------------------------------------------ */

void mx_ipu_setiq(int nonintra, u32 fb) {
  CMD(SETIQ, (nonintra ? 1u << 27 : 0) | (fb & 63));
  wait_cmd(0);
}

void mx_ipu_setiq_mem(int nonintra, const u8 *m) {
  u8 *t = MX_UNCACHED(table);
  memcpy(t, m, 64);
  mx_ipu_input(table, 4, 0);
  mx_ipu_setiq(nonintra, 0);
}

void mx_ipu_setvq(const u16 *clut) {
  u8 *t = MX_UNCACHED(table);
  memcpy(t, clut, 32);
  mx_ipu_input(table, 2, 0);
  CMD(SETVQ, 0);
  wait_cmd(0);
}

void mx_ipu_setth(u32 th0, u32 th1) {
  CMD(SETTH, (th1 & 0x1FF) << 16 | (th0 & 0x1FF));
  wait_cmd(0);
}

/* ---- decoding ---------------------------------------------------------- */

static void output(void *out, u32 mbs, u32 mbq) {
  cur.mbq = mbq;
  cur.total = mbs * mbq;
  mx_dma_recv(MX_CH_FROMIPU, out, cur.total);
}

/* ISO 13818-2, table 7-6: the scale of a code with q_scale_type 1 */
static const u8 nonlinear[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12, 14, 16, 18,  20,  22,
                                 24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

int mx_ipu_idec(u32 fb, u32 qsc, u32 flags, void *out, u32 mbs) {
  if (qsc_is_scale) { /* the field is 5 bits: larger scales cannot be given */
    qsc = q_type ? nonlinear[qsc & 31] : (qsc & 31) << 1;
    if (qsc > 31)
      qsc = 31;
  }
  output(out, mbs, flags & MX_IPU_RGB16 ? MX_IPU_MB_RGB16 : MX_IPU_MB_RGB32);
  CMD(IDEC, (flags & 0x0F000000u) | (qsc & 31) << 16 | (fb & 63));
  return finish();
}

void mx_ipu_bdec_start(u32 fb, u32 qsc, u32 flags, void *out) {
  output(out, 1, MX_IPU_MB_RAW16);
  CMD(BDEC, (flags & 0x0E000000u) | (qsc & 31) << 16 | (fb & 63));
}

int mx_ipu_bdec(u32 fb, u32 qsc, u32 flags, void *out) {
  mx_ipu_bdec_start(fb, qsc, flags, out);
  return finish();
}

int mx_ipu_csc(const void *raw8, u32 mbs, u32 flags, void *out) {
  mx_ipu_input(raw8, mbs * MX_IPU_MB_RAW8, 0);
  output(out, mbs, flags & MX_IPU_RGB16 ? MX_IPU_MB_RGB16 : MX_IPU_MB_RGB32);
  CMD(CSC, (flags & 0x0C000000u) | (mbs & 0x7FF));
  return finish();
}

int mx_ipu_pack(const void *rgb32, u32 mbs, u32 flags, void *out) {
  mx_ipu_input(rgb32, mbs * MX_IPU_MB_RGB32, 0);
  output(out, mbs, flags & MX_IPU_PACK_RGB16 ? MX_IPU_MB_RGB16 : MX_IPU_MB_INDX4);
  CMD(PACK, (flags & 0x0C000000u) | (mbs & 0x7FF));
  return finish();
}
