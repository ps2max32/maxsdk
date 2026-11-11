# Entry point. The loader (mxkernel, ps2link, the BIOS, PCSX2 -elf) jumps here with
# a0 = its argument block (ps2link passes { pid; argc; argv[16]; payload }),
# or garbage when there is none.

	.set noreorder
	.set noat
	.section .text.start, "ax", @progbits
	.globl __start
	.type __start, @function
	.ent __start
__start:
	move $16, $4                # keep the loader argument across the BSS clear

	# Clear BSS (16-byte aligned by the linker script).
	la $2, __bss_start
	la $3, __bss_end
1:	sltu $1, $2, $3
	beq $1, $0, 2f
	nop
	sq $0, 0($2)
	b 1b
	addiu $2, $2, 16
2:
	la $2, mx__loader_args
	sw $16, 0($2)

	# The main stack, as the BIOS's SetupThread gives it (its top less the
	# 0x2A0 bytes SetupThread uses above the stack pointer): set here, not by
	# the BIOS (mx OS plan 2g). mx__main calls SetupThread itself for a
	# program on the BIOS's threads (its arguments, the BIOS's bookkeeping).
	la $28, _gp
	la $2, __stack
	la $3, __stack_size
	addu $2, $2, $3
	addiu $29, $2, -0x2a0

	j mx__main
	nop
	.end __start
	.size __start, .-__start
