# Building an IOP module, as ps2sdk's iop/Rules.make: each module's Makefile sets
#
#   MODULE   the name of the .irx
#   SRCS     its sources, under src/
#   CFLAGS_MODULE, OBJS_MODULE   anything more
#
# and includes this file. Its exports are src/exports.ilb, if it has any (an ILB
# file: its export table, and what other modules import from it); its imports
# are what it calls of other modules' (iop.mk, the toolchain).
#
#   make [OUT=<dir>]    <dir>/<module>.irx

include $(dir $(lastword $(MAKEFILE_LIST)))iop.mk

OUT    ?= $(PS2MAX)/build/libs/maxsdk/iop
OBJ     = $(OUT)/obj/$(MODULE)
CC      = $(IOP_CC)
CFLAGS  = $(IOP_CFLAGS) -Os $(CFLAGS_MODULE) $(EXTRA_CFLAGS) -Iinclude -Isrc $(IOP_INCS)
HDRS    = $(wildcard include/*.h include/*/*.h src/*.h) $(wildcard $(MAXSDK)/common/include/*.h)
# a change of the build builds everything again
BUILD_DEPS = $(MAXSDK)/iop/Rules.mk $(MAXSDK)/iop/iop.mk

EXPORTS = $(wildcard src/exports.ilb)
OBJS    = $(SRCS:%.c=$(OBJ)/%.o) $(OBJS_MODULE)

all: $(OUT)/$(MODULE).irx

$(OBJ)/%.o: src/%.c $(HDRS) $(BUILD_DEPS)
	@mkdir -p $(dir $@)
	$(IOP_CC) $(CFLAGS) -c $< -o $@

# $(OBJ)/$(MODULE).irx keeps the symbols, for looking up addresses
$(OUT)/$(MODULE).irx: $(OBJS) $(EXPORTS) $(MAXSDK_ILBS) $(IOP_ILB_RSP)
	$(IOP_CC) -o $(OBJ)/$(MODULE).irx $(OBJS) $(call iop_link_libs,$(EXPORTS))
	$(IOP_CC) -s -o $@ $(OBJS) $(call iop_link_libs,$(EXPORTS))

.PHONY: all
