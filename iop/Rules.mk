# Building an IOP module, as ps2sdk's iop/Rules.make: each module's Makefile sets
#
#   MODULE   the name of the .irx
#   SRCS     its sources, under src/
#   CFLAGS_MODULE, OBJS_MODULE   anything more
#
# and includes this file. Its exports (src/exports.tab, if there is one) are linked
# first, and imports come from src/imports.lst. The IOP gcc is ps2dev's: there is
# no LLVM linker for ps2sdk's link script yet.
#
#   make [OUT=<dir>]    <dir>/<module>.irx

MAXSDK ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
PS2MAX ?= $(abspath $(MAXSDK)/../..)
OUT    ?= $(PS2MAX)/build/libs/maxsdk/iop
PS2MAX_PS2DEV  ?= $(abspath $(PS2MAX)/../ps2dev/install)
PS2MAX_IOP_BIN ?= $(PS2MAX_PS2DEV)/iop/bin
PS2MAX_IOP_SDK ?= $(PS2MAX_PS2DEV)/ps2sdk

OBJ    = $(OUT)/obj/$(MODULE)
CC     = $(PS2MAX_IOP_BIN)/mipsel-none-elf-gcc
STRIP  = $(PS2MAX_IOP_BIN)/mipsel-none-elf-strip
CFLAGS = -D_IOP -fno-builtin -G0 -Os -Wall -msoft-float -mno-explicit-relocs $(CFLAGS_MODULE) $(EXTRA_CFLAGS) \
         -Iinclude -Isrc -I$(MAXSDK)/common/include \
         -I$(PS2MAX_IOP_SDK)/iop/include -I$(PS2MAX_IOP_SDK)/common/include
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
	$(CC) $(CFLAGS) -fno-toplevel-reorder -c $(@:.o=.c) -o $@

$(OBJ)/imports.o: src/imports.lst src/irx_imports.h
	@mkdir -p $(dir $@)
	{ echo '#include "irx_imports.h"'; cat $<; } > $(@:.o=.c)
	$(CC) $(CFLAGS) -fno-toplevel-reorder -c $(@:.o=.c) -o $@

$(OBJ)/$(MODULE).elf: $(OBJS)
	$(CC) -T$(PS2MAX_IOP_SDK)/iop/startup/linkfile -o $@ $^ -nostdlib -dc -r

$(OUT)/$(MODULE).irx: $(OBJ)/$(MODULE).elf
	$(STRIP) --strip-unneeded --remove-section=.pdr --remove-section=.comment \
	  --remove-section=.mdebug.abi32 --remove-section=.gnu.attributes -o $(@:.irx=.stripped.elf) $<
	$(PS2MAX_IOP_SDK)/bin/iopfixup --irx1 -o $@ $(@:.irx=.stripped.elf)
	@rm -f $(@:.irx=.stripped.elf)

.PHONY: all
