# The memory cards for MXOS: ps2sdk's mcman (its XMCMAN, the one MXOS loaded
# before), forked (libs/maxsdk/docs/mxmc-plan.md). It keeps mcman's export
# table (library mcman 2.3), so MCSERV binds to it, and its device mc.
# AFL 2.0, as ps2sdk: LICENSE.

MODULE = mxmc
SRCS   = main.c mcdev.c mciomanx_backing.c mcsio2.c ps2mc_fio.c ps1mc_fio.c
CFLAGS_MODULE = -DBUILDING_XMCMAN
# sio2man's newer entries (ps2sdk's xsio2man.h): before the ILBs made from
# ps2sdk's headers, which have only the base table's
IOP_LDFLAGS = $(abspath src/xsio2man.ilb)

include ../../Rules.mk
