/* The program's core at a physical run of its own (docs/mx-os-plan.md, step
   2b): physical = virtual + mx__core_off for 0x10_0000 - 0x1FF_FFFF
   (<mx/cpu.h>). In a program, written by the kernel after the load (the note
   of src/base/kstubs.c names it); in the kernel, the running program's. 0:
   the BIOS's mappings, virtual = physical. In .data: the start-up clears
   .bss after the kernel wrote it. */
#include <mx/cpu.h>

u32 mx__core_off __attribute__((section(".data"))) = 0;
