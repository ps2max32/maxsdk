/* mxdev9: the expansion bay (DEV9) for maxsdk's IOP drivers (docs/iop-drivers.md).
   Library "mxdev9". */
#ifndef MXDEV9_H
#define MXDEV9_H

#include <types.h>
#include <irx.h>

/* SPEED interrupt bits (speedregs.h, smapregs.h): ATA 0-1, SMAP 2-6 */
#define MXDEV9_INTR_BITS 16

/* An interrupt handler for some SPEED bits, in interrupt context, with those of
   its bits that are set. It clears their source (or masks them with
   mxdev9_intr_off) before it returns. */
typedef void mxdev9_intr_fn(void *arg, u32 bits);

/* A DMA request on IOP channel 8. The caller owns it until done has run. */
struct mxdev9_dma {
  struct mxdev9_dma *next; /* mxdev9's */
  void *buf;               /* 4-byte aligned */
  u16 blocks;              /* blocks of block_words 32-bit words */
  u8 block_words;
  u8 device;               /* 0 ATA, 1 SMAP */
  u8 to_mem;               /* 1: device to memory */
  u8 pad[3];
  /* interrupts off, right before the transfer: set up the device's FIFO */
  void (*start)(struct mxdev9_dma *d);
  /* interrupt context, right after it; may queue more */
  void (*done)(struct mxdev9_dma *d);
  void *arg;
};

/* SPEED capabilities (SPD_CAPS_*) */
u32 mxdev9_caps(void);
/* the MAC address from the EEPROM: 0, or -1 if it has none or a bad checksum */
int mxdev9_mac(u8 *mac);
/* a handler for the bits of mask (at most 4 handlers): 0, or -1 */
int mxdev9_intr(u32 mask, mxdev9_intr_fn *fn, void *arg);
/* from any context */
void mxdev9_intr_on(u32 mask);
void mxdev9_intr_off(u32 mask);
/* queue a request, from any context: it starts when the channel is free, and
   its end is the channel's interrupt. For long transfers (ATA). */
void mxdev9_dma(struct mxdev9_dma *d);
/* A short transfer (a frame) from a thread, without waiting for an interrupt: begin starts
   it if the channel is free (0; -1 if not), the caller works on meanwhile, and
   end waits for the rest of it. done is not called; start is. On the IOP an
   interrupt and a thread switch cost more than the 40 us a frame's DMA takes
   (docs/iop-drivers.md). */
int mxdev9_dma_begin(struct mxdev9_dma *d);
void mxdev9_dma_end(struct mxdev9_dma *d);
void mxdev9_led(int on);
/* a function for power-off (at most 4), called by mxdev9_off */
int mxdev9_on_off(void (*fn)(void));
void mxdev9_off(void);
/* counters: DMA requests, those that waited for the channel, the most queued at
   once, DEV9 interrupts, rounds of their dispatch, reads of CHCR in
   mxdev9_dma_end while the transfer still ran */
void mxdev9_stats(u32 *out6);

#endif
