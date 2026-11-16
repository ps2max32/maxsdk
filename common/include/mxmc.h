/* mxmc: MXOS's memory card driver (iop/sio/mxmc, a fork of ps2sdk's mcman).
   Besides mcman's interface (the device mc, the library mcman for MCSERV) it
   exports the library "mxmc" for the other IOP modules, found at run time
   (loadcore's QueryLibraryEntryTable, version 1.1), so a module works with
   the ROM's or ps2sdk's mcman too, without it.

   MXMC_CARD: int mxmc_card(int port, struct mxmc_card *c), port 0 or 1 (the
   slots mc0, mc1). Whether a card is there, as mcman's own check before it
   opens a file (no file is taken: a card whose files are all open is still
   seen), and for a card that is ready its size and the bytes free. The free
   space is counted again only after the card was written or changed, so a
   call is a short SIO2 transfer. Returns c->state. */
#ifndef MXMC_H
#define MXMC_H

#define MXMC_CARD 4 /* the entry in the library's table */

enum {
  MXMC_NONE = 0,        /* no card, or it does not answer */
  MXMC_READY = 1,       /* a formatted card */
  MXMC_UNFORMATTED = 2, /* a card without a file system */
};

struct mxmc_card {
  int state;          /* MXMC_* */
  int type;           /* mcman's: 1 PS1, 2 PS2, 3 PDA */
  unsigned int size;  /* bytes; 0 when not known */
  unsigned int free;  /* bytes free */
};

typedef int mxmc_card_fn(int port, struct mxmc_card *c);

#endif
