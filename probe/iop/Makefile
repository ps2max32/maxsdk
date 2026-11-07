# IOP probes: clang for the code, the IOP gcc for what has no replacement yet (the
# linker with ps2sdk's link script, iopfixup), as tools/mxlink does.
#
#   make [OUT=<dir>]    <dir>/r3000a.irx, <dir>/net.irx, <dir>/rawnet.irx, <dir>/usbraw.irx, <dir>/fileraw.irx,
#                       <dir>/rawnet_mx.irx (rawnet.c on mxeth)
#
# Run with the EE side, probe/p25_iop.c (see docs/iop-core.md).

PS2MAX ?= $(abspath $(CURDIR)/../../../..)
OUT    ?= $(PS2MAX)/build/libs/maxsdk/probe/iop
LLVM   ?= $(PS2MAX)/build/toolchain/llvm/feature/bin
ifndef PS2MAX_IOP_BIN
$(error source envsetup.sh first: PS2MAX_IOP_BIN is not set)
endif

CC      = $(LLVM)/clang --target=mipsel-none-elf -mcpu=r3000a
CFLAGS  = -D_IOP -fno-builtin -ffreestanding -G0 -O2 -Wall -Werror -Wno-unused-but-set-global \
          -msoft-float -mno-abicalls -fno-pic -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -I$(PS2MAX_IOP_SDK)/iop/include -I$(PS2MAX_IOP_SDK)/common/include -I. \
          -I../../iop/dev9/mxeth/include -I../../iop/dev9/mxdev9/include
OBJS    = $(OUT)/r3000a.o $(OUT)/r3000a_asm.o $(OUT)/imports.o

all: $(OUT)/r3000a.irx $(OUT)/net.irx $(OUT)/rawnet.irx $(OUT)/usbraw.irx $(OUT)/fileraw.irx $(OUT)/rawnet_mx.irx

$(OUT)/%.o: %.c irx_imports.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/%.o: %.S
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/imports.o: imports.lst irx_imports.h
	@mkdir -p $(OUT)
	{ echo '#include "irx_imports.h"'; cat $<; } > $(@:.o=.c)
	$(CC) $(CFLAGS) -c $(@:.o=.c) -o $@

$(OUT)/net.o: net_imports.h
$(OUT)/rawnet.o: rawnet_imports.h
$(OUT)/rawnet_mx.o: rawnet.c rawnet_mx_imports.h ../../iop/dev9/mxeth/include/mxeth.h \
                      ../../iop/dev9/mxdev9/include/mxdev9.h
$(OUT)/usbraw.o: usbraw_imports.h
$(OUT)/fileraw.o: fileraw_imports.h

$(OUT)/%_imports.o: %_imports.lst %_imports.h irx_imports.h
	@mkdir -p $(OUT)
	{ echo '#include "$*_imports.h"'; cat $<; } > $(@:.o=.c)
	$(CC) $(CFLAGS) -c $(@:.o=.c) -o $@

$(OUT)/r3000a.elf: $(OBJS)
	$(PS2MAX_IOP_BIN)/mipsel-none-elf-gcc -T$(PS2MAX_IOP_SDK)/iop/startup/linkfile -o $@ $^ -nostdlib -dc -r

$(OUT)/net.elf: $(OUT)/net.o $(OUT)/net_imports.o
	$(PS2MAX_IOP_BIN)/mipsel-none-elf-gcc -T$(PS2MAX_IOP_SDK)/iop/startup/linkfile -o $@ $^ -nostdlib -dc -r

$(OUT)/rawnet.elf: $(OUT)/rawnet.o $(OUT)/rawnet_imports.o
	$(PS2MAX_IOP_BIN)/mipsel-none-elf-gcc -T$(PS2MAX_IOP_SDK)/iop/startup/linkfile -o $@ $^ -nostdlib -dc -r

$(OUT)/rawnet_mx.elf: $(OUT)/rawnet_mx.o $(OUT)/rawnet_mx_imports.o
	$(PS2MAX_IOP_BIN)/mipsel-none-elf-gcc -T$(PS2MAX_IOP_SDK)/iop/startup/linkfile -o $@ $^ -nostdlib -dc -r

$(OUT)/usbraw.elf: $(OUT)/usbraw.o $(OUT)/usbraw_imports.o
	$(PS2MAX_IOP_BIN)/mipsel-none-elf-gcc -T$(PS2MAX_IOP_SDK)/iop/startup/linkfile -o $@ $^ -nostdlib -dc -r

$(OUT)/fileraw.elf: $(OUT)/fileraw.o $(OUT)/fileraw_imports.o
	$(PS2MAX_IOP_BIN)/mipsel-none-elf-gcc -T$(PS2MAX_IOP_SDK)/iop/startup/linkfile -o $@ $^ -nostdlib -dc -r

$(OUT)/%.irx: $(OUT)/%.elf
	$(PS2MAX_IOP_BIN)/mipsel-none-elf-strip --strip-unneeded --remove-section=.pdr \
	  --remove-section=.comment --remove-section=.mdebug.abi32 --remove-section=.gnu.attributes \
	  -o $(@:.irx=.stripped.elf) $<
	$(PS2MAX_IOP_SDK)/bin/iopfixup --irx1 -o $@ $(@:.irx=.stripped.elf)

.PHONY: all
