/* Linker script for maxsdk programs linked into VM (MX_MEMORY=vm in
   mk/maxsdk.mk, docs/vm.md). Started by mxlink only.

   Core (from 0x100000, physical, loaded by mxlink): crt0, the libraries'
   code that runs with interrupts off or before the VM is on (listed below,
   checked by tools/pinned-check.py at each link), the libraries' rodata,
   data and bss (archive members), libmxc's glue, the VU
   and HLSL objects (in directories vu/ and hlsl/), and what the program marks
   MX_CORE_FN / MX_CORE_DATA (<mx/cpu.h>); VU microcode, constructors,
   packets and MX_DMA_LINES buffers wherever they come from.
   VM (from MX_VM_BASE, 0x0800_0000): the rest of the libraries' code (mx OS
   plan 4b) and of the program's own objects,
   text and rodata in one segment, data and bss in another, each starting
   on a page (32 KiB). The program's start-up (ee/src/vm/image.c) turns VM on
   and reads them from the ELF (argv[0]); the VM heap starts after them.
   jal reaches between the two (one 256 MiB region). */

OUTPUT_ARCH(mips)
ENTRY(__start)

MEMORY {
  ram : ORIGIN = 0x00100000, LENGTH = 0x02000000 - 0x00100000
  vm  : ORIGIN = 0x08000000, LENGTH = 128M
  spr : ORIGIN = 0x70000000, LENGTH = 16K
}

PHDRS {
  text PT_LOAD;
  note PT_NOTE;
  vmtext PT_LOAD;
  vmdata PT_LOAD;
}

SECTIONS {
  .text ORIGIN(ram) : {
    __text_start = .;
    KEEP(*(.text.start))
    *crt0.o(.text .text.*)
    /* phase 4b: of the libraries only the start-up (before the VM is on)
       and what runs with interrupts off; the rest is VM (MX_PIN_OBJS) */
    *libmxk.a:start.c.o(.text .text.*)
    *libmxk.a:kstubs.c.o(.text .text.*)
    *libmxk.a:ksys.c.o(.text .text.*)
    *libmxk.a:runtime.c.o(.text .text.*)
    *libmxk.a:core.c.o(.text .text.*)
    *libmxk.a:cache.c.o(.text .text.*)
    *libmxk.a:irq.c.o(.text .text.*)
    *libmxk.a:timer.c.o(.text .text.*)
    *libmxk.a:sif.c.o(.text .text.*)
    *libmxk.a:thread.c.o(.text .text.*)
    /* called with interrupts off (tools/pinned-check.py) */
    *libmxk.a:dma.c.o(.text .text.*)
    *libmxk.a:pkt.c.o(.text .text.*)
    *libmxk.a:vu.c.o(.text .text.*)
    *libmxk.a:gs.c.o(.text .text.*)
    *libmxk.a:display.c.o(.text .text.*)
    *libmxk.a:hires.c.o(.text .text.*)
    *libmxk.a:dbgscreen.c.o(.text .text.*)
    *libmxk.a:debug.c.o(.text .text.*)
    *libmxk.a:frame.c.o(.text .text.*)
    *libmxk.a:frame3.c.o(.text .text.*)
    *libmxk.a:prof.c.o(.text .text.*)
    *libmxk.a:sample.c.o(.text .text.*)
    *libmxk.a:printf.c.o(.text .text.*)
    *libmxk.a:string.c.o(.text .text.*)
    *libc.a:libc_machine_mips_mem*.o(.text .text.*)
    *libc.a:libc_string_*.o(.text .text.*)
    *libc.a:libc_machine_mips_str*.o(.text .text.*)
    *.core.a:*(.text .text.*)
    *glue.c.o(.text .text.*)
    */vu/*.o(.text .text.*)
    */hlsl/*.o(.text .text.*)
    *(.text.mx.core .text.mx.core.*)
    __text_end = .;
  } >ram :text

  /* libmxk's note for the kernel (src/base/kstubs.c): in the text segment
     and a PT_NOTE of its own */
  .note.mxk : ALIGN(4) { KEEP(*(.note.mxk .note.mxk.*)) } >ram :text :note

  .vutext : ALIGN(16) { *(.vutext .vutext.*) } >ram :text

  .rodata : ALIGN(128) {
    *crt0.o(.rodata .rodata.* .rdata)
    /* the start-up's and the interrupts' data: used before the VM is on, or
       with interrupts off (mx__core_off is written by the kernel at the
       load) */
    *libmxk.a:start.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:kstubs.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:ksys.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:runtime.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:core.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:cache.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:irq.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:timer.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:sif.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:thread.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:gs.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:dma.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:trace.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:video.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:display.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:dbgscreen.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:hires.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:vu.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:pkt.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:debug.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:frame.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:frame3.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:prof.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:sample.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:printf.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:string.c.o(.rodata .rodata.* .rdata)
    *libmxk.a:net.c.o(.rodata .rodata.* .rdata)
    *.core.a:*(.rodata .rodata.* .rdata)
    *glue.c.o(.rodata .rodata.* .rdata)
    */vu/*.o(.rodata .rodata.* .rdata)
    */hlsl/*.o(.rodata .rodata.* .rdata)
    *(.rodata.mx.core .rodata.mx.core.*)
  } >ram

  .init_array : ALIGN(16) {
    __init_array_start = .;
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  } >ram

  .data : ALIGN(128) {
    *crt0.o(.data .data.*)
    *libmxk.a:start.c.o(.data .data.*)
    *libmxk.a:kstubs.c.o(.data .data.*)
    *libmxk.a:ksys.c.o(.data .data.*)
    *libmxk.a:runtime.c.o(.data .data.*)
    *libmxk.a:core.c.o(.data .data.*)
    *libmxk.a:cache.c.o(.data .data.*)
    *libmxk.a:irq.c.o(.data .data.*)
    *libmxk.a:timer.c.o(.data .data.*)
    *libmxk.a:sif.c.o(.data .data.*)
    *libmxk.a:thread.c.o(.data .data.*)
    *libmxk.a:gs.c.o(.data .data.*)
    *libmxk.a:dma.c.o(.data .data.*)
    *libmxk.a:trace.c.o(.data .data.*)
    *libmxk.a:video.c.o(.data .data.*)
    *libmxk.a:display.c.o(.data .data.*)
    *libmxk.a:dbgscreen.c.o(.data .data.*)
    *libmxk.a:hires.c.o(.data .data.*)
    *libmxk.a:vu.c.o(.data .data.*)
    *libmxk.a:pkt.c.o(.data .data.*)
    *libmxk.a:debug.c.o(.data .data.*)
    *libmxk.a:frame.c.o(.data .data.*)
    *libmxk.a:frame3.c.o(.data .data.*)
    *libmxk.a:prof.c.o(.data .data.*)
    *libmxk.a:sample.c.o(.data .data.*)
    *libmxk.a:printf.c.o(.data .data.*)
    *libmxk.a:string.c.o(.data .data.*)
    *libmxk.a:net.c.o(.data .data.*)
    *.core.a:*(.data .data.*)
    *glue.c.o(.data .data.*)
    */vu/*.o(.data .data.*)
    */hlsl/*.o(.data .data.*)
    *(.data.mx.core .data.mx.core.*)
  } >ram

  _gp = ALIGN(128) + 0x7ff0;
  .sdata : ALIGN(128) { *(.lit4) *(.lit8) *(.sdata .sdata.*) } >ram

  .mx.packets : ALIGN(64) { *(.mx.packets .mx.packets.*) . = ALIGN(64); } >ram

  .bss (NOLOAD) : ALIGN(128) {
    __bss_start = .;
    *(.bss.mx.lines)
    . = ALIGN(64);
    *(.sbss .sbss.*) *(.scommon)
    *(.bss.mx.core .bss.mx.core.*)
    *crt0.o(.bss .bss.* COMMON)
    *libmxk.a:start.c.o(.bss .bss.* COMMON)
    *libmxk.a:kstubs.c.o(.bss .bss.* COMMON)
    *libmxk.a:ksys.c.o(.bss .bss.* COMMON)
    *libmxk.a:runtime.c.o(.bss .bss.* COMMON)
    *libmxk.a:core.c.o(.bss .bss.* COMMON)
    *libmxk.a:cache.c.o(.bss .bss.* COMMON)
    *libmxk.a:irq.c.o(.bss .bss.* COMMON)
    *libmxk.a:timer.c.o(.bss .bss.* COMMON)
    *libmxk.a:sif.c.o(.bss .bss.* COMMON)
    *libmxk.a:thread.c.o(.bss .bss.* COMMON)
    *libmxk.a:gs.c.o(.bss .bss.* COMMON)
    *libmxk.a:dma.c.o(.bss .bss.* COMMON)
    *libmxk.a:trace.c.o(.bss .bss.* COMMON)
    *libmxk.a:video.c.o(.bss .bss.* COMMON)
    *libmxk.a:display.c.o(.bss .bss.* COMMON)
    *libmxk.a:dbgscreen.c.o(.bss .bss.* COMMON)
    *libmxk.a:hires.c.o(.bss .bss.* COMMON)
    *libmxk.a:vu.c.o(.bss .bss.* COMMON)
    *libmxk.a:pkt.c.o(.bss .bss.* COMMON)
    *libmxk.a:debug.c.o(.bss .bss.* COMMON)
    *libmxk.a:frame.c.o(.bss .bss.* COMMON)
    *libmxk.a:frame3.c.o(.bss .bss.* COMMON)
    *libmxk.a:prof.c.o(.bss .bss.* COMMON)
    *libmxk.a:sample.c.o(.bss .bss.* COMMON)
    *libmxk.a:printf.c.o(.bss .bss.* COMMON)
    *libmxk.a:string.c.o(.bss .bss.* COMMON)
    *libmxk.a:net.c.o(.bss .bss.* COMMON)
    *.core.a:*(.bss .bss.* COMMON)
    *glue.c.o(.bss .bss.* COMMON)
    */vu/*.o(.bss .bss.* COMMON)
    */hlsl/*.o(.bss .bss.* COMMON)
    . = ALIGN(16);
    __bss_end = .;
  } >ram
  /* The main stack (crt0's SetupThread): in the core after the program, not
     at the top of memory, so that it moves with the core when the kernel puts
     that at a physical run of its own (docs/mx-os-plan.md, step 2b). 128 KiB,
     or --defsym=__mx_stack_size=<bytes> (a --defsym of __stack_size comes
     after this has used it: crt0 would give SetupThread that size over 128
     KiB, the stack would run into the heap) */
  .stack (NOLOAD) : ALIGN(64) {
    __mx_main_stack = .;
    . += DEFINED(__mx_stack_size) ? __mx_stack_size : 128 * 1024;
  } >ram
  _end = .;

  /* VM: the program's own objects */
  .vm.text ORIGIN(vm) : {
    __mx_vm_start = .;
    *(.text .text.*)
  } >vm :vmtext
  .vm.rodata : ALIGN(128) { *(.rodata .rodata.*) *(.rdata) } >vm :vmtext
  .vm.data : ALIGN(32768) { *(.data .data.*) } >vm :vmdata
  .vm.bss (NOLOAD) : ALIGN(128) { *(.bss .bss.*) *(COMMON) } >vm :vmdata
  __mx_vm_end = ALIGN(32768);

  .spr (NOLOAD) : { *(.spr .spr.*) } >spr

  /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.gnu.attributes) *(.comment) *(.pdr) }

  PROVIDE(__stack = __mx_main_stack);
  PROVIDE(__stack_size = DEFINED(__mx_stack_size) ? __mx_stack_size : 128 * 1024);
}

ASSERT(_end <= 0x01F00000, "the core takes all memory")
