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
