/* Link script for IOP modules with ld.lld -r (Rules.mk), in place of
   ps2sdk's iop/startup/linkfile (GNU ld's; lld does not take it): the
   relocatable ELF iopfixup turns into an IRX. It merges the sections the way
   that script does, each input section 16-byte aligned, the first object's
   .text first (exports.o: the export table must be at the start), and keeps
   the relocations (.rel.<section>). iopfixup makes .iopmod and the symbols
   _ftext, _etext, _fdata, _gp, _end. */

ENTRY(_start)

SECTIONS {
  .text 0 : SUBALIGN(16) { *(.text .text.*) *(.stub) }
  .rodata : SUBALIGN(16) { *(.rodata .rodata.*) }
  .data : SUBALIGN(16) { *(.data .data.*) }
  .sdata : { *(.sdata .sdata.*) *(.lit8) *(.lit4) }
  .sbss : { *(.sbss .sbss.*) *(.scommon) }
  .bss : SUBALIGN(16) { *(.bss .bss.*) *(COMMON) }
  /DISCARD/ : {
    *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) *(.mdebug.*)
    *(.note.*) *(.llvm_addrsig)
  }
}
