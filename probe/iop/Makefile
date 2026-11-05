# IOP probes: clang for the code, the IOP gcc for what has no replacement yet (the
# linker with ps2sdk's link script, iopfixup), as tools/mxlink does.
#
#   make [OUT=<dir>]    <dir>/r3000a.irx
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
          -I$(PS2MAX_IOP_SDK)/iop/include -I$(PS2MAX_IOP_SDK)/common/include -I.
OBJS    = $(OUT)/r3000a.o $(OUT)/r3000a_asm.o $(OUT)/imports.o

all: $(OUT)/r3000a.irx

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

$(OUT)/r3000a.elf: $(OBJS)
	$(PS2MAX_IOP_BIN)/mipsel-none-elf-gcc -T$(PS2MAX_IOP_SDK)/iop/startup/linkfile -o $@ $^ -nostdlib -dc -r

$(OUT)/r3000a.irx: $(OUT)/r3000a.elf
	$(PS2MAX_IOP_BIN)/mipsel-none-elf-strip --strip-unneeded --remove-section=.pdr \
	  --remove-section=.comment --remove-section=.mdebug.abi32 --remove-section=.gnu.attributes \
	  -o $(@:.irx=.stripped.elf) $<
	$(PS2MAX_IOP_SDK)/bin/iopfixup --irx1 -o $@ $(@:.irx=.stripped.elf)

.PHONY: all
