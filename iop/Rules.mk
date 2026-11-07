# Building an IOP module, as ps2sdk's iop/Rules.make: each module's Makefile sets
#
#   MODULE   the name of the .irx
#   SRCS     its sources, under src/
#   CFLAGS_MODULE, OBJS_MODULE   anything more
#
# and includes this file. Its exports (src/exports.tab, if there is one) are linked
# first, and imports come from src/imports.lst. All of it is LLVM's: the feature
# clang (-mcpu=r3000a; the import and export tables through irx_clang.h), ld.lld
# -r with irx.ld, llvm-objcopy; then ps2sdk's iopfixup makes the IRX.
#
#   make [OUT=<dir>]    <dir>/<module>.irx

MAXSDK ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
PS2MAX ?= $(abspath $(MAXSDK)/../..)
OUT    ?= $(PS2MAX)/build/libs/maxsdk/iop
PS2MAX_PS2DEV   ?= $(abspath $(PS2MAX)/../ps2dev/install)
PS2MAX_IOP_SDK  ?= $(PS2MAX_PS2DEV)/ps2sdk
PS2MAX_LLVM_BIN ?= $(PS2MAX)/build/toolchain/llvm/feature/bin

OBJ    = $(OUT)/obj/$(MODULE)
CC     = $(PS2MAX_LLVM_BIN)/clang --target=mipsel-none-elf -mcpu=r3000a
LD     = $(PS2MAX_LLVM_BIN)/ld.lld -m elf32ltsmip -r -T $(MAXSDK)/iop/irx.ld
OBJCOPY = $(PS2MAX_LLVM_BIN)/llvm-objcopy
CFLAGS = -std=gnu23 -D_IOP -fno-builtin -ffreestanding -G0 -Os -Wall -Wno-unused-but-set-global \
         -msoft-float -mno-abicalls -fno-pic -fno-unwind-tables -fno-asynchronous-unwind-tables \
         $(CFLAGS_MODULE) $(EXTRA_CFLAGS) \
         -Iinclude -Isrc -I$(MAXSDK)/common/include \
         -I$(PS2MAX_IOP_SDK)/iop/include -I$(PS2MAX_IOP_SDK)/common/include
TABLE_CFLAGS = -include $(MAXSDK)/iop/irx_clang.h
HDRS   = $(wildcard include/*.h include/*/*.h src/*.h) $(wildcard $(MAXSDK)/common/include/*.h)

EXPORTS = $(if $(wildcard src/exports.tab),$(OBJ)/exports.o)
OBJS    = $(EXPORTS) $(SRCS:%.c=$(OBJ)/%.o) $(OBJ)/imports.o $(OBJS_MODULE)

all: $(OUT)/$(MODULE).irx

$(OBJ)/%.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/exports.o: src/exports.tab
	@mkdir -p $(dir $@)
	{ echo '#include "irx.h"'; cat $<; } > $(@:.o=.c)
	$(CC) $(CFLAGS) $(TABLE_CFLAGS) -c $(@:.o=.c) -o $@

$(OBJ)/imports.o: src/imports.lst src/irx_imports.h
	@mkdir -p $(dir $@)
	{ echo '#include "irx_imports.h"'; cat $<; } > $(@:.o=.c)
	$(CC) $(CFLAGS) $(TABLE_CFLAGS) -c $(@:.o=.c) -o $@

$(OBJ)/$(MODULE).elf: $(OBJS)
	$(LD) -o $@ $^

$(OUT)/$(MODULE).irx: $(OBJ)/$(MODULE).elf
	$(OBJCOPY) --strip-unneeded $< $(@:.irx=.stripped.elf)
	$(PS2MAX_IOP_SDK)/bin/iopfixup --irx1 -o $@ $(@:.irx=.stripped.elf)
	@rm -f $(@:.irx=.stripped.elf)

.PHONY: all
