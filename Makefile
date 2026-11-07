# maxsdk: build the library, tests, probes and examples with clang (the LLVM
# R5900 toolchain in this tree; gcc is not supported). Programs on maxsdk
# include mk/maxsdk.mk (the flags, the link and the VU and HLSL rules).
#
#   make [OUT=<dir>]                        library, test, probe and example ELFs
#   make lib                                the libraries and crt0 only
#   make test-host                          host tests (generated fields, packets)
#   make check-deps                         fail if anything links ps2sdk
#   make gen                                regenerate the hardware headers
#
# Output goes to build/libs/maxsdk/ (in ps2max) by default.

HERE       := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PS2MAX     ?= $(abspath $(HERE)/../..)
OUT        ?= $(PS2MAX)/build/libs/maxsdk
MAXSDK_OUT := $(OUT)
HOSTCC     ?= cc

LIB_SRCS := $(filter-out %/kstubs.c %/novm.c,$(wildcard $(HERE)/ee/src/*/*.c)) $(wildcard $(HERE)/ee/src/*/*.S)
LIB_OBJS := $(patsubst $(HERE)/%,$(OUT)/obj/%.o,$(LIB_SRCS))
LIB       = $(MX_LIB)
# libmxk: the same but for what reaches the kernel (docs/kernel.md)
LIBK      = $(OUT)/libmxk.a
LIBKERN   = $(OUT)/libmxkern.a
CRT0      = $(MX_CRT0)
HDRS      = $(MX_HDRS)
# the library's own VU microcode (ee/src/*/*.vsm) goes into libmx.a
LIB_VSMS := $(wildcard $(HERE)/ee/src/*/*.vsm)
LIB_OBJS += $(patsubst %.vsm,$(OUT)/vu/%.o,$(notdir $(LIB_VSMS)))
vpath %.vsm $(sort $(dir $(LIB_VSMS)))
# IOP modules of ps2sdk that libmx loads (the sound driver, ee/src/base/snd.c),
# as data between the symbols <name>_irx and <name>_irx_end. They are the
# prebuilt modules of ps2dev: ps2max has no IOP toolchain of its own.
PS2MAX_IRX ?= $(PS2MAX)/../ps2dev/install/ps2sdk/iop/irx
LIB_IRX  := libsd audsrv
LIB_OBJS += $(patsubst %,$(OUT)/irx/%.o,$(LIB_IRX))
$(OUT)/irx/%.o: $(PS2MAX_IRX)/%.irx
	@mkdir -p $(dir $@)
	printf '%s\n' '.section .rodata.$*_irx, "a"' '.balign 16' '.globl $*_irx' '$*_irx:' \
	  '.incbin "$<"' '.globl $*_irx_end' '$*_irx_end:' > $(@:.o=.S)
	$(CC) $(CFLAGS) -c $(@:.o=.S) -o $@
# the VU0 renderer's feeder: its asm statements keep VF registers live
$(OUT)/obj/ee/src/vu/vu0r.c.o: CFLAGS += -mvu0 $(foreach r,$(shell seq 1 31),-ffixed-vf$(r))

# Each example has a directory in examples/; the tests' and probes' VU and
# HLSL programs and the examples' share one namespace (VSM_DIRS, HLSL_DIRS).
# tests/ps2 has its own copies of the programs of apps that it tests (the
# model's m*.hlsl, procgen's lvl*, tf*, fbm, ...), probe/ of vu0skin.vsm.
EXAMPLE_DIRS := $(patsubst %/,%,$(wildcard $(HERE)/examples/*/))
VSM_DIRS     := $(HERE)/tests/ps2 $(HERE)/probe $(EXAMPLE_DIRS)
HLSL_DIRS    := $(HERE)/tests/ps2 $(EXAMPLE_DIRS)
HLSL_IX      := lvl0 lvl1 lvl2 mflat mlit mtex mscei mskin
HLSL_VU0     := fbm floor nest
include $(HERE)/mk/maxsdk.mk

TESTS    := $(patsubst $(HERE)/tests/ps2/%.c,$(OUT)/tests/%.elf,$(wildcard $(HERE)/tests/ps2/*.c))
PROBES   := $(patsubst $(HERE)/probe/%.c,$(OUT)/probe/%.elf,$(wildcard $(HERE)/probe/*.c))
EXAMPLES := $(patsubst %.c,$(OUT)/examples/%.elf,$(notdir $(wildcard $(HERE)/examples/*/*.c)))
vpath %.c $(EXAMPLE_DIRS)

all: $(OUT)/libmx.a $(LIBK) $(LIBKERN) $(CRT0) $(TESTS) $(PROBES) $(EXAMPLES) $(OUT)/batch/ps2batch.elf
lib: $(OUT)/libmx.a $(LIBK) $(LIBKERN) $(CRT0)

$(OUT)/obj/%.c.o: $(HERE)/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/obj/%.S.o: $(HERE)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# The mem*/str* bodies must not be turned into calls to themselves.
$(OUT)/obj/ee/src/base/string.c.o: CFLAGS += -fno-builtin

$(CRT0): $(HERE)/ee/crt/crt0.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Three libraries (docs/kernel.md). libmxkern: all of it, the VM too: for the
# kernel, and the tests of the VM's inside. libmx: without the VM. libmxk: for
# programs under the kernel, in libk below.
VM_OBJS = $(patsubst %,$(OUT)/obj/ee/src/vm/%.o,vm.c image.c tlb.S)
$(OUT)/libmx.a: $(filter-out $(VM_OBJS),$(LIB_OBJS)) $(OUT)/obj/ee/src/vm/novm.c.o
	rm -f $@
	$(LLVM)/llvm-ar rcs $@ $^

$(LIBKERN): $(LIB_OBJS)
	rm -f $@
	$(LLVM)/llvm-ar rcs $@ $^

$(OUT)/objk/%.c.o: $(HERE)/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DMX_KERNEL=1 -c $< -o $@

LIBK_SRCS = ee/src/base/start.c ee/src/base/sif.c ee/src/base/kstubs.c
# what is the kernel's: no code in libmxk
LIBK_OUT  = ee/src/base/start.c ee/src/base/sif.c ee/src/base/exception.c ee/src/base/exception.S \
            ee/src/vm/vm.c ee/src/vm/mem.c ee/src/vm/heap.c ee/src/vm/image.c ee/src/vm/tlb.S
LIBK_OBJS = $(filter-out $(LIBK_OUT:%=$(OUT)/obj/%.o),$(LIB_OBJS)) $(LIBK_SRCS:%=$(OUT)/objk/%.o)

$(LIBK): $(LIBK_OBJS)
	rm -f $@
	$(LLVM)/llvm-ar rcs $@ $^

# tests linked into VM (ee/ld/maxsdk-vm.ld), started by mxlink (or mxlink's EMU build)
VM_TESTS = vm11_vmlink vm12_elfpage vm13_guards vm16_lock vm21_crash vm22_net vm23_pad vm24_home vm25_ids
$(VM_TESTS:%=$(OUT)/tests/%.elf): MX_MEMORY = vm
$(VM_TESTS:%=$(OUT)/tests/%.elf): $(LIBK)

# tests of the VM's inside: the VM in the program, on a bare PS2 or PCSX2
VM_INSIDE = $(filter-out $(VM_TESTS:%=$(OUT)/tests/%.elf),$(patsubst $(HERE)/tests/ps2/%.c,$(OUT)/tests/%.elf,\
              $(shell grep -lE 'mx_vm_|mx__vm_|mx_mem_|mx_heap_' $(HERE)/tests/ps2/*.c))) \
            $(patsubst $(HERE)/probe/%.c,$(OUT)/probe/%.elf,$(shell grep -lE 'mx_vm_|mx__vm_' $(HERE)/probe/*.c))
$(VM_INSIDE): LIB = $(LIBKERN)
$(VM_INSIDE): $(LIBKERN)

# Each test, probe or example is one C file, linked with crt0 and libmx only
# (libmx has the compiler helper functions; no libgcc).
$(OUT)/tests/%.elf: $(HERE)/tests/ps2/%.c $(LIB) $(CRT0) $(VU_OBJS) $(HLSL_OBJS) $(HLSL_HDRS) $(HDRS) $(MX_LD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(VU_OBJS) $(HLSL_OBJS) $(LIB))

$(OUT)/probe/%.elf: $(HERE)/probe/%.c $(LIB) $(CRT0) $(VU_OBJS) $(HLSL_OBJS) $(HLSL_HDRS) $(HDRS) $(MX_LD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PROBE_CFLAGS) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(VU_OBJS) $(HLSL_OBJS) $(LIB))

# VU0 macro mode (COP2 instructions in inline asm)
$(OUT)/probe/p10_vucopy.elf: PROBE_CFLAGS = -mvu0
$(OUT)/probe/p11_vu0mix.elf: PROBE_CFLAGS = -mvu0
$(OUT)/probe/p15_vu0skin.elf: PROBE_CFLAGS = -mvu0 -fapprox-func
$(OUT)/probe/p16_vu0reg.elf: PROBE_CFLAGS = -mvu0 -fapprox-func
# all VF registers reserved: the VU0 programs it calls write most of them
$(OUT)/probe/p17_vu0c.elf: PROBE_CFLAGS = -mvu0 $(foreach r,$(shell seq 1 31),-ffixed-vf$(r))
# p33: VU0 registers by asm, none for the compiler
$(OUT)/probe/p33_asid.elf: PROBE_CFLAGS = -mvu0 $(foreach r,$(shell seq 1 31),-ffixed-vf$(r))

# An example's C file finds the headers of its directory.
$(OUT)/examples/%.elf: %.c $(LIB) $(CRT0) $(VU_OBJS) $(HLSL_OBJS) $(HLSL_HDRS) $(HDRS) $(MX_LD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(dir $<) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(VU_OBJS) $(HLSL_OBJS) $(LIB))
# ---- C library and C++ runtime (phase 8, D8c) -------------------------------
# picolibc (built by `make picolibc` into PICOLIBC) with maxsdk's glue
# (libc/: stdio on the debug output, sbrk, _exit, a minimal C++ runtime, and
# the ps2sdk calls bench uses). Programs that use libc link
#   crt0, objects, libmxc.a, libc.a, libmx.a, compiler-rt builtins
# (soft double and the float helpers that code without -ffinite-math-only
# calls). Skipped while picolibc is not built.

PICOLIBC     ?= $(PS2MAX)/build/libs/picolibc/install
PICOLIBC_SRC ?= $(PS2MAX)/libs/picolibc
# compiler-rt's generic builtins, built like libmx (non-PIC; the toolchain's
# own libclang_rt.builtins.a is PIC and its GOT relocations do not link here)
RT_DIR       ?= $(PS2MAX_LLVM_SRC)/compiler-rt/lib/builtins
RT_SKIP       = divxc3 emutls enable_execute_stack eprintf extendhfxf2 fixunsxfdi fixunsxfsi \
                fixunsxfti fixxfdi fixxfti floatdixf floattixf floatundixf floatuntixf mulxc3 \
                powixf2 truncdfbf2 truncsfbf2 truncxfhf2 clear_cache trampoline_setup \
                os_version_check gcc_personality_v0 atomic
RT_SRCS      := $(filter-out $(patsubst %,$(RT_DIR)/%.c,$(RT_SKIP)),$(wildcard $(RT_DIR)/*.c))
RT_BUILTINS   = $(OUT)/librt.a
RT_CFLAGS     = -std=gnu11 -O2 -ffreestanding -fno-builtin -w -G0 --target=mips64el -mcpu=r5900 \
                -mabi=n32 -fno-pic -mno-abicalls
# hosted, not freestanding: libc is there, and C++ only treats main as the
# program's entry (unmangled) in a hosted build
LIBC_CFLAGS   = $(filter-out -ffreestanding,$(CFLAGS)) -isystem $(PICOLIBC)/include \
                -I$(HERE)/ee/compat/ps2sdk
CXXFLAGS      = $(filter-out -std=gnu11,$(LIBC_CFLAGS)) -std=gnu++17 -fno-exceptions -fno-rtti
CXX           = $(LLVM)/clang++
LIBMXC        = $(OUT)/libmxc.a
# the glue is linked as an object, so its sbrk and stdio win over picolibc's
# whatever the archive order
LIBC_LIBS     = $(OUT)/libc/glue.c.o $(LIBMXC) $(PICOLIBC)/lib/libc.a $(LIB) $(RT_BUILTINS)
HAVE_LIBC    := $(wildcard $(PICOLIBC)/lib/libc.a)

$(OUT)/libc/net.c.o $(OUT)/libc/tests/sockets.elf: LIBC_CFLAGS += -I$(HERE)/ee/compat/net
$(OUT)/libc/%.c.o: $(HERE)/ee/libc/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_CFLAGS) -c $< -o $@

$(OUT)/libc/%.cc.o: $(HERE)/ee/libc/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# on the compiler too: these sources rarely change, so objects of an old
# compiler would stay, and they are in every program
$(OUT)/rt/%.o: $(RT_DIR)/%.c $(LLVM)/clang
	@mkdir -p $(dir $@)
	$(CC) $(RT_CFLAGS) -c $< -o $@

$(RT_BUILTINS): $(patsubst $(RT_DIR)/%.c,$(OUT)/rt/%.o,$(RT_SRCS))
	rm -f $@
	$(LLVM)/llvm-ar rcs $@ $^

$(LIBMXC): $(OUT)/libc/glue.c.o $(OUT)/libc/compat.c.o $(OUT)/libc/atomic.c.o $(OUT)/libc/malloc.c.o \
           $(OUT)/libc/cxx.cc.o $(OUT)/libc/net.c.o
	rm -f $@
	$(LLVM)/llvm-ar rcs $@ $^

LIBC_TESTS := $(patsubst $(HERE)/tests/libc/%,$(OUT)/libc/tests/%.elf,\
                $(basename $(wildcard $(HERE)/tests/libc/*.c $(HERE)/tests/libc/*.cc)))

$(OUT)/libc/tests/%.elf: $(HERE)/tests/libc/%.c $(LIBMXC) $(LIB) $(CRT0) $(RT_BUILTINS) $(MX_LD)
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_CFLAGS) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(LIBC_LIBS))

$(OUT)/libc/tests/%.elf: $(HERE)/tests/libc/%.cc $(LIBMXC) $(LIB) $(CRT0) $(RT_BUILTINS) $(MX_LD)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(LIBC_LIBS))

ifneq ($(HAVE_LIBC),)
all: $(LIBMXC) $(RT_BUILTINS) $(LIBC_TESTS)
endif
libc: $(LIBMXC) $(RT_BUILTINS) $(LIBC_TESTS)

# Build picolibc from PICOLIBC_SRC (a picolibc 1.8.9 checkout with
# ee/libc/picolibc-r5900.patch applied) for the R5900, n32, with clang.
picolibc:
	@mkdir -p $(PICOLIBC)/../build-meson
	sed "s|@LLVM@|$(LLVM)|g" $(HERE)/ee/libc/cross-r5900.txt.in > $(PICOLIBC)/../cross.txt
	meson setup --wipe $(PICOLIBC)/../build-meson $(PICOLIBC_SRC) --cross-file $(PICOLIBC)/../cross.txt \
	  --prefix=$(PICOLIBC) -Dmultilib=false -Dpicocrt=false -Dsemihost=false -Dtests=false \
	  -Dthread-local-storage=false -Dposix-io=false -Dspecsdir=none -Dio-long-long=true \
	  -Dbuildtype=release
	ninja -C $(PICOLIBC)/../build-meson install

# ---- the combined PS2 batch ---------------------------------------------------
# One ELF that runs the tests, probes and examples below in a row, so a single
# PS2 run covers them. Each program's main() is renamed to <name>_main.

BATCH_SRCS := tests/ps2/dma.c tests/ps2/gs.c tests/ps2/vif.c tests/ps2/vu.c \
              tests/ps2/frame.c probe/p2_dma.c probe/p3_gs.c probe/p4_vu.c probe/p5_fast.c \
              examples/spin/spin.c examples/scene/scene.c examples/allunits/allunits.c tests/ps2/thread.c \
              tests/ps2/dbit.c
BATCH_OBJS := $(patsubst %.c,$(OUT)/batch/%.o,$(BATCH_SRCS))

$(OUT)/batch/%.o: $(HERE)/%.c $(HDRS) $(HLSL_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DMX_BATCH -Dmain=$(notdir $*)_main -I$(HERE)/tests/ps2 -c $< -o $@

$(OUT)/batch/ps2batch.elf: $(HERE)/tests/batch/ps2batch.c $(BATCH_OBJS) $(LIB) $(CRT0) $(VU_OBJS) \
                           $(HLSL_OBJS) $(MX_LD)
	$(CC) $(CFLAGS) -DMX_BATCH -c $< -o $(@:.elf=.o)
	$(call link,$@,$(CRT0) $(@:.elf=.o) $(BATCH_OBJS) $(VU_OBJS) $(HLSL_OBJS) $(LIB))

batch: $(OUT)/batch/ps2batch.elf

# ---- host tests -----------------------------------------------------------

# Pure parts of the library are tested on the host too.
HOST_SRCS := $(wildcard $(HERE)/tests/host/*.c) $(HERE)/ee/src/pkt/pkt.c $(HERE)/ee/src/pkt/chain.c \
             $(HERE)/ee/src/gs/shader.c $(HERE)/ee/src/gs/rt.c $(HERE)/ee/src/gs/display.c \
             $(HERE)/ee/src/gs/video.c $(HERE)/ee/src/gs/tex.c $(HERE)/ee/src/gs/vram.c \
             $(HERE)/ee/src/gs/vcache.c $(HERE)/ee/libc/malloc.c
HOST_BIN   = $(OUT)/host/test_host

$(HOST_BIN): $(HOST_SRCS) $(wildcard $(HERE)/tests/host/*.h) $(HDRS)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O1 -Wall -Wextra -DMX_HOST -DMXC_MALLOC_TEST -I$(HERE)/ee/include -I$(HERE)/tests/host \
	  $(HOST_SRCS) -o $@

test-host: $(HOST_BIN)
	$(HOST_BIN)

# ---- checks -----------------------------------------------------------------

# Nothing from ps2sdk: no ps2sdk path in any link map, no include of a ps2sdk
# header (the compiler is never given -I$PS2SDK), and no known ps2sdk symbol.
check-deps: all
	@! grep -l -i "ps2sdk" $(OUT)/tests/*.map $(OUT)/probe/*.map $(OUT)/examples/*.map 2>/dev/null
	@! $(LLVM)/llvm-nm $(TESTS) $(PROBES) $(EXAMPLES) 2>/dev/null | \
	  grep -E " (SifInitRpc|sceSifInitRpc|_ps2sdk_|InitMainThread|_InitSys|_libcglue)"
	@echo "check-deps: no ps2sdk"

# DMA addresses only from <mx/cpu.h> (mx_paddr_of, mx_paddr_range): no
# hand-made physical address (an address masked to 28/29 bits, an mx_paddr
# built from a number) anywhere else. exception.c masks kernel segments to
# read memory, not for the DMAC.
PHYS_SRCS = $(filter-out $(HERE)/ee/include/mx/cpu.h $(HERE)/ee/src/base/exception.c,\
              $(wildcard $(HERE)/ee/src/*/*.[chS] $(HERE)/ee/include/mx/*.h $(HERE)/tests/ps2/*.c \
                         $(HERE)/probe/*.c $(HERE)/examples/*/*.c $(HERE)/tutorials/*/*.c))
check-phys:
	@! grep -nE "0x0[fF]{7}u?\b|0x1[fF]{7}u?\b|\bmx_phys\(|\(mx_paddr\)\{[^0}]" $(PHYS_SRCS)
	@echo "check-phys: DMA addresses only from mx_paddr_of / mx_paddr_range"

gen:
	python3 -I -B $(HERE)/ee/gen/gen.py

clean:
	rm -rf $(OUT)

.PHONY: all lib batch libc picolibc test-host check-deps check-phys gen clean
