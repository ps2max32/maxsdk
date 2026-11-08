# IOP probes, with maxsdk's IOP toolchain (../../iop/iop.mk): clang for
# mipsel-none-irx, which links with ld.lld --irx into the IRX, the import
# tables from the ILB files.
#
#   make [OUT=<dir>]    <dir>/r3000a.irx, <dir>/net.irx, <dir>/rawnet.irx, <dir>/usbraw.irx, <dir>/fileraw.irx,
#                       <dir>/rawnet_mx.irx (rawnet.c on mxeth),
#                       <dir>/hidprobe.irx (pads and Bluetooth dongles on USB, probe/p35_hid.c)
#
# Run with the EE side, probe/p25_iop.c (see docs/iop-core.md).

include ../../iop/iop.mk

OUT    ?= $(PS2MAX)/build/libs/maxsdk/probe/iop
CFLAGS  = $(IOP_CFLAGS) -O2 -Werror $(IOP_INCS) -I. \
          -I../../iop/dev9/mxeth/include -I../../iop/dev9/mxdev9/include
PROBES  = r3000a net rawnet usbraw fileraw rawnet_mx hidprobe

all: $(PROBES:%=$(OUT)/%.irx)

$(OUT)/%.o: %.c
	@mkdir -p $(OUT)
	$(IOP_CC) $(CFLAGS) -c $< -o $@

$(OUT)/%.o: %.S
	@mkdir -p $(OUT)
	$(IOP_CC) $(CFLAGS) -c $< -o $@

$(OUT)/rawnet_mx.o: rawnet.c ../../iop/dev9/mxeth/include/mxeth.h ../../iop/dev9/mxdev9/include/mxdev9.h

$(OUT)/r3000a.irx: $(OUT)/r3000a_asm.o
$(OUT)/%.irx: $(OUT)/%.o $(MAXSDK_ILBS) $(IOP_ILB_RSP)
	$(IOP_CC) -s -o $@ $(filter %.o,$^) $(call iop_link_libs)

.PHONY: all
