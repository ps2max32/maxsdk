/* mxdev9: the expansion bay's CXD9611 and its SPEED chip (docs/iop-drivers.md).

   Power, the SPEED's interrupts, DMA on IOP channel 8 and the EEPROM's MAC.
   DMA requests are queued and started from the channel's interrupt, so no
   thread waits for a transfer and nothing spins. The register sequences
   follow ps2sdk's dev9 driver (iop/dev9/dev9, Academic Free License 2.0:
   ../LICENSE). */
#include "irx_imports.h"
#include <dev9regs.h>
#include <speedregs.h>
#include <mxdev9.h>

IRX_ID("mxdev9", 1, 1);

extern struct irx_export_table _exp_mxdev9;

#define SSBUS_R_1418 0xbf801418
#define SSBUS_R_141c 0xbf80141c
#define SSBUS_R_1420 0xbf801420
#define CHCR_30 (1u << 30)
#define CHCR_TR (1u << 24)
#define CHCR_CO (1u << 9)
#define CHCR_DR (1u << 0) /* memory to device */
#define DPCR2_DEV9 0x80u

static volatile iop_dmac_chan_t *const chan = (iop_dmac_chan_t *)DEV9_DMAC_BASE;

static u16 spd_rev, caps;
static int dvr;
static u8 mac[6];
static int mac_ok;

#define HANDLERS 4
static struct {
  u32 mask;
  mxdev9_intr_fn *fn;
  void *arg;
} intr[HANDLERS];
static u32 mask_now; /* SPD_R_INTR_MASK, which only mxdev9 writes */

static struct mxdev9_dma *head, *tail;
static int busy; /* a transfer or its done is running */
static u32 n_dma, n_queued, depth, max_depth, n_irq, n_rounds, n_spins;

static void (*off_fn[4])(void);

/* ---- interrupts */

static void set_mask(u32 m) {
  USE_SPD_REGS;
  if ((m & 0xffff) == mask_now)
    return;
  mask_now = m & 0xffff;
  SPD_REG16(SPD_R_INTR_MASK) = (u16)mask_now;
}

static int dev9_irq(void *unused) {
  USE_DEV9_REGS;
  USE_SPD_REGS;
  (void)unused;
  n_irq++;
  for (int round = 0; round < 16; round++) {
    u32 s = SPD_REG16(SPD_R_INTR_STAT) & mask_now;
    if (!s)
      break;
    n_rounds++;
    u32 left = s;
    for (int i = 0; i < HANDLERS; i++)
      if (s & intr[i].mask) {
        intr[i].fn(intr[i].arg, s & intr[i].mask);
        left &= ~intr[i].mask;
      }
    if (left)
      set_mask(mask_now & ~left); /* nobody's: off */
  }
  if (!dvr)
    DEV9_REG(DEV9_R_1466) = 1;
  DEV9_REG(DEV9_R_1466) = 0;
  return 1;
}

int mxdev9_intr(u32 mask, mxdev9_intr_fn *fn, void *arg) {
  int st, r = -1;
  CpuSuspendIntr(&st);
  for (int i = 0; i < HANDLERS; i++)
    if (!intr[i].mask) {
      intr[i].fn = fn;
      intr[i].arg = arg;
      intr[i].mask = mask;
      r = 0;
      break;
    }
  CpuResumeIntr(st);
  return r;
}

void mxdev9_intr_on(u32 mask) {
  int st;
  CpuSuspendIntr(&st);
  set_mask(mask_now | mask);
  CpuResumeIntr(st);
}

void mxdev9_intr_off(u32 mask) {
  int st;
  CpuSuspendIntr(&st);
  set_mask(mask_now & ~mask);
  CpuResumeIntr(st);
}

/* ---- DMA */

/* Channel 8's interrupt: off until the first queued request (mxdev9_dma), then
   on for good; a short transfer's end (mxdev9_dma_begin) is then ignored. Only
   through intrman: writing DICR2 here, once a frame, spoilt other channels'
   transfers (a memory card write's last bytes, sent over SIF). */
static int dma_irq_enabled;

/* interrupts off */
static void dma_go(struct mxdev9_dma *d) {
  USE_SPD_REGS;
  SPD_REG16(SPD_R_DMA_CTRL) = spd_rev < 17 ? (d->device & 3) | 4 : d->device | 6;
  if (d->start)
    d->start(d);
  chan->madr = (u32)d->buf;
  chan->bcr = (u32)d->blocks << 16 | d->block_words;
  chan->chcr = CHCR_30 | CHCR_TR | CHCR_CO | (d->to_mem ? 0 : CHCR_DR);
}

static int dma_irq(void *unused) {
  (void)unused;
  struct mxdev9_dma *d = head;
  if (busy != 1 || !d)
    return 1; /* a short transfer's end, which mxdev9_dma_end waits for */
  head = d->next;
  if (!head)
    tail = 0;
  depth--;
  if (d->done)
    d->done(d); /* busy is still set: what it queues waits for the line below */
  if (head)
    dma_go(head);
  else
    busy = 0;
  return 1;
}

void mxdev9_dma(struct mxdev9_dma *d) {
  int st;
  CpuSuspendIntr(&st);
  d->next = 0;
  if (tail)
    tail->next = d;
  else
    head = d;
  tail = d;
  n_dma++;
  if (++depth > max_depth)
    max_depth = depth;
  if (!dma_irq_enabled) {
    EnableIntr(IOP_IRQ_DMA_DEV9);
    dma_irq_enabled = 1;
  }
  if (!busy) {
    busy = 1;
    dma_go(d);
  } else
    n_queued++;
  CpuResumeIntr(st);
}

int mxdev9_dma_begin(struct mxdev9_dma *d) {
  int st;
  CpuSuspendIntr(&st);
  if (busy) {
    CpuResumeIntr(st);
    return -1;
  }
  busy = 2;
  n_dma++;
  dma_go(d);
  CpuResumeIntr(st);
  return 0;
}

void mxdev9_dma_end(struct mxdev9_dma *d) {
  (void)d;
  for (u32 n = 0; chan->chcr & CHCR_TR && n < 10000000; n++)
    n_spins++;
  int st;
  CpuSuspendIntr(&st);
  if (head) {
    busy = 1;
    dma_go(head);
  } else
    busy = 0;
  CpuResumeIntr(st);
}

void mxdev9_stats(u32 *out6) {
  out6[0] = n_dma;
  out6[1] = n_queued;
  out6[2] = max_depth;
  out6[3] = n_irq;
  out6[4] = n_rounds;
  out6[5] = n_spins;
}

/* ---- the EEPROM (93C46-like, bit-banged on the SPEED's PIO pins) */

static void pio(u8 v) {
  USE_SPD_REGS;
  SPD_REG8(SPD_R_PIO_DATA) = v;
  DelayThread(1);
}

static int read_eeprom(u16 *out4) {
  USE_SPD_REGS;
  int res = -1;
  SPD_REG8(SPD_R_PIO_DIR) = 0xe0 | (dvr ? 7 : 1);
  DelayThread(1);
  pio(0x80);
  for (int i = 0; i < 2; i++)
    pio(0xa0), pio(0xe0);
  for (int i = 0; i < 7; i++)
    pio(0x80), pio(0xc0);
  pio(0xc0);
  u8 v = SPD_REG8(SPD_R_PIO_DATA);
  DelayThread(1);
  if (v & 0x10) {
    pio(0);
    goto out;
  }
  pio(0x80);
  for (int i = 0; i < 4; i++) {
    u16 w = 0;
    for (int j = 15; j >= 0; j--) {
      pio(0xc0);
      if (SPD_REG8(SPD_R_PIO_DATA) & 0x10)
        w |= 1u << j;
      pio(0x80);
    }
    out4[i] = w;
  }
  pio(0);
  res = 0;
out:
  SPD_REG8(SPD_R_PIO_DIR) = dvr ? 7 : 1;
  return res;
}

static void get_mac(void) {
  u16 e[4];
  if (read_eeprom(e) < 0)
    return;
  u16 sum = (u16)(e[0] + e[1] + e[2]);
  if ((!e[0] && !e[1] && !e[2]) || sum != e[3])
    return;
  for (int i = 0; i < 3; i++)
    mac[2 * i] = (u8)e[i], mac[2 * i + 1] = (u8)(e[i] >> 8);
  mac_ok = 1;
}

int mxdev9_mac(u8 *out) {
  if (!mac_ok)
    return -1;
  for (int i = 0; i < 6; i++)
    out[i] = mac[i];
  return 0;
}

u32 mxdev9_caps(void) { return caps; }

void mxdev9_led(int on) {
  USE_SPD_REGS;
  if (dvr) {
    SPD_REG8(SPD_R_PIO_DIR) |= 1;
    SPD_REG8(SPD_R_PIO_DATA) = (u8)((SPD_REG8(SPD_R_PIO_DATA) & 0xE) | (on ? 0 : 1));
  } else
    SPD_REG8(SPD_R_PIO_DATA) = (u8)!on;
}

/* ---- power */

int mxdev9_on_off(void (*fn)(void)) {
  for (int i = 0; i < 4; i++)
    if (!off_fn[i]) {
      off_fn[i] = fn;
      return 0;
    }
  return -1;
}

void mxdev9_off(void) {
  USE_DEV9_REGS;
  for (int i = 0; i < 4; i++)
    if (off_fn[i])
      off_fn[i]();
  mxdev9_intr_off(0xffff);
  DEV9_REG(DEV9_R_1466) = 1;
  DEV9_REG(DEV9_R_1464) = 0;
  DEV9_REG(DEV9_R_1460) = DEV9_REG(DEV9_R_1464);
  DEV9_REG(DEV9_R_POWER) = DEV9_REG(DEV9_R_POWER) & ~4;
  DEV9_REG(DEV9_R_POWER) = DEV9_REG(DEV9_R_POWER) & ~1;
}

/* DEV9 off: on, then the device out of reset (ps2sdk: expbay_device_reset) */
static int power_on(void) {
  USE_DEV9_REGS;
  DEV9_REG(DEV9_R_1466) = 1;
  DEV9_REG(DEV9_R_1464) = 0;
  DEV9_REG(DEV9_R_1460) = DEV9_REG(DEV9_R_1464);
  if (DEV9_REG(DEV9_R_1462) & 1)
    return -1; /* nothing in the bay */
  DEV9_REG(DEV9_R_POWER) = (DEV9_REG(DEV9_R_POWER) & ~1) | 4;
  DelayThread(500000);
  DEV9_REG(DEV9_R_1460) = DEV9_REG(DEV9_R_1460) | 1;
  DEV9_REG(DEV9_R_POWER) = DEV9_REG(DEV9_R_POWER) | 1;
  DelayThread(500000);
  return 0;
}

int _start(int argc, char **argv) {
  USE_DEV9_REGS;
  USE_SPD_REGS;
  int st;
  (void)argc, (void)argv;

  if ((DEV9_REG(DEV9_R_REV) & 0xf0) != 0x30) {
    printf("mxdev9: no CXD9611 (expansion bay)\n");
    return MODULE_NO_RESIDENT_END;
  }
  _sw(0x51011, SSBUS_R_1420);
  _sw(0xe01a3043, SSBUS_R_1418);
  _sw(0xef1a3043, SSBUS_R_141c);
  if (!(DEV9_REG(DEV9_R_POWER) & 4) && power_on() < 0) {
    printf("mxdev9: no device\n");
    return MODULE_NO_RESIDENT_END;
  }
  spd_rev = SPD_REG16(SPD_R_REV_1);
  caps = SPD_REG16(SPD_R_REV_3);
  dvr = caps & SPD_CAPS_DVR ? 1 : 0;

  CpuSuspendIntr(&st);
  dmac_set_dpcr2(dmac_get_dpcr2() | DPCR2_DEV9);
  CpuResumeIntr(st);
  DEV9_REG(DEV9_R_1464) = 0x103 & 0x3f;
  mask_now = 1;
  set_mask(0);
  get_mac();
  if (dvr) {
    SPD_REG8(SPD_R_PIO_DIR) |= 6;
    DelayThread(1);
    SPD_REG8(SPD_R_PIO_DATA) = (u8)(SPD_REG8(SPD_R_PIO_DATA) | 6); /* LED 2 and PIO 3 off */
  }
  mxdev9_led(0);

  if (RegisterLibraryEntries(&_exp_mxdev9) != 0)
    return MODULE_NO_RESIDENT_END;
  CpuSuspendIntr(&st);
  RegisterIntrHandler(IOP_IRQ_DEV9, 1, dev9_irq, 0);
  RegisterIntrHandler(IOP_IRQ_DMA_DEV9, 1, dma_irq, 0);
  EnableIntr(IOP_IRQ_DEV9);
  CpuResumeIntr(st);
  DEV9_REG(DEV9_R_1466) = 0;
  printf("mxdev9: SPEED rev %x caps %x%s\n", spd_rev, caps, mac_ok ? "" : ", no MAC");
  return MODULE_RESIDENT_END;
}
