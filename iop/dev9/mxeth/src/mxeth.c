/* mxeth: the network adapter's SMAP (docs/iop-drivers.md).

   One driver thread receives and sends. The SMAP's interrupt masks itself and
   wakes it once for all that is there; the thread takes the frames out by
   DMA, each frame's DMA running while the one before goes to the clients,
   fills the TX FIFO, and unmasks the interrupt. A DMA interrupt per frame
   and a thread switch per frame cost the IOP more than the rest of a frame's
   DMA (docs/iop-drivers.md). The link thread watches the PHY. The register sequences follow ps2sdk's smap driver
   (iop/network/smap, Academic Free License 2.0: ../LICENSE). */
#include "irx_imports.h"
#include <speedregs.h>
#include <smapregs.h>
#include <mxdev9.h>
#include <mxeth.h>

IRX_ID("mxeth", 1, 1);

extern struct irx_export_table _exp_mxeth;

#define DEFAULT_FRAMES 48
#define SMAP_DEVICE 1
#define BD_N SMAP_BD_MAX_ENTRY
#define EV_RX 1u
#define EV_TX 2u
#define EV_HW 4u
#define RX_WORK (SMAP_INTR_RXEND | SMAP_INTR_RXDNV)
#define RX_INTR (SMAP_INTR_EMAC3 | RX_WORK)
#define TX_INTR (SMAP_INTR_TXEND | SMAP_INTR_TXDNV)

static volatile smap_bd_t *const tx_bd = (smap_bd_t *)(SMAP_REGBASE + SMAP_BD_TX_BASE);
static volatile smap_bd_t *const rx_bd = (smap_bd_t *)(SMAP_REGBASE + SMAP_BD_RX_BASE);

static u8 mac[6];
static volatile int link_mode;
static int ev;
static u32 st[MXETH_STATS];
static u32 pending; /* event bits to set, interrupts off */
#ifdef MXETH_PROFILE /* MXETH_T_*: IOP timer ticks of each part */
static int timer = -1;
#define NOW() (timer >= 0 ? (u32)GetTimerCounter(timer) : 0)
#define ADD(i, v) (st[i] += (v))
#else
#define NOW() 0u
#define ADD(i, v) ((void)(v))
#endif

static struct mxeth_client *clients;

/* receive: the free list with interrupts off, the rest the driver thread's */
static struct mxeth_frame *free_list;
static u32 n_free;
static volatile int rx_starved;
static u32 rx_i;
static struct mxdev9_dma rx_dma;

/* send: the queues with interrupts off, the rest the driver thread's */
static struct mxeth_tx *txq_head, *txq_tail, *done_head, *done_tail;
static u32 tx_space = SMAP_TX_BUFSIZE, tx_inflight, tx_bd_i, tx_end_i;
static struct mxdev9_dma tx_dma;

/* ---- registers */

static u32 e3_get(u32 off) {
  USE_SMAP_EMAC3_REGS;
  u32 r = SMAP_EMAC3_REG32(off);
  return (r >> 16 & 0xffff) | (r & 0xffff) << 16;
}

static void e3_set(u32 off, u32 v) {
  USE_SMAP_EMAC3_REGS;
  SMAP_EMAC3_REG32(off) = (v >> 16 & 0xffff) | (v & 0xffff) << 16;
  (void)e3_get(off);
}

/* An MDIO access takes about 30 us: short sleeps until it is done */
static u32 phy_cmd(u32 reg, u32 op) {
  return (reg & SMAP_E3_PHY_REG_ADDR_MSK) | op |
         (SMAP_DsPHYTER_ADDRESS & SMAP_E3_PHY_ADDR_MSK) << SMAP_E3_PHY_ADDR_BITSFT;
}

static void phy_write(u32 reg, u16 v) {
  e3_set(SMAP_R_EMAC3_STA_CTRL, phy_cmd(reg, SMAP_E3_PHY_WRITE) | (u32)v << SMAP_E3_PHY_DATA_BITSFT);
  for (int i = 0; i < 200 && !(e3_get(SMAP_R_EMAC3_STA_CTRL) & SMAP_E3_PHY_OP_COMP); i++)
    DelayThread(20);
}

static u16 phy_read(u32 reg) {
  e3_set(SMAP_R_EMAC3_STA_CTRL, phy_cmd(reg, SMAP_E3_PHY_READ));
  for (int i = 0; i < 200; i++) {
    u32 v;
    /* three in a row, as smap: the bit can show before the data */
    if (e3_get(SMAP_R_EMAC3_STA_CTRL) & SMAP_E3_PHY_OP_COMP &&
        e3_get(SMAP_R_EMAC3_STA_CTRL) & SMAP_E3_PHY_OP_COMP &&
        (v = e3_get(SMAP_R_EMAC3_STA_CTRL)) & SMAP_E3_PHY_OP_COMP)
      return (u16)(v >> SMAP_E3_PHY_DATA_BITSFT);
    DelayThread(20);
  }
  return 0;
}

/* ---- waking the driver thread */

/* from interrupt context */
static void wake_i(void) {
  if (pending)
    iSetEventFlag(ev, pending);
  pending = 0;
}

static void wake(void) {
  if (QueryIntrContext()) {
    wake_i();
    return;
  }
  int s;
  CpuSuspendIntr(&s);
  u32 b = pending;
  pending = 0;
  CpuResumeIntr(s);
  if (b)
    SetEventFlag(ev, b);
}

/* interrupts off */
static void to_done(struct mxeth_tx *t) {
  t->next = 0;
  if (done_tail)
    done_tail->next = t;
  else {
    done_head = t;
    pending |= EV_TX;
  }
  done_tail = t;
}

/* ---- the SMAP's interrupt (SPEED bits 2-6): it masks what fired and wakes
   the driver thread, which unmasks it when it is through */

static void smap_irq(void *arg, u32 s) {
  USE_SMAP_REGS;
  (void)arg;
  u32 t0 = NOW();
  st[MXETH_IRQS]++;
  SMAP_REG16(SMAP_R_INTR_CLR) = (u16)s;
  if (s & SMAP_INTR_EMAC3)
    e3_set(SMAP_R_EMAC3_INTR_STAT, SMAP_E3_INTR_TX_ERR_0 | SMAP_E3_INTR_SQE_ERR_0 | SMAP_E3_INTR_DEAD_0);
  if (s & SMAP_INTR_RXDNV)
    st[MXETH_RX_OVERRUN]++;
  mxdev9_intr_off(s & (RX_WORK | TX_INTR));
  pending |= EV_HW;
  wake_i();
  ADD(MXETH_T_IRQ, NOW() - t0);
}

/* ---- receive, in the driver thread */

static void wait_dmaen(u32 ctrl, u32 bit) {
  USE_SMAP_REGS;
  for (int n = 0; SMAP_REG8(ctrl) & bit && n < 10000; n++)
    st[MXETH_DMAEN_SPINS]++;
}

static struct mxeth_frame *take_free(void) {
  int s;
  CpuSuspendIntr(&s);
  struct mxeth_frame *f = free_list;
  if (f) {
    free_list = f->next;
    n_free--;
  } else
    rx_starved = 1;
  CpuResumeIntr(s);
  return f;
}

static void rx_next_bd(void) {
  USE_SMAP_REGS;
  SMAP_REG8(SMAP_R_RXFIFO_FRAME_DEC) = 0;
  rx_bd[rx_i % BD_N].ctrl_stat = SMAP_BD_RX_EMPTY;
  rx_i++;
}

/* the next good frame in the SMAP: 1 with its length and FIFO pointer */
static int rx_next(u32 *len, u32 *ptr) {
  USE_SMAP_REGS;
  for (;;) {
    /* smap's workaround: the MAL's RX FIFO can stop answering under load,
       so look at the frame count before the descriptors */
    if (!SMAP_REG8(SMAP_R_RXFIFO_FRAME_CNT))
      return 0;
    volatile smap_bd_t *bd = &rx_bd[rx_i % BD_N];
    u16 cs = bd->ctrl_stat;
    if (cs & SMAP_BD_RX_EMPTY)
      return 0;
    *len = bd->length, *ptr = bd->pointer;
    if (!(cs & SMAP_BD_RX_ERROR) && *len >= 14 && *len <= MXETH_FRAME_SIZE)
      return 1;
    st[MXETH_RX_ERRORS]++;
    SMAP_REG16(SMAP_R_RXFIFO_RD_PTR) = (u16)(*ptr + ((*len + 3) & ~3u));
    rx_next_bd();
  }
}

static void offer(struct mxeth_frame *f) {
  u32 t0 = NOW();
  for (struct mxeth_client *c = clients; c; c = c->next)
    if (c->rx && c->rx(f, c->arg)) {
      ADD(MXETH_T_CLIENTS, NOW() - t0);
      return;
    }
  st[MXETH_RX_UNTAKEN]++;
  mxeth_free(f);
  ADD(MXETH_T_CLIENTS, NOW() - t0);
}

static void rx_start(struct mxdev9_dma *d) {
  USE_SMAP_REGS;
  SMAP_REG16(SMAP_R_RXFIFO_SIZE) = d->blocks;
  SMAP_REG8(SMAP_R_RXFIFO_CTRL) = SMAP_RXFIFO_DMAEN;
}

/* All frames the SMAP holds. Each frame's DMA runs while the one before it
   goes to the clients: the CPU waits only for what is left of the DMA. */
static u32 rx_batch(void) {
  USE_SMAP_REGS;
  struct mxeth_frame *prev = 0;
  u32 len, ptr, n = 0;
  while (rx_next(&len, &ptr)) {
    struct mxeth_frame *f = take_free();
    if (!f) {
      st[MXETH_RX_STARVED]++;
      break;
    }
    f->len = (u16)len;
    SMAP_REG16(SMAP_R_RXFIFO_RD_PTR) = (u16)ptr;
    u32 blocks = len >> 6;
    if (blocks) {
      rx_dma.buf = f->data;
      rx_dma.blocks = (u16)blocks;
      while (mxdev9_dma_begin(&rx_dma) < 0)
        DelayThread(50); /* the channel is another driver's (ATA) */
      st[MXETH_RX_DMAS]++;
      if (prev) {
        offer(prev);
        prev = 0;
      }
      u32 t1 = NOW();
      mxdev9_dma_end(&rx_dma);
      wait_dmaen(SMAP_R_RXFIFO_CTRL, SMAP_RXFIFO_DMAEN);
      ADD(MXETH_T_DMA, NOW() - t1);
    }
    u32 *w = (u32 *)f->data;
    for (u32 i = blocks * 64; i < len; i += 4)
      w[i / 4] = SMAP_REG32(SMAP_R_RXFIFO_DATA);
    rx_next_bd();
    st[MXETH_RX_FRAMES]++;
    st[MXETH_RX_BYTES] += len;
    if (prev)
      offer(prev);
    prev = f;
    n++;
  }
  if (prev)
    offer(prev);
  return n;
}

/* ---- send, in the driver thread */

static void tx_start(struct mxdev9_dma *d) {
  USE_SMAP_REGS;
  SMAP_REG16(SMAP_R_TXFIFO_SIZE) = d->blocks;
  SMAP_REG8(SMAP_R_TXFIFO_CTRL) = SMAP_TXFIFO_DMAEN;
}

/* the frames the MAC has sent: their FIFO space and descriptors back */
static void tx_reclaim(void) {
  while (tx_inflight) {
    volatile smap_bd_t *bd = &tx_bd[tx_end_i % BD_N];
    u16 cs = bd->ctrl_stat;
    if (cs & SMAP_BD_TX_READY)
      break;
    if (cs & SMAP_BD_TX_ERROR)
      st[MXETH_TX_ERRORS]++;
    tx_space += (bd->length + 3) & ~3u;
    tx_end_i++;
    tx_inflight--;
  }
}

/* the senders' done: 1 if there were any */
static int run_done(void) {
  int s;
  CpuSuspendIntr(&s);
  struct mxeth_tx *t = done_head;
  done_head = done_tail = 0;
  CpuResumeIntr(s);
  if (!t)
    return 0;
  while (t) {
    struct mxeth_tx *n = t->next;
    if (t->done)
      t->done(t);
    t = n;
  }
  return 1;
}

/* As many queued frames into the TX FIFO as it takes (4 KiB: two full
   frames); TXEND says when there is room again. */
static void tx_fill(void) {
  USE_SMAP_REGS;
  u32 kicked = tx_bd_i;
  for (;;) {
    tx_reclaim();
    int s;
    CpuSuspendIntr(&s);
    struct mxeth_tx *t = txq_head;
    u32 size = t ? (t->len + 3) & ~3u : 0;
    if (t && (tx_inflight >= BD_N || tx_space < size))
      t = 0;
    if (t) {
      txq_head = t->next;
      if (!txq_head)
        txq_tail = 0;
    }
    CpuResumeIntr(s);
    if (!t)
      break;
    tx_space -= size;
    u32 len = t->len, ptr = SMAP_REG16(SMAP_R_TXFIFO_WR_PTR) + SMAP_TX_BASE;
    if ((u32)t->data & 3) {
      const u8 *p = t->data;
      for (u32 i = 0; i < len; i += 4, p += 4)
        SMAP_REG32(SMAP_R_TXFIFO_DATA) = p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
    } else {
      u32 blocks = len >> 6;
      if (blocks) {
        tx_dma.buf = (void *)t->data;
        tx_dma.blocks = (u16)blocks;
        while (mxdev9_dma_begin(&tx_dma) < 0)
          DelayThread(50);
        run_done(); /* the frames before, while the DMAC moves this one */
        mxdev9_dma_end(&tx_dma);
        wait_dmaen(SMAP_R_TXFIFO_CTRL, SMAP_TXFIFO_DMAEN);
      }
      const u32 *w = t->data;
      for (u32 i = blocks * 64; i < len; i += 4)
        SMAP_REG32(SMAP_R_TXFIFO_DATA) = w[i / 4];
    }
    volatile smap_bd_t *bd = &tx_bd[tx_bd_i % BD_N];
    bd->length = (u16)len;
    bd->pointer = (u16)ptr;
    SMAP_REG8(SMAP_R_TXFIFO_FRAME_INC) = 0;
    bd->ctrl_stat = SMAP_BD_TX_READY | SMAP_BD_TX_GENFCS | SMAP_BD_TX_GENPAD;
    tx_bd_i++;
    tx_inflight++;
    st[MXETH_TX_FRAMES]++;
    CpuSuspendIntr(&s);
    to_done(t);
    CpuResumeIntr(s);
    if (tx_inflight == 1) { /* the MAC was idle: it starts at once */
      e3_set(SMAP_R_EMAC3_TxMODE0, SMAP_E3_TX_GNP_0);
      kicked = tx_bd_i;
    }
  }
  /* the MAC through the new descriptors: once for all (EMAC3 access is slow) */
  if (tx_bd_i != kicked)
    e3_set(SMAP_R_EMAC3_TxMODE0, SMAP_E3_TX_GNP_0);
}

/* ---- the library */

static int driver_tid;

int mxeth_attach(struct mxeth_client *c) {
  int s;
  CpuSuspendIntr(&s);
  struct mxeth_client **p = &clients;
  while (*p && (*p)->prio <= c->prio)
    p = &(*p)->next;
  c->next = *p;
  *p = c;
  CpuResumeIntr(s);
  if (link_mode && c->link)
    c->link(link_mode, c->arg);
  return 0;
}

void mxeth_free(struct mxeth_frame *f) {
  int s;
  CpuSuspendIntr(&s);
  f->next = free_list;
  free_list = f;
  n_free++;
  if (rx_starved) {
    rx_starved = 0;
    pending |= EV_HW;
  }
  CpuResumeIntr(s);
  wake();
}

int mxeth_send(struct mxeth_tx *t) {
  int s, r = 0;
  CpuSuspendIntr(&s);
  if (!link_mode) {
    st[MXETH_TX_DROPPED]++;
    to_done(t);
    r = -1;
  } else {
    t->next = 0;
    if (txq_tail)
      txq_tail->next = t;
    else
      txq_head = t;
    txq_tail = t;
    /* the driver thread itself sends it before it waits again */
    if (QueryIntrContext() || GetThreadId() != driver_tid)
      pending |= EV_TX;
  }
  CpuResumeIntr(s);
  wake();
  return r;
}

void mxeth_mac(u8 *out) {
  for (int i = 0; i < 6; i++)
    out[i] = mac[i];
}

int mxeth_link(void) { return link_mode; }

u32 mxeth_free_frames(void) { return n_free; }

void mxeth_stats(u32 *out) {
  for (int i = 0; i < MXETH_STATS; i++)
    out[i] = st[i];
}

/* ---- the driver thread */

static volatile int rx_on; /* the MAC is on: RX interrupts wanted */

/* Receiving while frames come: the SMAP's interrupt stays masked, and the
   thread looks again after poll_us (an alarm), for all that came meanwhile,
   as ps2sdk's smap does: one wake-up for several frames. When a look finds
   nothing, the interrupt is unmasked again. */
static u32 poll_us = 1000;
static iop_sys_clock_t poll_clock;
static volatile int poll_set;

static unsigned int poll_alarm(void *arg) {
  (void)arg;
  poll_set = 0;
  pending |= EV_HW;
  wake_i();
  return 0;
}

static void driver(void *unused) {
  (void)unused;
  driver_tid = GetThreadId();
  for (;;) {
    u32 bits;
    if (WaitEventFlag(ev, EV_RX | EV_TX | EV_HW, WEF_OR | WEF_CLEAR, &bits) < 0)
      break;
    st[MXETH_WAKES]++;
    u32 t0 = NOW(), got = 0;
    do {
      tx_fill();
      got += rx_batch();
      tx_fill();
    } while (run_done());
    /* frames that came meanwhile set their status bit: unmasked, it fires */
    u32 on = tx_inflight ? TX_INTR : 0;
    if (rx_on && !rx_starved) {
      if (got && poll_us) {
        if (!poll_set) {
          poll_set = 1;
          if (SetAlarm(&poll_clock, poll_alarm, 0) < 0)
            poll_set = 0, on |= RX_WORK;
        }
      } else
        on |= RX_WORK;
    }
    mxdev9_intr_off((TX_INTR | RX_WORK) & ~on);
    mxdev9_intr_on(on);
    ADD(MXETH_T_THREAD, NOW() - t0);
  }
  ExitDeleteThread();
}

/* The negotiated mode, 0 while there is none. The PHY keeps its link when
   only the IOP is reset: then it is taken as it is, without a reset. */
static int phy_started;

static int phy_mode(void) {
  u16 bmcr = phy_read(SMAP_DsPHYTER_BMCR);
  phy_read(SMAP_DsPHYTER_BMSR); /* the link bit latches low */
  u16 bmsr = phy_read(SMAP_DsPHYTER_BMSR);
  if (!(bmcr & SMAP_PHY_BMCR_ANEN) || !(bmsr & SMAP_PHY_BMSR_LINK) || !(bmsr & SMAP_PHY_BMSR_ANCP) ||
      bmsr & 0x10 /* remote fault */) {
    if (phy_started && (bmcr & SMAP_PHY_BMCR_ANEN) && !(bmsr & 0x10))
      return 0; /* still negotiating */
    phy_write(SMAP_DsPHYTER_BMCR, SMAP_PHY_BMCR_RST);
    for (int i = 0; i < 100 && phy_read(SMAP_DsPHYTER_BMCR) & SMAP_PHY_BMCR_RST; i++)
      DelayThread(1000);
    phy_write(SMAP_DsPHYTER_BMCR, 0);
    u16 caps = phy_read(SMAP_DsPHYTER_BMSR), conf = 0x5E0;
    if (!(caps & 0x4000))
      conf &= ~0x100; /* 100 FDX */
    if (!(caps & 0x2000))
      conf &= ~0x80; /* 100 HDX */
    if (!(caps & 0x1000))
      conf &= ~0x40; /* 10 FDX */
    if (!(caps & 0x0800))
      conf &= ~0x20; /* 10 HDX */
    u16 anar = phy_read(SMAP_DsPHYTER_ANAR);
    phy_write(SMAP_DsPHYTER_ANAR, (u16)((conf & 0x5E0) | (anar & 0x1F)));
    phy_write(SMAP_DsPHYTER_BMCR, SMAP_PHY_BMCR_ANEN | SMAP_PHY_BMCR_RSAN);
    phy_started = 1;
    return 0;
  }
  phy_started = 1;
  u16 id1 = phy_read(SMAP_DsPHYTER_PHYIDR1), id2 = phy_read(SMAP_DsPHYTER_PHYIDR2);
  if (id1 == SMAP_PHY_IDR1_VAL && (id2 & SMAP_PHY_IDR2_MSK) == SMAP_PHY_IDR2_VAL &&
      !(id2 & SMAP_PHY_IDR2_REV_MSK)) {
    /* the DP83846A's first revision, as smap */
    phy_write(0x13, 1);
    phy_write(SMAP_DsPHYTER_PHYCTRL, 0x1898);
    phy_write(0x1F, 0);
    phy_write(0x1D, 0x5040);
    phy_write(0x1E, 0x8C);
    phy_write(0x13, 0);
  }
  u16 v = phy_read(SMAP_DsPHYTER_ANAR) & phy_read(SMAP_DsPHYTER_ANLPAR);
  int fast = (v & 0x180) != 0, fdx = (v & 0x140) != 0, flow = fdx && (v & 0x400);
  int mode = fast ? (fdx ? MXETH_LINK_100FDX : MXETH_LINK_100HDX) : (fdx ? MXETH_LINK_10FDX : MXETH_LINK_10HDX);
  return mode | (flow ? MXETH_LINK_FLOW : 0);
}

static void notify(int mode) {
  for (struct mxeth_client *c = clients; c; c = c->next)
    if (c->link)
      c->link(mode, c->arg);
}

static void link_thread(void *unused) {
  (void)unused;
  int mac_on = 0;
  for (;;) {
    if (!link_mode) {
      int m = phy_mode();
      if (!m) {
        DelayThread(100000);
        continue;
      }
      u32 m1 = e3_get(SMAP_R_EMAC3_MODE1) & 0x67FFFFFF;
      if (m & (MXETH_LINK_10FDX | MXETH_LINK_100FDX))
        m1 |= SMAP_E3_FDX_ENABLE;
      if (m & MXETH_LINK_FLOW)
        m1 |= SMAP_E3_FLOWCTRL_ENABLE | SMAP_E3_ALLOW_PF;
      e3_set(SMAP_R_EMAC3_MODE1, m1);
      if (!mac_on) {
        e3_set(SMAP_R_EMAC3_MODE0, SMAP_E3_TXMAC_ENABLE | SMAP_E3_RXMAC_ENABLE);
        DelayThread(10000);
        rx_on = 1;
        mxdev9_intr_on(RX_INTR);
        mac_on = 1;
      }
      link_mode = m;
      printf("mxeth: link %s %s duplex%s\n", m & (MXETH_LINK_100HDX | MXETH_LINK_100FDX) ? "100" : "10",
             m & (MXETH_LINK_10FDX | MXETH_LINK_100FDX) ? "full" : "half", m & MXETH_LINK_FLOW ? ", flow control" : "");
      notify(m);
      continue;
    }
    DelayThread(1000000);
    if (!(phy_read(SMAP_DsPHYTER_BMSR) & SMAP_PHY_BMSR_LINK)) {
      int s;
      CpuSuspendIntr(&s);
      link_mode = 0;
      /* what waits to be sent is dropped: its senders get their done */
      while (txq_head) {
        struct mxeth_tx *t = txq_head;
        txq_head = t->next;
        st[MXETH_TX_DROPPED]++;
        to_done(t);
      }
      txq_tail = 0;
      CpuResumeIntr(s);
      wake();
      printf("mxeth: link down\n");
      notify(0);
    }
  }
}

/* ---- start */

static int reset_wait(u32 off, u32 bit, int e3) {
  USE_SMAP_REGS;
  for (int i = 0; i < 10; i++) {
    if (!((e3 ? e3_get(off) : SMAP_REG8(off)) & bit))
      return 0;
    DelayThread(1000);
  }
  return -1;
}

static int smap_init(void) {
  USE_SPD_REGS;
  USE_SMAP_REGS;
  if (!(SPD_REG16(SPD_R_REV_3) & SPD_CAPS_SMAP) || SPD_REG16(SPD_R_REV_1) < 0x11)
    return -1;
  mxdev9_intr_off(SMAP_INTR_BITMSK);
  SMAP_REG8(SMAP_R_TXFIFO_CTRL) = SMAP_TXFIFO_RESET;
  if (reset_wait(SMAP_R_TXFIFO_CTRL, SMAP_TXFIFO_RESET, 0) < 0)
    return -2;
  SMAP_REG8(SMAP_R_RXFIFO_CTRL) = SMAP_RXFIFO_RESET;
  if (reset_wait(SMAP_R_RXFIFO_CTRL, SMAP_RXFIFO_RESET, 0) < 0)
    return -3;
  e3_set(SMAP_R_EMAC3_MODE0, SMAP_E3_SOFT_RESET);
  if (reset_wait(SMAP_R_EMAC3_MODE0, SMAP_E3_SOFT_RESET, 1) < 0)
    return -4;
  SMAP_REG8(SMAP_R_BD_MODE) = 0;
  for (int i = 0; i < BD_N; i++) {
    tx_bd[i].ctrl_stat = 0, tx_bd[i].reserved = 0, tx_bd[i].length = 0, tx_bd[i].pointer = 0;
    rx_bd[i].ctrl_stat = SMAP_BD_RX_EMPTY, rx_bd[i].reserved = 0, rx_bd[i].length = 0, rx_bd[i].pointer = 0;
  }
  SMAP_REG16(SMAP_R_INTR_CLR) = SMAP_INTR_BITMSK;
  e3_set(SMAP_R_EMAC3_MODE1, SMAP_E3_FDX_ENABLE | SMAP_E3_IGNORE_SQE | SMAP_E3_MEDIA_100M | SMAP_E3_RXFIFO_2K |
                                 SMAP_E3_TXFIFO_1K | SMAP_E3_TXREQ0_MULTI | SMAP_E3_TXREQ1_SINGLE);
  /* TX FIFO requests: low 7*8 = 56, urgent 15*8 = 120 */
  e3_set(SMAP_R_EMAC3_TxMODE1, (7 & SMAP_E3_TX_LOW_REQ_MSK) << SMAP_E3_TX_LOW_REQ_BITSFT |
                                   (15 & SMAP_E3_TX_URG_REQ_MSK) << SMAP_E3_TX_URG_REQ_BITSFT);
  e3_set(SMAP_R_EMAC3_RxMODE,
         SMAP_E3_RX_STRIP_PAD | SMAP_E3_RX_STRIP_FCS | SMAP_E3_RX_INDIVID_ADDR | SMAP_E3_RX_BCAST | SMAP_E3_RX_MCAST);
  e3_set(SMAP_R_EMAC3_INTR_STAT, SMAP_E3_INTR_TX_ERR_0 | SMAP_E3_INTR_SQE_ERR_0 | SMAP_E3_INTR_DEAD_0);
  e3_set(SMAP_R_EMAC3_INTR_ENABLE, SMAP_E3_INTR_TX_ERR_0 | SMAP_E3_INTR_SQE_ERR_0 | SMAP_E3_INTR_DEAD_0);
  e3_set(SMAP_R_EMAC3_ADDR_HI, (u32)mac[0] << 8 | mac[1]);
  e3_set(SMAP_R_EMAC3_ADDR_LO, (u32)mac[2] << 24 | (u32)mac[3] << 16 | (u32)mac[4] << 8 | mac[5]);
  e3_set(SMAP_R_EMAC3_PAUSE_TIMER, 0xFFFF);
  e3_set(SMAP_R_EMAC3_GROUP_HASH1, 0);
  e3_set(SMAP_R_EMAC3_GROUP_HASH2, 0);
  e3_set(SMAP_R_EMAC3_GROUP_HASH3, 0);
  e3_set(SMAP_R_EMAC3_GROUP_HASH4, 0);
  e3_set(SMAP_R_EMAC3_INTER_FRAME_GAP, 4);
  /* TX threshold (12+1)*64 = 832; RX watermarks 16*8 = 128, 128*8 = 1024 */
  e3_set(SMAP_R_EMAC3_TX_THRESHOLD, (12 & SMAP_E3_TX_THRESHLD_MSK) << SMAP_E3_TX_THRESHLD_BITSFT);
  e3_set(SMAP_R_EMAC3_RX_WATERMARK, (16 & SMAP_E3_RX_LO_WATER_MSK) << SMAP_E3_RX_LO_WATER_BITSFT |
                                        (128 & SMAP_E3_RX_HI_WATER_MSK) << SMAP_E3_RX_HI_WATER_BITSFT);
  return 0;
}

static u32 number(const char *s) {
  u32 v = 0;
  for (; *s >= '0' && *s <= '9'; s++)
    v = v * 10 + (u32)(*s - '0');
  return v;
}

int _start(int argc, char **argv) {
  u32 frames = DEFAULT_FRAMES;
  for (int i = 1; i < argc; i++)
    if (!strncmp(argv[i], "rx=", 3))
      frames = number(argv[i] + 3);
    else if (!strncmp(argv[i], "poll=", 5))
      poll_us = number(argv[i] + 5);
  if (frames < 8)
    frames = 8;
  if (mxdev9_mac(mac) < 0) {
    printf("mxeth: no MAC address\n");
    return MODULE_NO_RESIDENT_END;
  }
  int r = smap_init();
  if (r < 0) {
    printf("mxeth: no SMAP (%d)\n", r);
    return MODULE_NO_RESIDENT_END;
  }
  struct mxeth_frame *pool = AllocSysMemory(0, (int)(frames * sizeof *pool), 0);
  if (!pool)
    return MODULE_NO_RESIDENT_END;
  for (u32 i = 0; i < frames; i++) {
    pool[i].index = (u16)i;
    pool[i].next = free_list;
    free_list = &pool[i];
  }
  n_free = frames;

#ifdef MXETH_PROFILE
  if ((timer = AllocHardTimer(1, 32, 1)) >= 0)
    SetTimerMode(timer, 0);
#endif
  USec2SysClock(poll_us ? poll_us : 1, &poll_clock);
  rx_dma.block_words = tx_dma.block_words = 16;
  rx_dma.device = tx_dma.device = SMAP_DEVICE;
  rx_dma.to_mem = 1;
  rx_dma.start = rx_start;
  tx_dma.start = tx_start;

  iop_event_t e = {0, 0, 0};
  if ((ev = CreateEventFlag(&e)) < 0)
    return MODULE_NO_RESIDENT_END;
  iop_thread_t d = {TH_C, 0, driver, 0x1000, 0x28};
  iop_thread_t l = {TH_C, 0, link_thread, 0x800, 0x29};
  int dt = CreateThread(&d), lt = CreateThread(&l);
  if (dt <= 0 || lt <= 0)
    return MODULE_NO_RESIDENT_END;
  if (mxdev9_intr(SMAP_INTR_BITMSK, smap_irq, 0) < 0)
    return MODULE_NO_RESIDENT_END;
  if (RegisterLibraryEntries(&_exp_mxeth) != 0)
    return MODULE_NO_RESIDENT_END;
  StartThread(dt, 0);
  StartThread(lt, 0);
  printf("mxeth: %02x:%02x:%02x:%02x:%02x:%02x, %u receive buffers\n", mac[0], mac[1], mac[2], mac[3], mac[4],
         mac[5], frames);
  return MODULE_RESIDENT_END;
}
