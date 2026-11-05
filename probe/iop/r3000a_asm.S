/* The instruction sequences of r3000a.c. Everything is .set noreorder: what is
   written here is what runs. */

	.set	noreorder
	.set	noat
	.text

#define FN(n)	.globl n; .type n,@function; n:
#define RET	jr $ra; nop

/* ---- timing kernels: k(a, b, n, p), n passes of 16 repetitions of the body.
   a0, a1: operands; a2: passes; a3: a pointer. r3000a.c subtracts k_empty. */

#define KB(n)	FN(n) 1: .rept 16
#define KE	.endr; addiu $a2, $a2, -1; bnez $a2, 1b; nop; RET

FN(k_empty)
1:	addiu	$a2, $a2, -1
	bnez	$a2, 1b
	nop
	RET

KB(k_nop)
	nop
KE
KB(k_addu)
	addu	$t0, $t0, $a0
KE
KB(k_sll)
	sll	$t0, $t0, 1
KE
KB(k_lw)
	lw	$t0, 0($a3)
KE
KB(k_lb)
	lb	$t0, 0($a3)
KE
KB(k_lw_nop)
	lw	$t0, 0($a3)
	nop
KE
KB(k_lw_use)
	lw	$t0, 0($a3)
	addu	$t1, $t0, $t0
KE
KB(k_lw_nop_use)
	lw	$t0, 0($a3)
	nop
	addu	$t1, $t0, $t0
KE
KB(k_lwl_lwr)
	lwl	$t0, 3($a3)
	lwr	$t0, 0($a3)
KE
KB(k_sw)
	sw	$a0, 0($a3)
KE
KB(k_sb)
	sb	$a0, 0($a3)
KE
KB(k_sw_n1)
	sw	$a0, 0($a3)
	nop
KE
KB(k_sw_n3)
	sw	$a0, 0($a3)
	nop
	nop
	nop
KE
KB(k_sw_n7)
	sw	$a0, 0($a3)
	.rept 7
	nop
	.endr
KE
KB(k_sw_lw)
	sw	$a0, 0($a3)
	lw	$t0, 0($a3)
KE
KB(k_btaken)
	beq	$zero, $zero, 2f
	nop
2:
KE
KB(k_bnot)
	bne	$zero, $zero, 2f
	nop
2:
KE
KB(k_mfc0)
	mfc0	$t0, $12
KE
KB(k_mfhi)
	mfhi	$t0
KE
KB(k_mtlo)
	mtlo	$a0
KE
KB(k_mult)
	mult	$a0, $a1
KE
KB(k_multu)
	multu	$a0, $a1
KE
KB(k_div)
	div	$zero, $a0, $a1
KE
KB(k_divu)
	divu	$zero, $a0, $a1
KE
KB(k_mult_mflo)
	mult	$a0, $a1
	mflo	$t0
KE
KB(k_multu_mflo)
	multu	$a0, $a1
	mflo	$t0
KE
KB(k_div_mflo)
	div	$zero, $a0, $a1
	mflo	$t0
KE
KB(k_divu_mflo)
	divu	$zero, $a0, $a1
	mflo	$t0
KE
KB(k_mult_mfhi)
	mult	$a0, $a1
	mfhi	$t0
KE
KB(k_mult_n4)
	mult	$a0, $a1
	.rept 4
	nop
	.endr
	mflo	$t0
KE
KB(k_mult_n8)
	mult	$a0, $a1
	.rept 8
	nop
	.endr
	mflo	$t0
KE
KB(k_mult_n16)
	mult	$a0, $a1
	.rept 16
	nop
	.endr
	mflo	$t0
KE
KB(k_div_n16)
	div	$zero, $a0, $a1
	.rept 16
	nop
	.endr
	mflo	$t0
KE
KB(k_div_n48)
	div	$zero, $a0, $a1
	.rept 48
	nop
	.endr
	mflo	$t0
KE
KB(k_mult_mtlo)
	mult	$a0, $a1
	mtlo	$a0
KE
KB(k_mult_div)
	mult	$a0, $a1
	div	$zero, $a0, $a1
	mflo	$t0
KE

/* I-cache: a3 points into the sled, the size of the loop is what is left */
FN(k_sled)
	move	$t9, $ra
1:	jalr	$a3
	nop
	addiu	$a2, $a2, -1
	bnez	$a2, 1b
	nop
	jr	$t9
	nop

	.balign	16
	.globl	sled_start
sled_start:
	.rept	4096
	nop
	.endr
	.globl	sled_end
sled_end:
	RET

/* D-cache: a0 = stride, a1 = bytes, a2 = passes, a3 = buffer */
FN(k_walk)
1:	move	$t1, $a3
	addu	$t2, $a3, $a1
2:	lw	$t0, 0($t1)
	addu	$t1, $t1, $a0
	bne	$t1, $t2, 2b
	nop
	addiu	$a2, $a2, -1
	bnez	$a2, 1b
	nop
	RET

/* The same with a store: the cost of writing through */
FN(k_walk_sw)
1:	move	$t1, $a3
	addu	$t2, $a3, $a1
2:	sw	$zero, 0($t1)
	addu	$t1, $t1, $a0
	bne	$t1, $t2, 2b
	nop
	addiu	$a2, $a2, -1
	bnez	$a2, 1b
	nop
	RET

/* Scratchpad candidates are walked with the same k_walk; a store then a load
   at an address not read before tells if a store allocates: k_store_alloc
   stores to each line, then reads it back. */
FN(k_store_load)
1:	move	$t1, $a3
	addu	$t2, $a3, $a1
2:	sw	$zero, 0($t1)
	lw	$t0, 0($t1)
	addu	$t1, $t1, $a0
	bne	$t1, $t2, 2b
	nop
	addiu	$a2, $a2, -1
	bnez	$a2, 1b
	nop
	RET

/* ---- one kernel per instruction (stage "inst"). The body runs 16 times per pass.
   _t: independent, _c: each depends on the one before (t0 starts at 0, so add and
   sub cannot overflow: nothing catches that trap here). ---- */

#define T(n, ...)	FN(k_##n) move $t0, $zero; 1: .rept 16; __VA_ARGS__; .endr; addiu $a2, $a2, -1; bnez $a2, 1b; nop; RET
/* the same for what overwrites ra */
#define TR(n, ...)	FN(k_##n) move $t9, $ra; 1: .rept 16; __VA_ARGS__; .endr; addiu $a2, $a2, -1; bnez $a2, 1b; nop; jr $t9; move $ra, $t9

T(add_t, add $t0, $a0, $a1)
T(add_c, add $t0, $t0, $a1)
T(addu_t, addu $t0, $a0, $a1)
T(addu_c, addu $t0, $t0, $a1)
T(sub_t, sub $t0, $a0, $a1)
T(sub_c, sub $t0, $t0, $a1)
T(subu_t, subu $t0, $a0, $a1)
T(subu_c, subu $t0, $t0, $a1)
T(and_t, and $t0, $a0, $a1)
T(and_c, and $t0, $t0, $a1)
T(or_t, or $t0, $a0, $a1)
T(or_c, or $t0, $t0, $a1)
T(xor_t, xor $t0, $a0, $a1)
T(xor_c, xor $t0, $t0, $a1)
T(nor_t, nor $t0, $a0, $a1)
T(nor_c, nor $t0, $t0, $a1)
T(slt_t, slt $t0, $a0, $a1)
T(slt_c, slt $t0, $t0, $a1)
T(sltu_t, sltu $t0, $a0, $a1)
T(sltu_c, sltu $t0, $t0, $a1)
T(sllv_t, sllv $t0, $a0, $a1)
T(sllv_c, sllv $t0, $t0, $a1)
T(srlv_t, srlv $t0, $a0, $a1)
T(srlv_c, srlv $t0, $t0, $a1)
T(srav_t, srav $t0, $a0, $a1)
T(srav_c, srav $t0, $t0, $a1)
T(addi_t, addi $t0, $a0, 1)
T(addi_c, addi $t0, $t0, 1)
T(addiu_t, addiu $t0, $a0, 1)
T(addiu_c, addiu $t0, $t0, 1)
T(slti_t, slti $t0, $a0, 1)
T(slti_c, slti $t0, $t0, 1)
T(sltiu_t, sltiu $t0, $a0, 1)
T(sltiu_c, sltiu $t0, $t0, 1)
T(andi_t, andi $t0, $a0, 1)
T(andi_c, andi $t0, $t0, 1)
T(ori_t, ori $t0, $a0, 1)
T(ori_c, ori $t0, $t0, 1)
T(xori_t, xori $t0, $a0, 1)
T(xori_c, xori $t0, $t0, 1)
T(lui_t, lui $t0, 1)
T(sll_t, sll $t0, $a0, 3)
T(sll_c, sll $t0, $t0, 3)
T(srl_t, srl $t0, $a0, 3)
T(srl_c, srl $t0, $t0, 3)
T(sra_t, sra $t0, $a0, 3)
T(sra_c, sra $t0, $t0, 3)
T(sll31_t, sll $t0, $a0, 31)

T(lb_t, lb $t0, 0($a3))
T(lbu_t, lbu $t0, 0($a3))
T(lh_t, lh $t0, 0($a3))
T(lhu_t, lhu $t0, 0($a3))
T(lw_t, lw $t0, 0($a3))
T(lwl_t, lwl $t0, 0($a3))
T(lwr_t, lwr $t0, 0($a3))
T(lh_odd2, lh $t0, 2($a3))
T(lwl_o1, lwl $t0, 1($a3))
T(lwr_o1, lwr $t0, 1($a3))

T(sb_t, sb $a0, 0($a3))
T(sb_n, sb $a0, 0($a3); nop)
T(sh_t, sh $a0, 0($a3))
T(sh_n, sh $a0, 0($a3); nop)
T(sw_t, sw $a0, 0($a3))
T(sw_n, sw $a0, 0($a3); nop)
T(swl_t, swl $a0, 0($a3))
T(swl_n, swl $a0, 0($a3); nop)
T(swr_t, swr $a0, 0($a3))
T(swr_n, swr $a0, 0($a3); nop)

T(beq_y, beq $zero, $zero, 2f; nop; 2:)
T(beq_n, beq $a0, $zero, 2f; nop; 2:)
T(bne_y, bne $a0, $zero, 2f; nop; 2:)
T(bne_n, bne $zero, $zero, 2f; nop; 2:)
T(blez_y, blez $zero, 2f; nop; 2:)
T(blez_n, blez $a0, 2f; nop; 2:)
T(bgtz_y, bgtz $a0, 2f; nop; 2:)
T(bgtz_n, bgtz $zero, 2f; nop; 2:)
T(bltz_y, bltz $a1, 2f; nop; 2:)
T(bltz_n, bltz $zero, 2f; nop; 2:)
T(bgez_y, bgez $zero, 2f; nop; 2:)
T(bgez_n, bgez $a1, 2f; nop; 2:)
TR(bltzal_y, bltzal $a1, 2f; nop; 2:)
TR(bltzal_n, bltzal $zero, 2f; nop; 2:)
TR(bgezal_y, bgezal $zero, 2f; nop; 2:)
TR(bgezal_n, bgezal $a1, 2f; nop; 2:)
T(j_y, j 2f; nop; 2:)
TR(jal_y, jal 2f; nop; 2:)
/* la is two more instructions */
T(jr_la, la $t1, 2f; jr $t1; nop; 2:)
T(jalr_la, la $t1, 2f; jalr $t2, $t1; nop; 2:)
TR(call_ret, jal ret_only; nop)

FN(ret_only)	RET

T(mflo_i, mflo $t0)
T(mfhi_i, mfhi $t0)
T(mtlo_i, mtlo $a0)
T(mthi_i, mthi $a0)
T(mfc0_sr, mfc0 $t0, $12)
T(mfc0_cause, mfc0 $t0, $13)
T(mfc0_epc, mfc0 $t0, $14)
T(mfc0_prid, mfc0 $t0, $15)

/* dependencies between units: nothing should stall */
T(alu_addr, addiu $t1, $a3, 0; lw $t0, 0($t1))
T(alu_branch, addiu $t1, $zero, 0; bne $t1, $zero, 2f; nop; 2:)
T(alu_store, addiu $t1, $a0, 1; sw $t1, 0($a3))
T(lw_nop_sw, lw $t1, 0($a3); nop; sw $t1, 0($a3))
T(mflo_use, mflo $t1; addu $t0, $t1, $t1)
T(mtlo_mflo, mtlo $a0; mflo $t0)

/* SR written with its own value (interrupts are off) */
FN(k_mtc0_sr)
	mfc0	$t0, $12
	nop
1:	.rept 16
	mtc0	$t0, $12
	.endr
	addiu	$a2, $a2, -1
	bnez	$a2, 1b
	nop
	RET

/* rfe pops the mode stack: all of IE and KU cleared first, so it stays that way */
FN(k_rfe)
	mfc0	$t0, $12
	li	$t1, -64
	and	$t1, $t0, $t1
	mtc0	$t1, $12
	nop
	nop
	nop
1:	.rept 16
	rfe
	.endr
	addiu	$a2, $a2, -1
	bnez	$a2, 1b
	nop
	mtc0	$t0, $12
	nop
	nop
	RET

/* traps, under trap_entry (stage "exc"): the round trip with its 8 instructions */
T(trap_syscall, syscall)
T(trap_break, break)
T(trap_ri, .word 0x0000000f)
T(trap_cpu, .word 0x44080000)

/* SR with bit 8 (a software interrupt mask, interrupts are off) flipped, read back
   0, 1 or 2 instructions after the mtc0. Returns what was read. */
#define MTC(n, gap)	FN(n) mfc0 $t0, $12; nop; xori $t1, $t0, 0x100; mtc0 $t1, $12; gap; mfc0 $v0, $12; nop; mtc0 $t0, $12; nop; nop; RET
MTC(h_mtc0_d0, )
MTC(h_mtc0_d1, nop)
MTC(h_mtc0_d2, nop; nop)
MTC(h_mtc0_d3, nop; nop; nop)

/* mflo, mfhi: the next instruction has the value? */
FN(h_mflo_use)
	li	$t1, 3
	li	$t2, 7
	li	$t0, 5
	mult	$t1, $t2
	mflo	$t0
	move	$v0, $t0
	RET
FN(h_mfhi_use)
	li	$t1, 0x10000
	li	$t0, 5
	mult	$t1, $t1
	mfhi	$t0
	move	$v0, $t0
	RET

/* ---- instruction results: u32 f(u32 a, u32 b) ---- */

#define R3(op)	FN(f_##op) op $v0, $a0, $a1; RET
R3(add)
R3(addu)
R3(sub)
R3(subu)
R3(and)
R3(or)
R3(xor)
R3(nor)
R3(slt)
R3(sltu)
FN(f_sllv)	sllv	$v0, $a0, $a1; RET
FN(f_srlv)	srlv	$v0, $a0, $a1; RET
FN(f_srav)	srav	$v0, $a0, $a1; RET
FN(f_addi)	addi	$v0, $a0, -2; RET
FN(f_addiu)	addiu	$v0, $a0, -2; RET
FN(f_slti)	slti	$v0, $a0, -2; RET
FN(f_sltiu)	sltiu	$v0, $a0, -2; RET
FN(f_andi)	andi	$v0, $a0, 0xf0f0; RET
FN(f_ori)	ori	$v0, $a0, 0xf0f0; RET
FN(f_xori)	xori	$v0, $a0, 0xf0f0; RET
FN(f_lui)	lui	$v0, 0xabcd; RET
FN(f_sll)	sll	$v0, $a0, 4; RET
FN(f_srl)	srl	$v0, $a0, 4; RET
FN(f_sra)	sra	$v0, $a0, 4; RET

#define COMMA ,
#define MD(n, op, r)	FN(f_##n) op; r $v0; RET
MD(mult_lo, mult $a0 COMMA $a1, mflo)
MD(mult_hi, mult $a0 COMMA $a1, mfhi)
MD(multu_lo, multu $a0 COMMA $a1, mflo)
MD(multu_hi, multu $a0 COMMA $a1, mfhi)
MD(div_lo, div $zero COMMA $a0 COMMA $a1, mflo)
MD(div_hi, div $zero COMMA $a0 COMMA $a1, mfhi)
MD(divu_lo, divu $zero COMMA $a0 COMMA $a1, mflo)
MD(divu_hi, divu $zero COMMA $a0 COMMA $a1, mfhi)
FN(f_mtlo)	mtlo	$a0; mflo $v0; RET
FN(f_mthi)	mthi	$a0; mfhi $v0; RET

/* loads: a0 points to the data, a1 is the old register value */
#define LD(op, o)	FN(f_##op##o) move $v0, $a1; op $v0, o($a0); nop; RET
LD(lb, 0)
LD(lb, 1)
LD(lbu, 1)
LD(lh, 0)
LD(lh, 2)
LD(lhu, 2)
LD(lw, 0)
LD(lwl, 0)
LD(lwl, 1)
LD(lwl, 2)
LD(lwl, 3)
LD(lwr, 0)
LD(lwr, 1)
LD(lwr, 2)
LD(lwr, 3)

/* stores: a0 points to the word, a1 is stored; returns the word */
#define ST(op, o)	FN(f_##op##o) op $a1, o($a0); lw $v0, 0($a0); nop; RET
ST(sb, 1)
ST(sh, 2)
ST(sw, 0)
ST(swl, 0)
ST(swl, 1)
ST(swl, 2)
ST(swl, 3)
ST(swr, 0)
ST(swr, 1)
ST(swr, 2)
ST(swr, 3)

/* branches: 1 if taken. The delay slot always runs: it sets bit 1. */
#define BR2(op)	FN(f_##op) li $v0, 1; op $a0, $a1, 2f; ori $v0, $v0, 2; xori $v0, $v0, 1; 2: RET
#define BR1(op)	FN(f_##op) li $v0, 1; op $a0, 2f; ori $v0, $v0, 2; xori $v0, $v0, 1; 2: RET
BR2(beq)
BR2(bne)
BR1(blez)
BR1(bgtz)
BR1(bltz)
BR1(bgez)

/* the link: returns its offset from the branch, 8 */
FN(f_bltzal)
	move	$t9, $ra
	li	$t0, -1
4:	bltzal	$t0, 2f
	nop
	move	$v0, $zero
	jr	$t9
	move	$ra, $t9
2:	la	$t0, 4b
	subu	$v0, $ra, $t0
	jr	$t9
	move	$ra, $t9
FN(f_bgezal)
	move	$t9, $ra
4:	bgezal	$zero, 2f
	nop
	move	$v0, $zero
	jr	$t9
	move	$ra, $t9
2:	la	$t0, 4b
	subu	$v0, $ra, $t0
	jr	$t9
	move	$ra, $t9
FN(f_jal)
	move	$t9, $ra
4:	jal	2f
	nop
	move	$v0, $zero
	jr	$t9
	move	$ra, $t9
2:	la	$t0, 4b
	subu	$v0, $ra, $t0
	jr	$t9
	move	$ra, $t9
FN(f_jalr)
	move	$t9, $ra
	la	$t1, 2f
4:	jalr	$t2, $t1
	nop
	move	$v0, $zero
	jr	$t9
	nop
2:	la	$t0, 4b
	subu	$v0, $t2, $t0
	jr	$t9
	nop
FN(f_j)
	li	$v0, 1
	j	2f
	nop
	li	$v0, 0
2:	RET
FN(f_jr)
	li	$v0, 1
	la	$t0, 2f
	jr	$t0
	nop
	li	$v0, 0
2:	RET

/* ---- hazards: what really happens. a0 points to the data of hazards() in r3000a.c ---- */

/* the instruction after a load sees the old value? */
FN(h_load_use)
	li	$t0, 5
	lw	$t0, 0($a0)
	move	$v0, $t0
	RET
/* one instruction later the new one */
FN(h_load_nop_use)
	li	$t0, 5
	lw	$t0, 0($a0)
	nop
	move	$v0, $t0
	RET
/* a write in the delay slot against the load */
FN(h_load_write)
	li	$t0, 5
	lw	$t0, 0($a0)
	li	$t0, 6
	nop
	move	$v0, $t0
	RET
/* the same with lb and an ALU result */
FN(h_lb_write)
	li	$t0, 5
	lb	$t0, 0($a0)
	addiu	$t0, $zero, 6
	nop
	move	$v0, $t0
	RET
/* two loads to one register */
FN(h_load_load)
	li	$t0, 5
	lw	$t0, 0($a0)
	lw	$t0, 4($a0)
	nop
	move	$v0, $t0
	RET
/* a load whose address comes from the load before it: the old base */
FN(h_load_base)
	move	$t0, $a0
	lw	$t0, 8($a0)
	lw	$v0, 4($t0)
	nop
	RET
/* the instruction after the slot of two loads to one register */
FN(h_load_load_use)
	li	$t0, 5
	lw	$t0, 0($a0)
	lw	$t0, 4($a0)
	move	$v0, $t0
	RET
/* lwl after lw to one register: is the merge fed with the load in flight? */
FN(h_lw_lwl)
	li	$t0, 5
	lw	$t0, 0($a0)
	lwl	$t0, 5($a0)
	nop
	move	$v0, $t0
	RET
/* lwl, lwr without a nop */
FN(h_lwl_lwr)
	li	$t0, 5
	lwl	$t0, 26($a0)
	lwr	$t0, 23($a0)
	nop
	move	$v0, $t0
	RET
/* lwr, then a plain use */
FN(h_lwr_use)
	li	$t0, 5
	lwl	$t0, 26($a0)
	lwr	$t0, 23($a0)
	move	$v0, $t0
	RET
/* a branch reading the register of the load just before it */
FN(h_load_branch)
	li	$t0, 0
	lw	$t0, 0($a0)
	beqz	$t0, 2f
	li	$v0, 1
	li	$v0, 2
2:	RET
/* jr reading the register of the load just before it: both targets are safe */
FN(h_load_jr)
	la	$t0, 2f
	la	$t1, 3f
	sw	$t1, 16($a0)
	lw	$t0, 16($a0)
	jr	$t0
	nop
	nop
2:	li	$v0, 1
	RET
3:	li	$v0, 2
	RET
/* a load in a branch delay slot, used at the target */
FN(h_load_bdelay)
	li	$t0, 5
	beq	$zero, $zero, 2f
	lw	$t0, 0($a0)
2:	move	$v0, $t0
	RET
/* the same one instruction later */
FN(h_load_bdelay_nop)
	li	$t0, 5
	beq	$zero, $zero, 2f
	lw	$t0, 0($a0)
2:	nop
	move	$v0, $t0
	RET
/* mfc0 then use: the delay of Table 13.1 */
FN(h_mfc0_use)
	li	$t0, 5
	mfc0	$t0, $12
	move	$v0, $t0
	RET
FN(h_mfc0_nop_use)
	li	$t0, 5
	mfc0	$t0, $12
	nop
	move	$v0, $t0
	RET
/* the delay slot of jal reads ra */
FN(h_jal_ra)
	move	$t9, $ra
	move	$t0, $zero
4:	jal	2f
	move	$t0, $ra
2:	la	$t1, 4b
	subu	$v0, $t0, $t1
	jr	$t9
	move	$ra, $t9
/* mtlo while a multiply runs: does the multiply end later and overwrite it? */
FN(h_mult_mtlo)
	li	$t0, 0x55
	li	$t1, 3
	li	$t2, 7
	mult	$t1, $t2
	mtlo	$t0
	mflo	$v0
	RET
/* mthi while a multiply runs: LO afterwards */
FN(h_mult_mthi_lo)
	li	$t0, 0x55
	li	$t1, 3
	li	$t2, 7
	mult	$t1, $t2
	mthi	$t0
	mflo	$v0
	RET
/* the same, HI */
FN(h_mult_mthi_hi)
	li	$t0, 0x55
	li	$t1, 3
	li	$t2, 7
	mult	$t1, $t2
	mthi	$t0
	mfhi	$v0
	RET
/* a second multiply while one runs: only the last result? */
FN(h_mult_mult)
	li	$t1, 3
	li	$t2, 7
	li	$t3, 11
	mult	$t1, $t2
	mult	$t1, $t3
	mflo	$v0
	RET
/* a divide while a multiply runs */
FN(h_mult_div)
	li	$t1, 21
	li	$t2, 7
	mult	$t1, $t2
	div	$zero, $t1, $t2
	mflo	$v0
	RET
/* a multiply while a divide runs */
FN(h_div_mult)
	li	$t1, 21
	li	$t2, 7
	div	$zero, $t1, $t2
	mult	$t1, $t2
	mflo	$v0
	RET
/* mflo right before mult, as the manual forbids: only wrong after an exception */
FN(h_mflo_mult)
	li	$t1, 3
	li	$t2, 7
	mult	$t1, $t2
	mflo	$v0
	mult	$t2, $t2
	nop
	RET

/* mtlo, mthi and a running multiply: the other half, and an old LO to tell it apart */
FN(h_mult_mtlo_hi)
	li	$t0, 0x55
	li	$t1, 0x66
	mthi	$t1
	li	$t1, 3
	li	$t2, 7
	mult	$t1, $t2
	mtlo	$t0
	mfhi	$v0
	RET
FN(h_old_mult_mthi_lo)
	li	$t0, 0x1234
	mtlo	$t0
	li	$t1, 3
	li	$t2, 7
	mult	$t1, $t2
	mthi	$t0
	mflo	$v0
	RET
FN(h_mult_n3_mthi_lo)
	li	$t0, 0x1234
	mtlo	$t0
	li	$t1, 3
	li	$t2, 7
	mult	$t1, $t2
	nop
	nop
	nop
	mthi	$t0
	mflo	$v0
	RET
FN(h_mult_n20_mthi_lo)
	li	$t0, 0x1234
	mtlo	$t0
	li	$t1, 3
	li	$t2, 7
	mult	$t1, $t2
	.rept 20
	nop
	.endr
	mthi	$t0
	mflo	$v0
	RET
FN(h_div_mthi_lo)
	li	$t0, 0x1234
	mtlo	$t0
	li	$t1, 21
	li	$t2, 7
	div	$zero, $t1, $t2
	mthi	$t0
	mflo	$v0
	RET
FN(h_div_mtlo_hi)
	li	$t0, 0x1234
	mthi	$t0
	li	$t1, 22
	li	$t2, 7
	div	$zero, $t1, $t2
	mtlo	$t0
	mfhi	$v0
	RET

/* mflo at distance 0, 1 or 2 from the next mult, with interrupts on: u32 s(n)
   counts the passes in which mflo did not get 3 * 7. An interrupt that aborts the
   mflo after the next mult has started gives 11 * 13 instead (manual, ch. 13). */
#define STRESS(name, gap)		\
FN(name);				\
	move	$v0, $zero;		\
	li	$t1, 3;			\
	li	$t2, 7;			\
	li	$t3, 11;		\
	li	$t4, 13;		\
	li	$t5, 21;		\
1:	mult	$t1, $t2;		\
	.rept 10;			\
	nop;				\
	.endr;				\
	mflo	$t0;			\
	gap;				\
	mult	$t3, $t4;		\
	beq	$t0, $t5, 2f;		\
	addiu	$a0, $a0, -1;		\
	addiu	$v0, $v0, 1;		\
2:	.rept 10;			\
	nop;				\
	.endr;				\
	bnez	$a0, 1b;		\
	nop;				\
	RET
STRESS(s_gap0, )
STRESS(s_gap1, nop)
STRESS(s_gap2, nop; nop)

/* ---- coprocessor 0 ---- */
FN(c0_prid)	mfc0	$v0, $15; nop; RET
FN(c0_sr)	mfc0	$v0, $12; nop; RET

/* ---- exceptions: r3000a.c puts a jump to trap_entry at the vector. EPC + 4 is
   where it returns to: none of the instructions below is in a delay slot. ---- */
	.globl	trap_entry
trap_entry:
	mfc0	$k0, $13
	lui	$k1, %hi(trap_cause)
	sw	$k0, %lo(trap_cause)($k1)
	mfc0	$k0, $14
	lui	$k1, %hi(trap_epc)
	sw	$k0, %lo(trap_epc)($k1)
	addiu	$k0, $k0, 4
	jr	$k0
	rfe

/* a0 points to a scratch buffer, 32 bytes before and after it are free */
#define X(n, ...)	FN(x_##n) __VA_ARGS__; RET
#define XW(n, w)	FN(x_##n) .word w; nop; RET
X(none, nop)
/* a load from a0: u32 x_peek(u32 addr) */
/* the nops: a bus error comes after the load, and trap_entry skips one instruction */
FN(x_peek)	lw $v0, 0($a0); .rept 8; nop; .endr; RET
X(add_ov, li $t0, 0x7fffffff; add $t0, $t0, $t0)
X(addi_ov, li $t0, 0x7fffffff; addi $t0, $t0, 1)
X(sub_ov, li $t0, 0x80000000; li $t1, 1; sub $t0, $t0, $t1)
X(lw_unal, lw $t0, 1($a0))
X(lh_unal, lh $t0, 1($a0))
X(sw_unal, sw $t0, 1($a0))
X(sh_unal, sh $t0, 1($a0))
X(syscall, syscall)
X(break, break)
X(scratch, li $t0, 0x1f800000; lw $t0, 0($t0); nop)
X(tlbp, tlbp)
X(tlbwi, tlbwi)
X(tlbwr, tlbwr)
X(tlbr, tlbr)
/* cache 0, 0($a0) and pref 0, 0($a0) are the opcodes 0x2f and 0x33 */
XW(cache, 0xbc800000)
XW(pref, 0xcc800000)
/* swc3 $0, 0($a0) */
XW(swc3, 0xec800000)
/* FPU */
XW(mfc1, 0x44080000)
XW(mtc1, 0x44880000)
XW(lwc1, 0xc4800000)
XW(swc1, 0xe4800000)
XW(add_s, 0x46000000)
XW(bc1f, 0x45000000)
FN(x_bc1t)	.word 0x45010001; nop; RET
XW(cfc1, 0x44480000)
XW(ctc1, 0x44c80000)
/* COP2 (the PS1's GTE) */
XW(mfc2, 0x48080000)
XW(mtc2, 0x48880000)
XW(cfc2, 0x48480000)
XW(ctc2, 0x48c80000)
XW(lwc2, 0xc8800000)
XW(swc2, 0xe8800000)
XW(cop2, 0x4a000000)
FN(x_bc2f)	.word 0x49000001; nop; RET
FN(x_bc2t)	.word 0x49010001; nop; RET
/* bc0f, bc0t with a branch to the next instruction but one: the same path */
FN(x_bc0f)	.word 0x41000001; nop; RET
FN(x_bc0t)	.word 0x41010001; nop; RET
/* MIPS II and later */
XW(ll, 0xc0880000)
XW(sc, 0xe0880000)
XW(sync, 0x0000000f)
XW(teq, 0x00000034)
FN(x_beql)	.word 0x50000001; nop; RET
XW(ldc1, 0xd4800000)
XW(movz, 0x0000400a)
XW(mul, 0x70004002)
XW(clz, 0x70004020)
/* MIPS III: 64 bits */
XW(daddu, 0x0000402d)
XW(ld, 0xdc880000)
XW(sd, 0xfc880000)
/* opcodes no MIPS has: reserved */
XW(res_op, 0x78000000)
XW(res_fn, 0x00000001)
