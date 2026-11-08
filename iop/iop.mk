# The IOP toolchain of maxsdk, for any Makefile that builds IOP modules (IRX):
# Rules.mk (one module per directory), the probes, mxlink. All of it is LLVM's:
# clang for the IOP target mipsel-none-irx (R3000A, soft float, no abicalls
# or gp-relative data), which links with ld.lld --irx: lld writes the IRX
# itself (.iopmod, the relocations for the IOP's loader) in ps2sdk's layout,
# and the import and export tables.
#
# Imports: a module calls the functions of other modules as declared in their
# headers; ld.lld makes the import table for each library from its ILB file
# (Sony's import library format): maxsdk's modules' src/exports.ilb, then
# those of ps2sdk's IOP headers (made by mkilb.py into IOP_ILB_DIR). The first
# ILB that lists a name provides it.
# Exports: --irx-export=<module's exports.ilb> writes _exp_<library>.
#
#   $(IOP_CC) $(IOP_CFLAGS) -c x.c -o x.o
#   $(IOP_CC) -o m.irx x.o $(call iop_link_libs,<exports.ilb or nothing>)
#   (compiler-rt's builtins come with the link: 64-bit division, soft float)
#   (the ILB list is a prerequisite: $(IOP_ILB_RSP))

# (its rule does not become the including Makefile's default goal)
iop_mk_goal := $(.DEFAULT_GOAL)

MAXSDK ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
PS2MAX ?= $(abspath $(MAXSDK)/../..)
PS2MAX_PS2DEV   ?= $(abspath $(PS2MAX)/../ps2dev/install)
PS2MAX_IOP_SDK  ?= $(PS2MAX_PS2DEV)/ps2sdk
PS2MAX_LLVM_BIN ?= $(PS2MAX)/build/toolchain/llvm/feature/bin

IOP_CC     = $(PS2MAX_LLVM_BIN)/clang --target=mipsel-none-irx
# Small modules: a section per function and variable, and the link keeps only
# what is used (IOP_GC=0: all of it); ld.lld --irx also turns the module's
# calls and jumps into PC-relative branches, which need no relocation.
# IOP_LTO=1: link-time optimization across a module's sources, a little
# smaller (mxnet -9%), but with it the network modules do not run yet.
IOP_GC    ?= 1
IOP_LTO   ?= 0
IOP_CFLAGS = -std=gnu23 -fno-builtin -ffreestanding -Wall -Wno-unused-but-set-global \
             $(if $(filter 1,$(IOP_GC)),-ffunction-sections -fdata-sections)
IOP_LTO_FLAGS = $(if $(filter 1,$(IOP_LTO)),-flto)
IOP_CFLAGS   += $(IOP_LTO_FLAGS)
IOP_INCS   = -I$(MAXSDK)/common/include -I$(PS2MAX_IOP_SDK)/iop/include \
             -I$(PS2MAX_IOP_SDK)/common/include

IOP_ILB_DIR  ?= $(PS2MAX)/build/libs/maxsdk/iop/ilb
IOP_ILB_RSP   = $(IOP_ILB_DIR)/ps2sdk.rsp
MAXSDK_ILBS   = $(sort $(wildcard $(MAXSDK)/iop/*/*/src/exports.ilb))
iop_comma    := ,
iop_link_libs = $(IOP_LTO_FLAGS) $(if $(filter 1,$(IOP_GC)),-Wl$(iop_comma)--gc-sections) $(IOP_LDFLAGS) $(if $(1),-Wl$(iop_comma)--irx-export=$(abspath $(1))) $(MAXSDK_ILBS) @$(IOP_ILB_RSP)

$(IOP_ILB_RSP): $(MAXSDK)/iop/mkilb.py
	python3 $< $(IOP_ILB_DIR) $(PS2MAX_IOP_SDK)/iop/include $(PS2MAX_IOP_SDK)/common/include

.DEFAULT_GOAL := $(iop_mk_goal)
