# USB mass storage for bdm, from ps2sdk's usbmass_bd: a whole command goes to the
# USB driver at once, see src/usb_mass.c.
#
# EXTRA_CFLAGS=-DUSB_MASS_FAULT=<1|2> OUT=<dir>: makes reads fail, to test the
# recovery (bad CBW, sector past the end); with -DUSB_MASS_FAULT_CMD=0x2a
# writes instead. Never for use.

MODULE = usbmass_bd
SRCS   = main.c usb_mass.c scsi.c
CFLAGS_MODULE = -Isrc/include

include ../../Rules.mk
