#ifndef MXPAD_INT_H
#define MXPAD_INT_H

/* between mxpad.c (the pads on USB) and bt.c (a Bluetooth dongle) */

#include <types.h>

/* bt.c: the dongle's driver; pair_all is called when a dongle comes up, so
   that the pads on USB are paired with it (mxpad.c) */
void bt_init(void (*pair_all)(void));
/* the dongle's address (HCI byte order: least significant byte first): 1
   when a dongle is up, else 0 */
int bt_host(u8 addr[6]);
/* a new link key for the pad at addr (HCI byte order), kept for when it
   connects over Bluetooth */
void bt_new_key(const u8 addr[6], u8 key[16]);

/* mxpad.c: a slot for a Bluetooth pad of a kind (MXPAD_*): the slot, or < 0
   when all are taken; its input reports in the USB layout (report 0x01, the
   id first); the slot freed; the slot's colour */
int mxpad_open(int kind);
void mxpad_input(int slot, const u8 *report, u32 n);
void mxpad_close(int slot);
void mxpad_slot_rgb(int slot, u8 rgb[3]);
/* bt.c: a pad that will connect without a link key (the DS3), paired over USB */
void bt_known(const u8 addr[6], int kind);

#endif
