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
   byte a slot; out: s32 0. Before it, the slot's own number. Not answered
   (-1) by an mxpad.irx before it: the version stays 1. */
#define MXPAD_PLAYERS 4

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
#define MXPAD_TOUCH 2 /* a finger on the touchpad: tx, ty */

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
  unsigned int reserved2;
};
/* SIF DMA moves 16 bytes at a time: a record of another size would write over the next */
_Static_assert(sizeof(struct mxpad_record) == 32, "struct mxpad_record");

struct mxpad_area {
  struct mxpad_record r[MXPAD_PADS][2];
};

#endif
