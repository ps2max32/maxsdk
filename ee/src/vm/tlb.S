# TLB exceptions with VM on (codes 1-3, installed by mx_vm_init). The
# kernel jumps here with only k0 and k1 changed, at EXL = 1. Save the
# registers, then let C (vm.c, on an exception stack of its own) decide:
#   VMX_RETURN: the TLB is right now; restore and ERET to the instruction
#   VMX_SLOW:   the page is in the swap file: C copied the registers onto the
#               faulting thread's stack; ERET into mx__vm_slow there (with
#               the thread's interrupt state), which resumes by the syscall
#               VM_RESUME_SYSCALL (mx__vm_resume_k)
#   VMX_FATAL:  restore and go to the crash report (exception.S)
# C at EXL = 1 touches core memory only, so it cannot fault itself.

#include "vm_int.h"

	.set noreorder
	.set noat
	.text

	.globl mx__vm_vector
	.type mx__vm_vector, @function
	.ent mx__vm_vector
mx__vm_vector:
	# The fast refill: a TLB refill (code 2 or 3) of a VM page that is
	# mapped (V in lo[]) and has no entry gets the next entry here, as
	# vm.c's tlb_take and tlb_sync would (vm_int.h, struct mx__vm_fast),
	# with $1-$3 saved; anything else goes to the full path below.
	lui $27, %hi(mx__vm_fast)
	addiu $27, $27, %lo(mx__vm_fast)
	mfc0 $26, $13
	andi $26, $26, 0x78	# ExcCode << 2: 0x08 (2) and 0x0C (3) give 0x08
	xori $26, $26, 0x08
	bnez $26, full
	nop
	sq $1, VMF_SAVE($27)
	sq $2, VMF_SAVE + 16($27)
	sq $3, VMF_SAVE + 32($27)
	mfc0 $26, $8		# BadVAddr
	srl $26, $26, 15
	addiu $26, $26, -VMF_BASE_PAGE	# k0 = the page (if VM)
	lw $1, VMF_NPAGES($27)
	sltu $1, $26, $1
	beqz $1, other
	lw $2, VMF_DIR($27)
have:	sw $26, VMF_PAGE($27)	# the page in the tables, for slot_page below
	srl $1, $26, VML_SHIFT
	sll $1, $1, 2
	addu $2, $2, $1
	lw $2, 0($2)		# $2 = its leaf (the zero leaf where no page was touched yet:
	andi $26, $26, 63	# no entry and V = 0 there, so C); k0 = the page in the leaf
	addu $1, $2, $26
	lbu $1, VML_PAGE_SLOT($1)
	bnez $1, slow		# it has an entry: C
	sll $3, $26, 2
	addu $3, $2, $3
	lw $1, VML_LO($3)	# $1 = lo[page]
	andi $3, $1, 2		# LO_V
	beqz $3, slow
	lw $3, VMF_NEXT($27)	# $3 = the entry to take
	mtc0 $3, $0		# Index
	mtc0 $1, $2		# EntryLo0
	addiu $1, $1, 0x100	# EntryLo1: the next 16 KiB
	mtc0 $1, $3
	ori $1, $0, 0x6000	# PageMask 16 KiB
	mtc0 $1, $5
	mfc0 $1, $10		# EntryHi: the refill set its VPN2 from BadVAddr and kept
	ori $1, $1, 0x6000	# the ASID (the process's): VPN2's low bits cleared for
	xori $1, $1, 0x6000	# the 32 KiB pair
	mtc0 $1, $10
	sync.p
	tlbwi
	sync.p
	addu $1, $2, $26
	sb $3, VML_PAGE_SLOT($1)	# page_slot[page] = entry
	lw $1, VMF_REFILLS($27)
	addiu $1, $1, 1
	sw $1, VMF_REFILLS($27)
	sll $26, $26, 2
	addu $26, $2, $26
	sw $1, VML_STAMP($26)	# stamp[page] = refills: used now (eviction)
	lw $1, VML_HIST($2)	# hist[page]++, when a program asked (mx_vm_refill_hist)
	beqz $1, 3f
	subu $26, $26, $2
	addu $1, $1, $26
	lw $2, 0($1)
	addiu $2, $2, 1
	sw $2, 0($1)
3:	sll $1, $3, 1
	addu $1, $1, $27
	lhu $2, VMF_SLOT_PAGE($1)	# what the entry held: page + 1, or 0
	lw $26, VMF_PAGE($27)
	addiu $26, $26, 1
	sh $26, VMF_SLOT_PAGE($1)	# slot_page[entry] = page + 1
	beqz $2, 4f
	addiu $2, $2, -1
	srl $1, $2, VML_SHIFT	# that page: no entry
	sll $1, $1, 2
	lw $26, VMF_DIR($27)
	addu $1, $1, $26
	lw $1, 0($1)
	andi $2, $2, 63
	addu $1, $1, $2
	sb $0, VML_PAGE_SLOT($1)
4:	addu $1, $27, $3	# the next entry: the VM's in turn (next_of)
	lbu $3, VMF_NEXT_OF($1)
	sw $3, VMF_NEXT($27)
	mfc0 $1, $14		# an instruction fetch: BadVAddr = EPC (+ 4 in a delay slot)
	mfc0 $2, $13
	bgez $2, 6f
	nop
	addiu $1, $1, 4
6:	mfc0 $2, $8
	bne $1, $2, 7f
	lw $1, VMF_REFILLS_I($27)
	addiu $1, $1, 1
	sw $1, VMF_REFILLS_I($27)
7:	lq $1, VMF_SAVE($27)
	lq $2, VMF_SAVE + 16($27)
	lq $3, VMF_SAVE + 32($27)
	sync.l		# the handler's stores done: the instruction returned to may be a CACHE
	sync.p
	eret
	nop
	# Not the VM window: the region's pages (mapped files, heaps of pages)
	# follow the whole VM window's in the tables (vm.c's va_page); $2 is
	# the directory already.
other:	addiu $26, $26, VMF_BASE_PAGE - VMF_REG_PAGE	# k0 = the page of the region
	lw $3, VMF_NREG($27)
	sltu $3, $26, $3
	bnez $3, have
	addiu $26, $26, VMF_VM_PAGES	# k0 = the page in the tables
slow:
	lq $1, VMF_SAVE($27)
	lq $2, VMF_SAVE + 16($27)
	lq $3, VMF_SAVE + 32($27)
full:
	lui $26, %hi(mx__vm_xs)
	addiu $26, $26, %lo(mx__vm_xs)
	sq $1, 16($26)
	sq $2, 32($26)
	sq $3, 48($26)
	sq $4, 64($26)
	sq $5, 80($26)
	sq $6, 96($26)
	sq $7, 112($26)
	sq $8, 128($26)
	sq $9, 144($26)
	sq $10, 160($26)
	sq $11, 176($26)
	sq $12, 192($26)
	sq $13, 208($26)
	sq $14, 224($26)
	sq $15, 240($26)
	sq $16, 256($26)
	sq $17, 272($26)
	sq $18, 288($26)
	sq $19, 304($26)
	sq $20, 320($26)
	sq $21, 336($26)
	sq $22, 352($26)
	sq $23, 368($26)
	sq $24, 384($26)
	sq $25, 400($26)
	sq $28, 448($26)
	sq $29, 464($26)
	sq $30, 480($26)
	sq $31, 496($26)
	pmfhi $8
	sq $8, VMX_HI($26)
	pmflo $8
	sq $8, VMX_LO($26)
	mfsa $8
	sw $8, VMX_SA($26)
	mfc0 $8, $12
	sw $8, VMX_STATUS($26)
	mfc0 $8, $13
	sw $8, VMX_CAUSE($26)
	mfc0 $8, $14
	sw $8, VMX_EPC($26)
	mfc0 $8, $8
	sw $8, VMX_BADVADDR($26)
	lui $29, %hi(mx__vm_xstack + 8192)
	addiu $29, $29, %lo(mx__vm_xstack + 8192)
	jal mx__vm_exception
	move $4, $26
	lui $26, %hi(mx__vm_xs)
	addiu $26, $26, %lo(mx__vm_xs)
	beqz $2, 1f
	addiu $2, $2, -VMX_SLOW
	beqz $2, 2f
	nop
	# VMX_FATAL
	lq $8, VMX_HI($26)
	pmthi $8
	lq $8, VMX_LO($26)
	pmtlo $8
	lw $8, VMX_SA($26)
	mtsa $8
	lq $1, 16($26)
	lq $2, 32($26)
	lq $3, 48($26)
	lq $4, 64($26)
	lq $5, 80($26)
	lq $6, 96($26)
	lq $7, 112($26)
	lq $8, 128($26)
	lq $9, 144($26)
	lq $10, 160($26)
	lq $11, 176($26)
	lq $12, 192($26)
	lq $13, 208($26)
	lq $14, 224($26)
	lq $15, 240($26)
	lq $16, 256($26)
	lq $17, 272($26)
	lq $18, 288($26)
	lq $19, 304($26)
	lq $20, 320($26)
	lq $21, 336($26)
	lq $22, 352($26)
	lq $23, 368($26)
	lq $24, 384($26)
	lq $25, 400($26)
	lq $28, 448($26)
	lq $29, 464($26)
	lq $30, 480($26)
	lq $31, 496($26)
	lui $26, %hi(mx__exception_vector)
	addiu $26, $26, %lo(mx__exception_vector)
	jr $26
	nop
2:	# VMX_SLOW: sp and a0 = the copy, EPC = mx__vm_slow
	lw $4, VMX_FRAME($26)
	move $29, $4
	lui $8, %hi(mx__vm_slow)
	addiu $8, $8, %lo(mx__vm_slow)
	mtc0 $8, $14
	sync.l		# the handler's stores done: the instruction returned to may be a CACHE
	sync.p
	eret
	nop
1:	# VMX_RETURN
	lq $8, VMX_HI($26)
	pmthi $8
	lq $8, VMX_LO($26)
	pmtlo $8
	lw $8, VMX_SA($26)
	mtsa $8
	lq $1, 16($26)
	lq $2, 32($26)
	lq $3, 48($26)
	lq $4, 64($26)
	lq $5, 80($26)
	lq $6, 96($26)
	lq $7, 112($26)
	lq $8, 128($26)
	lq $9, 144($26)
	lq $10, 160($26)
	lq $11, 176($26)
	lq $12, 192($26)
	lq $13, 208($26)
	lq $14, 224($26)
	lq $15, 240($26)
	lq $16, 256($26)
	lq $17, 272($26)
	lq $18, 288($26)
	lq $19, 304($26)
	lq $20, 320($26)
	lq $21, 336($26)
	lq $22, 352($26)
	lq $23, 368($26)
	lq $24, 384($26)
	lq $25, 400($26)
	lq $28, 448($26)
	lq $29, 464($26)
	lq $30, 480($26)
	lq $31, 496($26)
	sync.l		# the handler's stores done: the instruction returned to may be a CACHE
	sync.p
	eret
	nop
	.end mx__vm_vector
	.size mx__vm_vector, .-mx__vm_vector

# The handler of syscall -VM_RESUME_SYSCALL, a0 = the registers of a slow
# fault: the kernel calls it at EXL = 1 (below a frame of its own on the
# caller's stack, which the restored sp drops). Restore everything, Status
# (EXL still on) and EPC as they were at the fault, and ERET to the
# instruction that faulted.
	.globl mx__vm_resume_k
	.type mx__vm_resume_k, @function
	.ent mx__vm_resume_k
mx__vm_resume_k:
	move $26, $4
	lq $8, VMX_HI($26)
	pmthi $8
	lq $8, VMX_LO($26)
	pmtlo $8
	lw $8, VMX_SA($26)
	mtsa $8
	lw $8, VMX_EPC($26)
	mtc0 $8, $14
	lw $8, VMX_STATUS($26)
	mtc0 $8, $12
	sync.p
	lq $1, 16($26)
	lq $2, 32($26)
	lq $3, 48($26)
	lq $4, 64($26)
	lq $5, 80($26)
	lq $6, 96($26)
	lq $7, 112($26)
	lq $8, 128($26)
	lq $9, 144($26)
	lq $10, 160($26)
	lq $11, 176($26)
	lq $12, 192($26)
	lq $13, 208($26)
	lq $14, 224($26)
	lq $15, 240($26)
	lq $16, 256($26)
	lq $17, 272($26)
	lq $18, 288($26)
	lq $19, 304($26)
	lq $20, 320($26)
	lq $21, 336($26)
	lq $22, 352($26)
	lq $23, 368($26)
	lq $24, 384($26)
	lq $25, 400($26)
	lq $28, 448($26)
	lq $29, 464($26)
	lq $30, 480($26)
	lq $31, 496($26)
	sync.l		# the handler's stores done: the instruction returned to may be a CACHE
	sync.p
	eret
	nop
	.end mx__vm_resume_k
	.size mx__vm_resume_k, .-mx__vm_resume_k

# mx__vm_resume(frame): from mx__vm_slow, never returns.
	.globl mx__vm_resume
	.type mx__vm_resume, @function
	.ent mx__vm_resume
mx__vm_resume:
	li $3, -VM_RESUME_SYSCALL
	syscall
	nop
	.end mx__vm_resume
	.size mx__vm_resume, .-mx__vm_resume
