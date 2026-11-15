#ifndef MXPAD_H
#define MXPAD_H

/* mxpad.irx (iop/usb/mxpad): DualShock 3, DualShock 4 and DualSense pads on
   USB, for the EE. Each pad has a slot (0 .. MXPAD_PADS-1) and two records in
   an area of EE memory that the EE gives with MXPAD_AREA; the IOP writes the
   older of the two by SIF DMA whenever the pad's state changes (and once when
   the area is given), so the newer (the higher seq) is the pad's state.
   docs/pad-bt-plan.md has the reports these come from. */

#define MXPAD_SID 0x4D585044 /* 'MXPD' */
#define MXPAD_VERSION 1
#define MXPAD_PADS 4

/* in: u32 the area's physical EE address (64-byte aligned, 0: stop writing);
   out: u32 MXPAD_VERSION, u32 MXPAD_PADS */
#define MXPAD_AREA 1
/* in: u32 slot, u8 small motor (0/1 on a DS3), u8 big motor (0..255), u8 red,
   green, blue of the light bar (all 0: the slot's colour); out: s32 0, or < 0
   with no pad in the slot */
#define MXPAD_OUTPUT 2

/* in: u32 seconds: for that long a pad in pairing mode (DualSense: Create +
   PS, DS4: Share + PS) that is not paired yet is looked for over Bluetooth,
   paired and connected; out: s32 0 */
#define MXPAD_PAIR 3

/* in: u8 the player number (0: player 1) shown by the pad in each slot, one
   byte a slot; out: s32 0. Before it, the slot's own number. */
#define MXPAD_PLAYERS 4

/* added for kernel version 28 (<mx/pad.h>'s controllers); the version
   stays 1, so that a kernel before it still takes this mxpad.irx (an older
   one answers these -1) */
/* in: u32 slot; out: s32 0, or < 0 (no pad there, or not on Bluetooth): its
   link ends, and the DS3, DS4 and DualSense turn themselves off */
#define MXPAD_DISCONNECT 5
/* out: u32 how many, then that many struct mxpad_paired: the pads paired
   with the dongle (BTPADS.DAT), at most MXPAD_PAIRED_MAX */
#define MXPAD_PAIRED 6
/* in: 6 bytes, a paired pad's address (HCI byte order); out: s32 0, or < 0
   (not paired): its pairing taken out (and its link ended) */
#define MXPAD_FORGET 7
/* out: struct mxpad_pairing: MXPAD_PAIR's progress */
#define MXPAD_PAIR_STATE 8
/* in: 3 bytes a slot (red, green, blue; all 0: the player's colour), 4
   slots; out: s32 0. The light bar of each slot's pad from now on */
#define MXPAD_COLOURS 9

/* the pad's kind, 0: no pad in the slot */
#define MXPAD_NONE 0
#define MXPAD_DS3 1
#define MXPAD_DS4 2
#define MXPAD_DS5 3

/* buttons: the low 16 bits are <mx/pad.h>'s MX_PAD_*, held = 1 */
#define MXPAD_PS 0x10000
#define MXPAD_TOUCHPAD 0x20000 /* the touchpad's click */
#define MXPAD_MUTE 0x40000

/* flags */
#define MXPAD_CABLE 1
#define MXPAD_TOUCH 2    /* a finger on the touchpad: tx, ty */
#define MXPAD_CHARGING 4 /* on a cable and charging (not yet full) */
#define MXPAD_BT 8       /* on Bluetooth (else on USB) */

struct mxpad_record { /* 32 bytes */
  unsigned int seq;     /* written last of the two when higher; 0: never written */
  unsigned int buttons; /* MX_PAD_* and MXPAD_PS, ... */
  unsigned char kind;   /* MXPAD_* */
  unsigned char battery; /* 0..10, 255: not known */
  unsigned char flags;
  unsigned char pressure; /* 1: the DS3's pressure (not in this record yet) */
  unsigned char lx, ly, rx, ry; /* 0..255, the middle at 128, y grows downwards */
  unsigned char l2, r2;         /* the triggers, 0..255 */
  unsigned short tx, ty;        /* the first finger (DS4 0..1919 x 0..942, DualSense 0..1919 x 0..1079) */
  unsigned short reserved;
  unsigned int connected;       /* the pad's number in the order pads came (1, 2, ...) */
  unsigned int id;              /* the pad's own (mxpad_id of its address); 0: not known yet */
};
/* SIF DMA moves 16 bytes at a time: a record of another size would write over the next */
_Static_assert(sizeof(struct mxpad_record) == 32, "struct mxpad_record");

/* MXPAD_PAIRED */
#define MXPAD_PAIRED_MAX 8
struct mxpad_paired {
  unsigned char addr[6]; /* HCI byte order */
  unsigned char kind;    /* MXPAD_* (0: not known) */
  unsigned char connected; /* 1: on Bluetooth now */
};

/* MXPAD_PAIR_STATE */
enum {
  MXPAD_PAIR_IDLE,       /* not looking */
  MXPAD_PAIR_SEARCHING,  /* looking for a pad in pairing mode */
  MXPAD_PAIR_CONNECTING, /* one found (addr): pairing with it */
  MXPAD_PAIR_DONE,       /* addr paired (over Bluetooth, or over USB with the cable) */
  MXPAD_PAIR_NOT_FOUND,  /* the time ran out without one */
  MXPAD_PAIR_FAILED,     /* addr did not pair */
  MXPAD_PAIR_NO_DONGLE,  /* no Bluetooth dongle */
};
struct mxpad_pairing {
  unsigned int state; /* MXPAD_PAIR_* */
  unsigned int seq;   /* changes with every change of state */
  unsigned char addr[6], kind, pad;
};

/* A pad's id: FNV-1a of its address, never 0 */
static inline unsigned int mxpad_id(const unsigned char addr[6]) {
  unsigned int h = 2166136261u;
  for (int i = 0; i < 6; i++)
    h = (h ^ addr[i]) * 16777619u;
  return h ? h : 1;
}

struct mxpad_area {
  struct mxpad_record r[MXPAD_PADS][2];
};

#endif
