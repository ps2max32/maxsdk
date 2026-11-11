#ifndef MX_SIFDMA_H
#define MX_SIFDMA_H

#include <mx/types.h>

/* ee/src/base/sifdma.c: SIF DMA without the BIOS. The kernel takes channel
   6 (SIF1) at its start and gives it back before ExecPS2; until then, and
   after, all of them go to the BIOS. */
void mx__sif_dma_take(void);
void mx__sif_dma_give(void);
u32 mx__sif_dma_set(const void *transfers, int n, int intr);
int mx__sif_dma_stat(u32 id);
void mx__sif_dchain(int intr);
void mx__sif_stop_dma(void);
u32 mx__sif_get_reg(u32 reg);
u32 mx__sif_set_reg(u32 reg, u32 v);
/* a store in kernel mode (the kernel's syscall), for the registers of DMA
   channels 5 to 7, which the BIOS's TLB maps read only; set by the kernel */
extern void (*mx__kpoke32)(u32 a, u32 v);
extern u32 (*mx__kpeek32)(u32 a);
/* own, want, written, cur, SIF1's CHCR, TADR, QWC, MADR (a stuck SIF's report) */
void mx__sif_dma_state(u32 s[8]);

#endif
