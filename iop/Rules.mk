# Building an IOP module, as ps2sdk's iop/Rules.make: each module's Makefile sets
#
#   MODULE   the name of the .irx
#   SRCS     its sources, under src/
#   CFLAGS_MODULE, OBJS_MODULE   anything more
#
# and includes this file. Its exports (src/exports.tab, if there is one) are linked
# first, and imports come from src/imports.lst. All of it is LLVM's: the feature
# clang for the IOP target, mipsel-none-irx (R3000A, soft float, no abicalls or
# gp-relative data; the import and export tables through irx_clang.h), which
# links with ld.lld --irx: lld writes the IRX itself (.iopmod, the relocations
# for the IOP's loader), in its built-in layout of ps2sdk's linkfile.
#
#   make [OUT=<dir>]    <dir>/<module>.irx

MAXSDK ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
PS2MAX ?= $(abspath $(MAXSDK)/../..)
OUT    ?= $(PS2MAX)/build/libs/maxsdk/iop
PS2MAX_PS2DEV   ?= $(abspath $(PS2MAX)/../ps2dev/install)
PS2MAX_IOP_SDK  ?= $(PS2MAX_PS2DEV)/ps2sdk
PS2MAX_LLVM_BIN ?= $(PS2MAX)/build/toolchain/llvm/feature/bin

OBJ    = $(OUT)/obj/$(MODULE)
CC     = $(PS2MAX_LLVM_BIN)/clang --target=mipsel-none-irx
CFLAGS = -std=gnu23 -fno-builtin -ffreestanding -Os -Wall -Wno-unused-but-set-global \
         $(CFLAGS_MODULE) $(EXTRA_CFLAGS) \
         -Iinclude -Isrc -I$(MAXSDK)/common/include \
         -I$(PS2MAX_IOP_SDK)/iop/include -I$(PS2MAX_IOP_SDK)/common/include
TABLE_CFLAGS = -include $(MAXSDK)/iop/irx_clang.h
HDRS   = $(wildcard include/*.h include/*/*.h src/*.h) $(wildcard $(MAXSDK)/common/include/*.h)
# a change of the build (flags, the tables' header) builds everything again
BUILD_DEPS = $(MAXSDK)/iop/Rules.mk $(MAXSDK)/iop/irx_clang.h

EXPORTS = $(if $(wildcard src/exports.tab),$(OBJ)/exports.o)
OBJS    = $(EXPORTS) $(SRCS:%.c=$(OBJ)/%.o) $(OBJ)/imports.o $(OBJS_MODULE)

all: $(OUT)/$(MODULE).irx

$(OBJ)/%.o: src/%.c $(HDRS) $(BUILD_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/exports.o: src/exports.tab $(BUILD_DEPS)
	@mkdir -p $(dir $@)
	{ echo '#include "irx.h"'; cat $<; } > $(@:.o=.c)
	$(CC) $(CFLAGS) $(TABLE_CFLAGS) -c $(@:.o=.c) -o $@

$(OBJ)/imports.o: src/imports.lst src/irx_imports.h $(BUILD_DEPS)
	@mkdir -p $(dir $@)
	{ echo '#include "irx_imports.h"'; cat $<; } > $(@:.o=.c)
	$(CC) $(CFLAGS) $(TABLE_CFLAGS) -c $(@:.o=.c) -o $@

# $(OBJ)/$(MODULE).irx keeps the symbols, for looking up addresses
$(OUT)/$(MODULE).irx: $(OBJS)
	$(CC) -o $(OBJ)/$(MODULE).irx $^
	$(CC) -s -o $@ $^

.PHONY: all
