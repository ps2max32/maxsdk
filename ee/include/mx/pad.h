#ifndef MX_PAD_H
#define MX_PAD_H

/* The controllers, through the IOP's pad driver (rom0:SIO2MAN and
   rom0:PADMAN, loaded unless a pad driver is already running). The IOP
   writes each port's state into EE memory every frame; reading it is a
   memory read. */

#include <mx/types.h>

#define MX_PAD_SELECT 0x0001
#define MX_PAD_L3 0x0002
#define MX_PAD_R3 0x0004
#define MX_PAD_START 0x0008
#define MX_PAD_UP 0x0010
#define MX_PAD_RIGHT 0x0020
#define MX_PAD_DOWN 0x0040
#define MX_PAD_LEFT 0x0080
#define MX_PAD_L2 0x0100
#define MX_PAD_R2 0x0200
#define MX_PAD_L1 0x0400
#define MX_PAD_R1 0x0800
#define MX_PAD_TRIANGLE 0x1000
#define MX_PAD_CIRCLE 0x2000
#define MX_PAD_CROSS 0x4000
#define MX_PAD_SQUARE 0x8000

/* The home button: the PS button of a DualShock 3, DualShock 4 or DualSense
   (on USB); a pad without one gives it for this combination held together,
   in place of the combination's four buttons. Under mx-kernel it opens the
   kernel's home menu (a program on maxsdk's frame pipeline) or ends the
   program; elsewhere it ends the program (mx_exit(0)) when the pad is read: a
   way back to the loader from any program, whatever its menus offer. */
#define MX_PAD_HOME 0x10000
#define MX_PAD_QUIT (MX_PAD_SELECT | MX_PAD_START | MX_PAD_L1 | MX_PAD_R1)

/* Open port 0 and 1 (slot 0). Returns < 0 if the pad driver is not there. */
int mx_pad_init(void);
/* 1 if a pad on port (0/1) is connected and its state is being read. */
int mx_pad_connected(int port);
/* The buttons held on port (0/1), MX_PAD_* bits (MX_PAD_HOME included); 0 if no
   pad is connected. */
u32 mx_pad_buttons(int port);
/* Buttons pressed since the previous call for this port. */
u32 mx_pad_pressed(int port);
/* The sticks of port (0/1) when the pad is in analog mode: st[0..3] = right
   x, right y, left x, left y, 0 .. 255 with the middle at 128 (y grows
   downwards). Returns 1, or 0 with 128s when the pad is digital or not
   connected. A DualShock is put into analog mode (locked) by the first calls
   after it is connected. */
int mx_pad_sticks(int port, u8 st[4]);

/* The motors of the pad on port (0..): small (the right, light one: 0 off,
   else on; a DS3's is on or off only), big (the left, heavy one: 0..255).
   They run until the next call; the kernel stops them when the program ends.
   A SIO2 DualShock is put in DualShock (analog) mode for them. Returns 0, or
   < 0 when there is no pad there (or it is being found again: the motors
   then start when it is). Under mx-kernel (version 11) only. */
int mx_pad_rumble(int port, u32 small, u32 big);

/* ---- The controllers, for the shell (kernel version 28) ----------------
   A program's port (0..) as mx_pad_buttons counts them: the SIO2 pads, then
   the USB and Bluetooth pads in the order they came. The kernel also reads
   these settings (<mx/setting.h>): pad.idle_off (minutes: a Bluetooth pad
   on battery that sent nothing for that long is turned off; 0 or unset:
   never), pad.brightness (the light bars, 0..100, 25 by default) and
   pad.<id>.colour (a pad's light bar, "rrggbb"; id as 8 hex digits, as
   mx_pad_info gives it; unset: its player's colour). On libmx the calls
   return -1 (mx_pad_paired and mx_pad_gen 0). */

/* the ports (0 .. MX_PAD_PORTS - 1): the two controller ports and four USB
   or Bluetooth pads */
#define MX_PAD_PORTS 6

enum {
  MX_PAD_KIND_NONE,
  MX_PAD_KIND_PS2, /* a pad in a controller port (DualShock 2, ...) */
  MX_PAD_KIND_DS3,
  MX_PAD_KIND_DS4,
  MX_PAD_KIND_DS5, /* DualSense */
};
enum {
  MX_PAD_LINK_PORT, /* a controller port */
  MX_PAD_LINK_USB,
  MX_PAD_LINK_BT,
};
#define MX_PAD_INFO_CABLE 1    /* on a USB cable (also while on Bluetooth) */
#define MX_PAD_INFO_CHARGING 2 /* charging, not yet full */

struct mx_pad_info {
  u8 kind;    /* MX_PAD_KIND_* */
  u8 link;    /* MX_PAD_LINK_* */
  u8 battery; /* 0..10, 255: not known (a controller port's) */
  u8 flags;   /* MX_PAD_INFO_* */
  u8 hw;      /* its controller port (0/1), or mxpad.irx's slot */
  u8 rgb[3];  /* its light bar now (DS4, DualSense), 0 0 0: none */
  u32 id;     /* its own, for its settings (a hash of its Bluetooth address);
                 0: a controller port's, or not known yet */
};
/* The pad on port: 1, 0 with none there, -1 past the last port */
int mx_pad_info(int port, struct mx_pad_info *i);
/* A counter that changes when anything mx_pad_info, mx_pad_paired or
   mx_pad_pair_state tell changes (a pad came or went, its battery,
   charging, kind or id, a pairing); MX_EVENT_PAD (status: this counter)
   is the hint */
u32 mx_pad_gen(void);
/* The shell only. A Bluetooth pad's link ended: it turns itself off. 0, or
   -1 (none there, not on Bluetooth) */
int mx_pad_disconnect(int port);

/* Pairing over Bluetooth (the shell only): for seconds (0: stop), a pad in
   pairing mode (DualSense: Create + PS, DS4: Share + PS) that is not paired
   is looked for, paired and connected. A pad plugged in by USB pairs by
   itself. mx_pad_pair_state tells the progress; each change is also an
   MX_EVENT_PAD (<mx/proc.h>). */
int mx_pad_pair(u32 seconds);
enum {
  MX_PAD_PAIR_IDLE,       /* not looking */
  MX_PAD_PAIR_SEARCHING,  /* looking for a pad in pairing mode */
  MX_PAD_PAIR_CONNECTING, /* one found: pairing with it */
  MX_PAD_PAIR_DONE,       /* id paired (over Bluetooth, or by USB) */
  MX_PAD_PAIR_NOT_FOUND,  /* the time ran out without one */
  MX_PAD_PAIR_FAILED,     /* id did not pair */
  MX_PAD_PAIR_NO_DONGLE,  /* no Bluetooth dongle */
};
struct mx_pad_pairing {
  u32 state; /* MX_PAD_PAIR_* */
  u32 seq;   /* changes with every change of state */
  u32 id;    /* the pad (CONNECTING, DONE, FAILED), else 0 */
  u8 kind;   /* MX_PAD_KIND_*, NONE when not known yet */
  u8 pad[3];
};
int mx_pad_pair_state(struct mx_pad_pairing *p);

/* The pads paired with the Bluetooth dongle (at most 8): how many into p */
struct mx_pad_paired {
  u32 id;
  u8 kind;      /* MX_PAD_KIND_*, NONE when not known */
  u8 connected; /* 1: on Bluetooth now */
  u8 addr[6];   /* its address, the most significant byte first */
};
int mx_pad_paired(struct mx_pad_paired *p, int max);
/* The shell only: a paired pad taken out (its link ended); it pairs again
   by USB or in pairing mode. 0, or -1 */
int mx_pad_forget(u32 id);

#endif
