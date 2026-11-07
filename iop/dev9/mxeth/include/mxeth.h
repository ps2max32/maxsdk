/* mxeth: the network adapter's Ethernet (SMAP) for maxsdk's IOP drivers
   (docs/iop-drivers.md). Library "mxeth".

   mxeth owns the receive buffers and offers each frame to its clients in
   order of prio (the lowest first), in its driver thread. Frames to send are
   queued from any context and leave from interrupts. */
#ifndef MXETH_H
#define MXETH_H

#include <types.h>
#include <irx.h>

/* the most a frame holds: 1514 bytes and its rounding to 4 */
#define MXETH_FRAME_SIZE 1520

struct mxeth_frame {
  struct mxeth_frame *next; /* mxeth's while queued, the client's once taken */
  u16 len;
  u16 nr; /* the buffer's number */
  u32 user[6]; /* the client's */
  u8 data[MXETH_FRAME_SIZE] __attribute__((aligned(16)));
};

struct mxeth_client {
  struct mxeth_client *next; /* mxeth's */
  int prio;
  /* the driver thread: 1 if the client takes the frame (it calls mxeth_free
     later), 0 to pass it on */
  int (*rx)(struct mxeth_frame *f, void *arg);
  /* the link thread: the link came up (mode: MXETH_LINK_*) or went down (0) */
  void (*link)(int mode, void *arg);
  void *arg;
};

struct mxeth_tx {
  struct mxeth_tx *next; /* mxeth's */
  const void *data;      /* 4-byte aligned is fastest: else it goes by PIO */
  u16 len;               /* at most 1514 */
  u16 pad;
  /* the driver thread, once the data is in the adapter (or dropped without a
     link): the caller may reuse it */
  void (*done)(struct mxeth_tx *t);
  void *arg;
};

#define MXETH_LINK_10HDX 1
#define MXETH_LINK_10FDX 2
#define MXETH_LINK_100HDX 4
#define MXETH_LINK_100FDX 8
#define MXETH_LINK_FLOW 0x40

/* counters (mxeth_stats) */
enum {
  MXETH_RX_FRAMES,
  MXETH_RX_BYTES,
  MXETH_RX_ERRORS,  /* bad frames from the MAC */
  MXETH_RX_STARVED, /* times a frame had to wait for a free buffer */
  MXETH_RX_OVERRUN, /* RXDNV: the adapter dropped frames */
  MXETH_RX_UNTAKEN, /* frames no client took */
  MXETH_TX_FRAMES,
  MXETH_TX_ERRORS,
  MXETH_TX_DROPPED, /* sent without a link */
  MXETH_DMAEN_SPINS, /* reads of a FIFO's DMA enable still set after the DMA's end */
  MXETH_WAKES,      /* driver thread wake-ups */
  MXETH_IRQS,       /* SMAP interrupts */
  MXETH_RX_DMAS,    /* receive DMAs (frames of 64 bytes or more) */
  /* built with MXETH_PROFILE: IOP timer ticks (36.864 MHz) */
  MXETH_T_IRQ,    /* in the SMAP's interrupt */
  MXETH_T_CLIENTS, /* in the clients' rx */
  MXETH_T_DMA,    /* waiting for the rest of a receive DMA */
  MXETH_T_THREAD, /* in the driver thread's work, all of it */
  MXETH_STATS
};

int mxeth_attach(struct mxeth_client *c);
/* any context */
void mxeth_free(struct mxeth_frame *f);
/* any context: 0, or -1 without a link (done is called, the frame dropped) */
int mxeth_send(struct mxeth_tx *t);
void mxeth_mac(u8 *mac);
/* MXETH_LINK_* or 0 */
int mxeth_link(void);
u32 mxeth_free_frames(void);
void mxeth_stats(u32 *out);

#define mxeth_IMPORTS_start DECLARE_IMPORT_TABLE(mxeth, 1, 1)
#define mxeth_IMPORTS_end END_IMPORT_TABLE
#define I_mxeth_attach DECLARE_IMPORT(4, mxeth_attach)
#define I_mxeth_free DECLARE_IMPORT(5, mxeth_free)
#define I_mxeth_send DECLARE_IMPORT(6, mxeth_send)
#define I_mxeth_mac DECLARE_IMPORT(7, mxeth_mac)
#define I_mxeth_link DECLARE_IMPORT(8, mxeth_link)
#define I_mxeth_free_frames DECLARE_IMPORT(9, mxeth_free_frames)
#define I_mxeth_stats DECLARE_IMPORT(10, mxeth_stats)

#endif
