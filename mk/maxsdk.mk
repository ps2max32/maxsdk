# mk/maxsdk.mk: what a program built on maxsdk includes (clang, the LLVM
# R5900 toolchain; gcc is not supported). maxsdk's own Makefile uses it too.
#
# Set before including:
#   OUT          the program's output directory
#   VSM_DIRS     directories of its VU microcode (*.vsm)
#   HLSL_DIRS    directories of its HLSL programs (*.hlsl)
#   HLSL_IX      programs also built indexed, HLSL_VU0 programs for VU0,
#                IX_N (or IX_N_<name>) vertices per indexed batch,
#                VU_OPT_FLAGS for opt (e.g. -vu-contexts=1)
# Optional: PS2MAX, LLVM, MAXSDK_OUT (where libmx.a and crt0 are built),
# MX_MEMORY (phys or vm).
#
# It gives CC, CFLAGS, $(call link,out,objs), MX_LIB, MX_CRT0, MX_LD, MX_HDRS, MX_LOADER,
# VU_OBJS, HLSL_OBJS, HLSL_HDRS and their rules, and a rule that builds libmx
# by maxsdk's Makefile when a program needs it. Link a program as
#   $(call link,$@,$(MX_CRT0) <objects> $(VU_OBJS) $(HLSL_OBJS) $(MX_LIB))

MAXSDK     := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
# the includer's first target stays the default goal
MX_GOAL    := $(.DEFAULT_GOAL)
PS2MAX     ?= $(abspath $(MAXSDK)/../..)
# the feature LLVM (ps2max/build-llvm.sh); envsetup.sh exports the same paths
PS2MAX_LLVM_SRC ?= $(PS2MAX)/toolchain/llvm/llvm-project
PS2MAX_LLVM_BIN ?= $(PS2MAX)/build/toolchain/llvm/feature/bin
LLVM       ?= $(PS2MAX_LLVM_BIN)
MAXSDK_OUT ?= $(PS2MAX)/build/libs/maxsdk

# -ffinite-math-only: the R5900 FPU has no INF/NaN, so clang only uses it for
# finite math; without the flag it calls soft-float helpers (not in libmx).
# -fno-math-errno: the math functions do not set errno, so sqrtf is sqrt.s.
COMMON_CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -ffreestanding \
                -fno-common -ffinite-math-only -fno-math-errno -I$(MAXSDK)/ee/include -G0
CC       = $(LLVM)/clang
CFLAGS   = $(COMMON_CFLAGS) --target=mips64el -mcpu=r5900 -mabi=n32 -fno-pic -mno-abicalls \
           -fomit-frame-pointer -I$(OUT)/hlsl
CFLAGS  += $(EXTRA_CFLAGS)
# libmxk with MX_MEMORY=vm or MX_KERNEL=1 (all in core): under mx-kernel
# (docs/kernel.md)
# BSD sockets (ee/compat/net): for a program's own CFLAGS
MX_NET_CFLAGS = -I$(MAXSDK)/ee/compat/net
MX_LIB   = $(MAXSDK_OUT)/libmx$(if $(filter vm,$(MX_MEMORY))$(MX_KERNEL),k).a
MX_CRT0  = $(MAXSDK_OUT)/obj/crt0.o
# MX_MEMORY=vm: the program's own objects linked into VM (docs/vm.md),
# started by mxlink (tools/mxlink, the PS2's loader; on PCSX2 MX_LOADER); phys (the default):
# all in core
MX_MEMORY ?= phys
MX_LD_phys = $(MAXSDK)/ee/ld/maxsdk.ld
# MX_VM_TEXT=core (with MX_MEMORY=vm): the program's code stays core
MX_VM_TEXT ?= vm
MX_LD_vm   = $(MAXSDK)/ee/ld/maxsdk-vm$(if $(filter core,$(MX_VM_TEXT)),-coretext).ld
MX_LD    = $(MX_LD_$(MX_MEMORY))
# PCSX2: mx-kernel without network, for programs on libmxk. It starts what
# host:mxload.txt says.
MX_LOADER = $(abspath $(MAXSDK)/../../build/tools/mxlink-emu/mxlink.elf)
MX_HDRS := $(wildcard $(MAXSDK)/ee/include/mx/*.h $(MAXSDK)/ee/include/mx/hw/*.h)
# $(call link,out,objs)
# With MX_MEMORY=vm, the objects of objs also in MX_CORE_OBJS (a library
# linked as objects, e.g. libgl's) stay core: they are linked from an
# archive of their own ($(out:.elf=.core.a)), and ee/ld/maxsdk-vm.ld keeps
# archive members core.
link_objs = $(filter-out $(MX_CORE_OBJS),$(2)) --whole-archive $(1:.elf=.core.a) --no-whole-archive
link     = $(if $(filter vm,$(MX_MEMORY)),$(if $(MX_CORE_OBJS),rm -f $(1:.elf=.core.a) && \
             $(LLVM)/llvm-ar rcs $(1:.elf=.core.a) $(filter $(MX_CORE_OBJS),$(2)) && )) \
           $(LLVM)/ld.lld -m elf32ltsmipn32 -T$(MX_LD) -z max-page-size=128 \
           --gc-sections -Map $(1:.elf=.map) -o $(1) \
           $(if $(and $(filter vm,$(MX_MEMORY)),$(MX_CORE_OBJS)),$(call link_objs,$(1),$(2)),$(2))

ifneq ($(MAXSDK_OUT),$(OUT))
# one make of maxsdk for both, not two at once under -j: a grouped target
# (&:, GNU make 4.3), or before 4.3 the crt0 made with the library
ifneq ($(filter grouped-target,$(.FEATURES)),)
$(MX_LIB) $(MX_CRT0) &: $(wildcard $(MAXSDK)/ee/src/*/*) $(MX_HDRS)
	$(MAKE) -C $(MAXSDK) OUT=$(MAXSDK_OUT) lib
else
$(MX_LIB): $(wildcard $(MAXSDK)/ee/src/*/*) $(MX_HDRS)
	$(MAKE) -C $(MAXSDK) OUT=$(MAXSDK_OUT) lib
$(MX_CRT0): $(MX_LIB)
endif
endif

$(MX_LOADER):
	$(MAKE) -C $(MAXSDK)/../../tools/mxlink emu

# ---- VU microcode -------------------------------------------------------------
# *.vsm, assembled by llvm-mc -triple=vu: each becomes an EE object with the
# code between the symbols <name>_vu and <name>_vu_end. Names are unique over
# VSM_DIRS and HLSL_DIRS.

VSMS    := $(wildcard $(addsuffix /*.vsm,$(VSM_DIRS)))
VU_OBJS := $(patsubst %.vsm,$(OUT)/vu/%.o,$(notdir $(VSMS)))
vpath %.vsm $(VSM_DIRS)

$(OUT)/vu/%.vu.elf: %.vsm
	@mkdir -p $(dir $@)
	$(LLVM)/llvm-mc -triple=vu -mcpu=vu1 -filetype=obj $(VSM_FLAGS) $< -o $@

$(OUT)/vu/%.bin: $(OUT)/vu/%.vu.elf
	$(LLVM)/llvm-objcopy -O binary --only-section=.vutext $< $@

$(OUT)/vu/%.o: $(OUT)/vu/%.bin
	printf '%s\n' '.section .vutext, "a"' '.balign 16' '.globl $*_vu' '$*_vu:' \
	  '.incbin "$<"' '.globl $*_vu_end' '$*_vu_end:' > $(@:.o=.S)
	$(CC) $(CFLAGS) -c $(@:.o=.S) -o $@

# ---- HLSL programs --------------------------------------------------------------
# ee/include/vu.hlsli: clang's HLSL for SPIR-V, then opt and llc for the VU
# (VUHLSLAdapter, VUEntryLowering); the layout header <name>.h from section
# .vu.header, wrapped like the microcode above. HLSL_VU0 lists the programs
# for VU0; the others run on VU1. (-Wno-deprecated-builtins: clang's own HLSL
# headers use them for max and min.)
#
# HLSL_IX: vertex programs that are also built indexed (docs/hlsl.md), from
# the same <name>.hlsl as program <name>_ix: IX_N vertices per batch (or
# IX_N_<name>), the header's macros VU_IX_<NAME>_..., MX_VU_SHADER(IX_<NAME>,
# <name>_ix_vu, <name>_ix_vu_end).

VU_OPT_FLAGS ?=
IX_N         ?= 32
HLSLS        := $(wildcard $(addsuffix /*.hlsl,$(HLSL_DIRS)))
HLSL_NAMES    = $(basename $(notdir $(HLSLS))) $(addsuffix _ix,$(HLSL_IX))
HLSL_OBJS     = $(patsubst %,$(OUT)/hlsl/%.o,$(HLSL_NAMES))
HLSL_HDRS     = $(patsubst %,$(OUT)/hlsl/%.h,$(HLSL_NAMES))
vpath %.hlsl $(HLSL_DIRS)
VU_CPU        = $(if $(filter $*,$(HLSL_VU0)),vu0,vu1)

$(OUT)/hlsl/%.spv.ll: %.hlsl $(MAXSDK)/ee/include/vu.hlsli $(LLVM)/clang
	@mkdir -p $(dir $@)
	$(LLVM)/clang -cc1 -triple spirv-unknown-vulkan1.3-library -x hlsl -emit-llvm -O2 \
	  -finclude-default-header -I$(MAXSDK)/ee/include -Wno-deprecated-builtins $< -o $@

$(OUT)/hlsl/%.vu.ll: $(OUT)/hlsl/%.spv.ll $(LLVM)/opt
	$(LLVM)/opt -mtriple=vu -O2 -vu-cpu=$(VU_CPU) $(VU_OPT_FLAGS) $< -S -o $@

# the indexed program from the same SPIR-V (the shorter stem wins over the
# rule above)
$(OUT)/hlsl/%_ix.vu.ll: $(OUT)/hlsl/%.spv.ll $(LLVM)/opt
	$(LLVM)/opt -mtriple=vu -O2 -vu-cpu=vu1 -vu-indexed=$(or $(IX_N_$*),$(IX_N)) \
	  -vu-header-prefix=VU_IX_ $(VU_OPT_FLAGS) $< -S -o $@

$(OUT)/hlsl/%.vu.elf: $(OUT)/hlsl/%.vu.ll $(LLVM)/llc
	$(LLVM)/llc -mtriple=vu -mcpu=$(VU_CPU) -O3 -filetype=obj $< -o $@

$(OUT)/hlsl/%.h: $(OUT)/hlsl/%.vu.elf
	$(LLVM)/llvm-objcopy --dump-section .vu.header=$@.tmp $< /dev/null
	{ echo '/* generated from $*.hlsl */'; echo '#ifndef $(shell echo $* | tr a-z A-Z)_H'; \
	  echo '#define $(shell echo $* | tr a-z A-Z)_H'; cat $@.tmp; echo '#endif'; } > $@
	rm -f $@.tmp

$(OUT)/hlsl/%.bin: $(OUT)/hlsl/%.vu.elf
	$(LLVM)/llvm-objcopy -O binary --only-section=.vutext $< $@

$(OUT)/hlsl/%.o: $(OUT)/hlsl/%.bin
	printf '%s\n' '.section .vutext, "a"' '.balign 16' '.globl $*_vu' '$*_vu:' \
	  '.incbin "$<"' '.globl $*_vu_end' '$*_vu_end:' > $(@:.o=.S)
	$(CC) $(CFLAGS) -c $(@:.o=.S) -o $@

# `make run` of the apps: the command that runs an ELF in PCSX2 headlessly and
# stops it at the end marker (@@PS2TEST_END@@): PCSX2_RUN <elf> <timeout in seconds>
PCSX2_RUN ?= pcsx2-run

.SECONDARY:
.DEFAULT_GOAL := $(MX_GOAL)
