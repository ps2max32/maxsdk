/* SIF DMA without the BIOS (mx OS plan, 2g: the BIOS only at the boot).

   SIF1 (DMAC channel 6, EE to IOP) runs in chain mode through a ring of 32
   DMA tags here, the last a NEXT back to the first, as the BIOS's
   SifSetDma does with a ring of its own (docs/bios.md): a transfer is an
   IOP tag (its destination, its size in words, the ERT and INT_O flags) in
   a packet of the ring's own, with the first 7 quadwords of the data
   copied after it (from a physical address too, through the uncached
   alias), then the rest of the data; each a REF tag, the last of a
   call REFE, so the channel stops after it. The copy is the BIOS's
   contract: sif.c sends a command packet (at most 112 bytes) from a buffer
   it uses again at once, on the stack too (an IOP reset's). Without it the
   PS2 (not PCSX2, whose DMA is done at once) sent the IOP a later packet. A call while the channel
   runs: the DMAC suspended (CPND), the channel stopped, the tag it was on
   made a REF again (rebuilt from MADR and QWC if it was the last), the new
   tags after it, started again.

   The ring is used only once taken (mx__sif_dma_take, the kernel at its
   start): a program on the BIOS's threads, and the kernel when it runs on
   them, use the BIOS's ring, so channel 6 is given back, idle and at the
   BIOS's place, before ExecPS2 (mx__sif_dma_give). Before it is taken, and
   after it is given back, the calls go to the BIOS.

   SIF0 (channel 5, IOP to EE: the receive chain), the SIF's registers and
   flags are written here once taken, by the BIOS before. The BIOS's software registers
   (SYSREG_*, a table in its memory that ps2sdk's programs read too) stay
   the BIOS's for now. */

#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/hw/regs.h>
#include <mx/sif.h>
#include <mx/sifdma.h>

#define RING 32 /* tags; RING - 1 used, the last the NEXT to the first */
#define USED (RING - 1)
#define TAG_REFE 0x00000000u
#define TAG_REF 0x30000000u
#define TAG_NEXT 0x20000000u
#define TAG_IRQ 0x80000000u
#define CHCR_STR 0x100u
#define CHCR_GO 0x184u /* STR, TIE, chain */
#define CPND (1u << 16)
/* struct sif_dma's attr (ps2sdk's SIF_DMA_*) */
#define ATTR_INT_I 0x02
#define ATTR_INT_O 0x04
#define ATTR_TAG 0x20
#define ATTR_ERT 0x40

struct sif_dma {
  u32 src, dest;
  s32 size, attr;
};

/* touched only uncached, on lines of their own */
#define COPY 7 /* quadwords of data copied after the IOP tag */
static u32 tags[RING][4] MX_DMA_LINES;
static u32 pkts[RING][4 * (1 + COPY)] MX_DMA_LINES;
static u32 cur = USED - 1; /* the last tag written */
static u32 written;        /* tags written, all time */
static int own;
static u32 bios_tadr; /* the BIOS's place in its ring, when taken */

static volatile u32 *uc(void *p) { return (volatile u32 *)MX_UNCACHED(p); }
static u32 phys(const void *p) { return mx_paddr_of(p).v; }
#define R32(a) (*(volatile u32 *)(uintptr_t)(a))

/* The registers of DMA channels 5 to 7 (the page at 0x1000_C000): the
   BIOS's TLB maps that page read only, so a store in user mode (the
   kernel's threads, programs) is a TLB modified exception (seen on the
   PS2: PCSX2 lets it pass). Stored in kernel mode: at once through kseg1
   in an exception or interrupt handler, else by the kernel's syscall. */
void (*mx__kpoke32)(u32 a, u32 v);

static void w32(u32 a, u32 v) {
  u32 st = mx_status();
  if (((st >> 3) & 3) == 0 || (st & 6)) /* KSU kernel, EXL or ERL */
    R32(0xA0000000u | a) = v;
  else if (mx__kpoke32)
    mx__kpoke32(0xA0000000u | a, v);
  else
    R32(a) = v;
}

static int busy(void) { return (R32(MX_DMA_SIF1_CHCR) & CHCR_STR) != 0; }

/* The switch to the ring: only with channel 6 idle. A transfer of the
   BIOS's that the IOP does not take yet (the kernel's start, right after a
   loader's IOP) keeps it busy: waiting for it at the take hung the PS2
   before the IOP's reset. So the take is a wish, done by the first call
   that finds the channel idle; until then the BIOS's ring goes on. */
static int want;

static void switch_to_ring(void) {
  /* the ring is touched only uncached, but its lines may be in the data
     cache, dirty, from the start's clearing of .bss: written back later
     they would put zeros over the tags under the DMA. Dropped, not written
     back (a precaution: not seen to happen) */
  mx_dcache_invalidate(tags, sizeof tags);
  mx_dcache_invalidate(pkts, sizeof pkts);
  bios_tadr = R32(MX_DMA_SIF1_TADR);
  volatile u32 *t = uc(tags[USED]);
  t[0] = TAG_NEXT, t[1] = phys(tags[0]), t[2] = t[3] = 0;
  cur = USED - 1;
  w32(MX_DMA_SIF1_QWC, 0);
  w32(MX_DMA_SIF1_TADR, phys(tags[0]));
  own = 1;
}

void mx__sif_dma_take(void) {
  want = 1;

  int was = mx_di();
  if (!own && !busy())
    switch_to_ring();
  mx_irq_restore(was);
}

/* Back to the BIOS's ring, idle: what is under way finishes first, for at
   most about a second (an IOP that takes nothing more must not hang the
   ExecPS2 after it) */
void mx__sif_dma_give(void) {
  want = 0;
  if (!own)
    return;
  for (u32 i = 0; busy() && i < 300000000u / 16; i++)
    ;
  w32(MX_DMA_SIF1_CHCR, 0);
  w32(MX_DMA_SIF1_QWC, 0);
  w32(MX_DMA_SIF1_TADR, bios_tadr);
  own = 0;
}

/* the tag the channel is on (or was on last): the one before TADR */
static u32 hw_index(void) {
  u32 i = (R32(MX_DMA_SIF1_TADR) - phys(tags[0])) / 16;
  return i ? i - 1 : USED - 1;
}

/* tags free for new transfers: USED with nothing queued, USED - 1 with the
   last one under way (and cur, its tag, rebuilt) */
static u32 free_tags(void) {
  u32 hw = hw_index();
  if (hw == cur)
    return R32(MX_DMA_SIF1_QWC) ? USED - 1 : USED;
  if (hw < cur)
    return USED - 1 - (cur - hw);
  return hw - cur - 1;
}

static volatile u32 *next_tag(void) {
  cur = cur + 1 == USED ? 0 : cur + 1;
  written++;
  return uc(tags[cur]);
}

/* the quadwords of a transfer copied into its packet: always, as the BIOS
   does, also from a physical address (the data of an RPC call). A caller
   may use its buffer again as soon as the call returns; a REF to it was
   read by the DMA later, and the IOP got another call's data and answered
   no more (on the PS2; PCSX2 does the DMA at once) */
static u32 copied(const struct sif_dma *t) {
  u32 qwc = ((u32)t->size + 15) / 16;
  return qwc < COPY ? qwc : COPY;
}

/* where the copy reads: an uncached alias as given, a physical address
   through the uncached alias (user mode: no kseg1) */
static const volatile u32 *copy_from(u32 src) {
  if (src & 0x80000000u) /* the scratchpad (a DMA address's SPR bit) */
    return (const volatile u32 *)(uintptr_t)(0x70000000u | (src & 0x3FFFu));
  return (const volatile u32 *)(uintptr_t)(src >= 0x20000000u ? src : (src | 0x20000000u));
}

static void put(const struct sif_dma *t, int last) {
  u32 qwc = ((u32)t->size + 15) / 16, n = copied(t), rest = qwc - n;
  u32 end = last ? TAG_REFE : TAG_REF, irq = (t->attr & ATTR_INT_I) ? TAG_IRQ : 0;
  u32 iop = (t->dest & 0x00FFFFFFu) | ((t->attr & ATTR_INT_O) ? 0x40000000u : 0) |
            ((t->attr & ATTR_ERT) ? 0x80000000u : 0);
  volatile u32 *e = next_tag();
  volatile u32 *p = uc(pkts[cur]);
  p[0] = iop, p[1] = qwc * 4, p[2] = p[3] = 0;
  const volatile u32 *from = copy_from(t->src);
  for (u32 i = 0; i < 4 * n; i++)
    p[4 + i] = from[i];
  e[1] = phys(pkts[cur]);
  if (!rest) {
    e[0] = end | irq | (1 + n);
    return;
  }
  e[0] = TAG_REF | (1 + n);
  e = next_tag();
  e[0] = end | irq | rest;
  /* the DMA address of the rest: an uncached alias made physical, a
     physical or scratchpad (SPR bit) one as it is */
  u32 a = (t->src & 0xE0000000u) == 0x20000000u ? t->src & 0x1FFFFFFFu : t->src;
  e[1] = a + 16 * n;
}

/* one tag a transfer that fits in its packet, else two (ATTR_TAG, the IOP
   tag in the data, is not used by maxsdk: not taken) */
static int tags_for(const struct sif_dma *t, int n) {
  int k = 0;
  for (int i = 0; i < n; i++) {
    if (t[i].attr & ATTR_TAG)
      return 1 << 20;
    k += ((u32)t[i].size + 15) / 16 > copied(&t[i]) ? 2 : 1;
  }
  return k;
}

/* the BIOS's SifSetDma: 0 with no room (try again), else an id for
   mx__sif_dma_stat; intr: called by an interrupt handler (the BIOS's iSifSetDma
   while the channel is the BIOS's) */
u32 mx__sif_dma_set(const void *transfers, int n, int intr) {
  const struct sif_dma *t = transfers;
  /* in an interrupt handler interrupts are off already, and must stay off
     after: an EI there lets the next interrupt in while the handler runs */
  int was = intr ? 0 : mx_di();
  if (!own && want && !busy())
    switch_to_ring();
  if (!own) {
    mx_irq_restore(was);
    return intr ? mx_bios_isif_set_dma((void *)transfers, n)
                : mx_bios_sif_set_dma((void *)transfers, n);
  }
  /* the channel stopped where it is */
  R32(MX_D_ENABLEW) = R32(MX_D_ENABLER) | CPND;
  w32(MX_DMA_SIF1_CHCR, 0);
  (void)R32(MX_DMA_SIF1_CHCR);
  R32(MX_D_ENABLEW) = R32(MX_D_ENABLER) & ~CPND;
  u32 room = free_tags(), need = (u32)tags_for(t, n), id = 0;
  if (need <= room) {
    if (room != USED) {
      volatile u32 *e = uc(tags[cur]);
      if (room == USED - 1) {
        /* the last one under way: its rest, a REF, from the channel's
           registers, and the channel started from it again */
        e[0] = (e[0] & 0x8C000000u) | TAG_REF | R32(MX_DMA_SIF1_QWC);
        e[1] = R32(MX_DMA_SIF1_MADR);
        w32(MX_DMA_SIF1_QWC, 0);
        w32(MX_DMA_SIF1_TADR, phys(tags[cur]));
      } else {
        e[0] |= TAG_REF; /* REFE: goes on */
      }
    }
    for (int i = 0; i < n; i++)
      put(&t[i], i == n - 1);
    id = written ? written : 1; /* the last tag's number: never 0 */
  }
  /* (again) running, also with no room: what was queued goes on */
  w32(MX_DMA_SIF1_CHCR, R32(MX_DMA_SIF1_CHCR) | CHCR_GO);
  (void)R32(MX_DMA_SIF1_CHCR);
  mx_irq_restore(was);
  return id;
}

/* the BIOS's SifDmaStat: >= 0 while the transfer of id is queued or under
   way, -1 when it is done */
int mx__sif_dma_stat(u32 id) {
  if (!own)
    return mx_bios_sif_dma_stat(id);
  if (!busy())
    return -1;
  int was = mx_di();
  /* the number of the tag the channel is on */
  u32 hw = hw_index(), back = cur >= hw ? cur - hw : cur + USED - hw;
  u32 on = written - back;
  mx_irq_restore(was);
  return (int)(id - on) >= 0 ? 0 : -1;
}

/* SIF0 (channel 5): the IOP's packets received in chain mode, its tags'
   IRQ on (the BIOS's SifSetDChain; intr: from an interrupt handler). Like
   all below, the BIOS's own call while SIF1 is not taken: a program on the
   BIOS's threads does what it did before (on the PS2, not on PCSX2, the
   kernel's memory was found overwritten after such a program wrote the
   channel and the registers itself). */
void mx__sif_dchain(int intr) {
  if (!own) {
    if (intr)
      mx_bios_isif_set_dchain();
    else
      mx_bios_sif_set_dchain();
    return;
  }
  int was = intr ? 0 : mx_di(); /* as in mx__sif_dma_set */
  w32(MX_DMA_SIF0_CHCR, 0);
  w32(MX_DMA_SIF0_QWC, 0);
  w32(MX_DMA_SIF0_CHCR, CHCR_GO);
  (void)R32(MX_DMA_SIF0_CHCR);
  mx_irq_restore(was);
}

void mx__sif_stop_dma(void) {
  if (!own) {
    mx_bios_sif_stop_dma();
    return;
  }
  w32(MX_DMA_SIF0_CHCR, 0);
  w32(MX_DMA_SIF0_QWC, 0);
  (void)R32(MX_DMA_SIF0_QWC);
}

/* The SIF's registers (the BIOS's SifSetReg and SifGetReg): 1 MSCOM (the
   EE's address of the IOP's packets), 2 SMCOM, 3 MSFLG, 4 SMFLG; a flag is
   read until two reads agree (the IOP may be changing it). Above them,
   with bit 31, the BIOS's software registers. */
#define SIF_MSCOM 0x1000F200u
#define SIF_SMCOM 0x1000F210u
#define SIF_MSFLG 0x1000F220u
#define SIF_SMFLG 0x1000F230u

static u32 flag(u32 a) {
  u32 v;
  do {
    v = R32(a);
    for (int i = 0; i < 16; i++)
      __asm__ volatile("nop");
  } while (v != R32(a));
  return v;
}

u32 mx__sif_get_reg(u32 reg) {
  if (!own)
    return (u32)mx_bios_sif_get_reg(reg);
  switch (reg) {
  case 1: return R32(SIF_MSCOM);
  case 2: return R32(SIF_SMCOM);
  case 3: return flag(SIF_MSFLG);
  case 4: return flag(SIF_SMFLG);
  }
  return (u32)mx_bios_sif_get_reg(reg);
}

u32 mx__sif_set_reg(u32 reg, u32 v) {
  if (!own)
    return (u32)mx_bios_sif_set_reg(reg, (s32)v);
  switch (reg) {
  case 1: R32(SIF_MSCOM) = v; return R32(SIF_MSCOM);
  case 3: R32(SIF_MSFLG) = v; return flag(SIF_MSFLG);
  case 4: R32(SIF_SMFLG) = v; return flag(SIF_SMFLG);
  }
  return (u32)mx_bios_sif_set_reg(reg, (s32)v);
}

/* for a report of a stuck SIF: own, want, written, cur, then SIF1's CHCR,
   TADR, QWC and MADR */
void mx__sif_dma_state(u32 s[8]) {
  s[0] = (u32)own, s[1] = (u32)want, s[2] = written, s[3] = cur;
  s[4] = R32(MX_DMA_SIF1_CHCR), s[5] = R32(MX_DMA_SIF1_TADR), s[6] = R32(MX_DMA_SIF1_QWC);
  s[7] = R32(MX_DMA_SIF1_MADR);
}
