# IOP core (R3000A)

Every instruction, latency and hazard of the IOP CPU: what the manual says, what LLVM does
(`-mcpu=r3000a`), and what the hardware does. Update this file when any of the three changes.

- **Manual:** IDT R30xx Family Software Reference Manual, `docs/md/MIPS specs/R3000/R3000.md`
  (ch. 13 and Table 13.1 for timing). It describes IDT's parts, not Sony's core, so a value that
  was not measured is only a guess for the IOP.
- **LLVM:** branch r5900-feature at 64d968aad29f. All `r3000a-*` and `mips1-*` lit tests pass.
- **Hardware:** `probe/iop/r3000a.c`, `r3000a_asm.S`, 2026-11-05.
- **Cycles** are of the 36.864 MHz clock, read from RTC3. `nop` measures 1.00 per instruction, so
  timer ticks and CPU cycles are the same.

Status words in the tables: **measured** on hardware; **manual** taken from the manual only, never
run; **open** not known.

## Only a real R3000A counts

**A test result counts only from a PS2 with a real R3000A: SCPH-70xxx or older.** SCPH-75xxx and
newer have the PPC-IOP ("Deckard"): a PowerPC runs software that emulates the R3000A, with
approximate timing, and PRId is a hard-coded value of that software. Results from such a console
say something about the emulator. Never use them here, and do not use PCSX2 either.

**The console used here has a real R3000A:** confirmed by its owner, an SCPH-70004. mxlink's
`ps2client status` reports the model too. On another console, check these before accepting a run,
and record them in a table like this one:

| Check | Real R3000A | This console | Strength |
|---|---|---|---|
| Model | SCPH-70xxx or older | SCPH-70004 (ROMVER `0200EC20040614`, BIOS 2.00, PAL slim) | Strong, the boundary is from `ppctty`'s README and the PPC-Monitor notes |
| ROM file table | No `XPARAM`, no PPC file | None of them in `rom0:ROMDIR` | Good. That Deckard ROMs have XPARAM comes from ps2sdk `iop/deckard`, not checked on a ROM |
| `GM_IF` (0x1F801450) bit 31 | Clear | `0x00000021`: clear | Weak alone: only ps2sdk says so, never seen set here |
| PRId (COP0 15) | Not a Deckard value | `0x00000024` | Data point for this core, see below |

`r3000a.irx` stops without results if `GM_IF` bit 31 is set. That bit is the only check it can do
alone, and **it was never tried on a Deckard**, so the model still has to be checked by hand.

- PRId `0x24` is Imp 0, Rev 0x24, on a real R3000A; the manual gives Imp 3 for IDT's parts. A search
  result (not verified) says Deckard reports "3.0".
- In ps2sdk's `iop_regs.h`, `IOP_TYPE_POWERPC` is 1 but `IOP_CPU_TYPE` is masked with 0x80000000, so
  `IOP_CPU_TYPE == IOP_TYPE_POWERPC` is never true. Test against `IOP_TYPE_MIPSR3000` (0), as
  `ppctty` does.

## Findings to act on

Open unless marked. The IOP bench (`apps/llvm_bench/iop`) measures real patterns; its README has what it changed in LLVM.

| # | Finding | Effect | Proposed |
|---|---|---|---|
| 1 | `-mcpu=r3000a` alone defaults to hard float: `add.s` for a float add, `__mips_hard_float` defined | FPU instructions trap on the IOP (CU1 is clear). Our builds pass `-msoft-float`, so they are safe | Soft float by default for r3000a |
| 2 | The assembler accepts `cache` and `pref`, but `cache` is a reserved instruction here and `pref` traps as coprocessor unusable | Assembled code traps. In ps2sdk `pref` sits behind `USE_PREFETCH` in sysclib (seen in `memset.S`) | Reject, or keep for assembling only and say so |
| 3 | `.set reorder` adds no load, HI/LO or `mfc0` delay: `lw; addu` and `mflo; mult` come out as written | Hand-written `.S` and inline asm that counts on gas's nops is wrong here | Add it to the assembler, or document that asm must be scheduled by hand |
| 4 | `mfc0` is not in `HasLoadDelaySlot`, and inline asm is not looked into: `mfc0 $1, $12` was followed directly by `addiu $1, $1, 1` | The use reads the **old** register (measured). Silent wrong value | Rule for asm authors: end with a `nop`. Or treat `=r` asm outputs as late on r3000a |
| 5 | Model: mult latency 12, div 35. Measured `mflo` issue distance: 7, 10 or 14 after mult, 37 after div | Only scheduling quality. `mult`'s cost depends on rs, which a static model cannot see | **Done:** mult 14, div 37, stores 2 cycles, `mfc0` delay. No kernel of the bench changed |
| 6 | i64 multiply spills HI/LO through the stack and reloads with `mtlo`/`mthi` just to `mflo` again | **Fixed:** the scheduler kept all multiplies apart from their results; `MipsAccOrderDAGMutation` | - |
| 7 | `lw`/`lwl` to one register: `lwl` or `lwr` in the slot of a plain load gets the loaded value, but `SafeInLoadDelaySlot` only allows lwl/lwr pairs | One `nop` too many, rarely | Low priority |

## Latencies and costs

Issue is one instruction per cycle.

| Operation | Manual | LLVM model | Measured | Status |
|---|---|---|---|---|
| ALU, shift | 1 | 1 | 1.00 | measured |
| Branch, taken or not | 1 delay slot | 1 | 2.00 for branch + slot: no penalty | measured |
| Load, D-cache hit | Result 1 late | Latency 2 | 1.00 issue, 1 delay slot | measured |
| Load, D-cache miss | - | Not modelled | +5 cycles | measured |
| Load, uncached RAM | - | Not modelled | 7 (`lw`), 8 with a `nop` after it | measured |
| Load, 0x1F800000 region | - | Not modelled | 1.00 | measured |
| Load, I_STAT register | - | Not modelled | 4.93 | measured |
| Load, BIOS ROM | - | Not modelled | 33.93 | measured |
| `lwl` + `lwr` | Pair needs no nop | Pair allowed | 2.00 | measured |
| Store | Write buffer, 4 entries | Own unit, busy 2 | One per 2 cycles at most; 1.83 in a burst of 16 (see below) | measured |
| Store then load, cached | - | - | 2.00: no penalty | measured |
| Store then load, uncached | Buffer drains first | - | 10.96 | measured |
| `mfc0` | Result 1 late | Latency 2 | 1.00 issue, 1 delay slot; SR, Cause, EPC, PRId alike | measured |
| `mtc0` (SR) | Result 1 late | Not modelled | 1.00 | measured |
| `rfe` | - | Not modelled | 1.00 | measured |
| `mfhi`, `mflo` idle, `mthi`, `mtlo` | - | - | 1.00, no delay on the result | measured |
| `jal` + slot, `j` + slot | 1 delay slot | - | 2.00 | measured |
| `jr`, `jalr` + slot | 1 delay slot | - | 2.00 (4.00 with the `la` before them) | measured |
| Call and return, `jal` + `jr`, both with a `nop` | - | - | 4.00: no penalty | measured |
| Trap round trip: trap, vector `j` + slot, 9 handler instructions with `rfe` | - | - | 14.1 for these 12 instructions; `syscall`, `break`, reserved and coprocessor unusable within 0.1 | measured |
| Result of one unit into another: ALU to load address, branch, store data; `mflo` to ALU | - | - | No stall | measured |
| `mult`, `multu` issue | - | Unit busy 14 | 1.00: never stalls, a new one replaces the running one | measured |
| `mult` + `mflo` | 12 clocks, "11 late" | Latency 14 | **8, 11 or 15**, see below | measured |
| `div`, `divu` issue | - | Unit busy 37 | 1.00 | measured |
| `div` + `mflo` | 35 | Latency 37 | **38**, whatever the operands, also /0 | measured |
| Fetch, cached (kuseg, kseg0) | - | - | 1.00 | measured |
| Fetch, uncached (kseg1) | - | - | 6.06 per instruction | measured |

The burst of stores: 1.83 is under the 2.00 of `sw` + `nop`, probably because the buffer drains
during the 3 loop instructions that are subtracted. 2 is the rate to use.

### Multiply: the cost depends on rs alone

`mult` + `mflo`, whole pair, 225 operand pairs each. `mflo` issues one cycle before the end, so
the distance is the value minus 1. rt never changes the result.

| rs | `mult` | `multu` |
|---|---|---|
| fits 12 bits: -0x800 to 0x7FF (`mult`), below 0x800 (`multu`) | 8 | 8 |
| fits 21 bits: -0x100000 to 0xFFFFF (`mult`), below 0x100000 (`multu`) | 11 | 11 |
| larger | 15 | 15 |

So `multu` by a negative number viewed as unsigned costs 15, and the same bits in `mult` may cost 8.
**Put the smaller operand in rs.** LLVM does this when it can prove which one is smaller.

Overlap, large rs: 4 or 8 `nop`s between `mult` and `mflo` cost nothing (15), 16 `nop`s give 18, no
stall left. For `div` 16 `nop`s still give 38, 48 give 50.

### Caches

| Fact | Value | Evidence | Status |
|---|---|---|---|
| D-cache exists | Yes | Cached load 1, uncached 7. Sony's IOP guide calls it write-through | measured |
| D-cache size | At least 1 KiB, under 2 KiB | Walk at 16-byte stride: 4.09 per access at 1 KiB, 9.08 at 2 KiB | measured |
| D-cache line | 16 bytes | 32 KiB walk by stride 4/8/16/32/64: 5.25, 6.52, 9.05, 9.06, 9.07 | measured |
| D-cache miss | +5 cycles | 9.05 against 4.09 | measured |
| Store allocates? | No | Store + load per line over 32 KiB: 13.10; over 512 bytes: 5.17 | measured, by inference |
| I-cache size | About 8 KiB | Nop loop, cycles per instruction: 1.00 to 4 KiB, 1.02 from 4.5 to 8 KiB, 1.71 at 12 KiB, 2.14 at 16 KiB. The small step above 4 KiB is unexplained | measured |
| I-cache organisation | Consistent with direct mapped | 12 KiB loop misses about half as much as 16 KiB | measured, by inference |
| I-cache miss | About 4.5 cycles per 16 bytes | Same loops. Line size not measured | measured, by inference |
| Region at 0x1F800000 | 1 KiB of RAM | Patterns read back in the first and last word; 1-cycle loads; reads from +0x400 are bus errors | measured |
| That region's relation to the D-cache | Same size; separate or shared RAM is unknown | - | open |

## Hazards

| Hazard | Manual | LLVM | Hardware | LLVM status |
|---|---|---|---|---|
| Use right after a load | Not allowed | `handleLoadDelaySlot` | **Reads the old value** (no interlock, no time lost: `lw` + use 2.00) | Handled |
| Use one instruction later | Fine | - | New value | - |
| Base register of the next load | Same | Same pass | Old base is used | Handled |
| Branch or `jr` on a just-loaded register | Same | Same pass | Old value decides | Handled |
| Load in a branch delay slot, used at the target | Same | `MipsDelaySlotFiller` (`mips1-load-in-delay-slot.ll`) | Old value at the target, new one after | Handled |
| Write to the register in the load's delay slot | Not stated | No rule needed | **The write wins**, the load is dropped. Same as program order | Safe |
| Two loads to one register | Not stated | No rule needed | The second wins. An instruction in the second's slot reads the value from **before both**, so the first load is cancelled | Safe, the pass treats that use as a hazard |
| `lwl`/`lwr` pair | No nop between | Allowed in `SafeInLoadDelaySlot` | Merges correctly. Use after the pair reads the old value | Handled |
| `lwl`/`lwr` after a plain load | Not stated | Adds a `nop` | Gets the loaded value | Safe, see finding 7 |
| `mfc0` then use | Table 13.1, tick | Not handled | **Reads the old value** | Inline asm only: finding 4 |
| `mflo`, `mfhi` then use | Not listed | Nothing needed | New value at once, no delay slot | Safe |
| `mtc0` SR then `mfc0` | Table 13.1, tick | Not handled | **New value at once**, also with nothing between them | Inline asm only |
| `mfhi`/`mflo` within two of `mult`/`div` | Wrong after an exception | `handleMFLOSlot`, also across blocks and in delay slots | **Not reproduced**: 0 wrong of 3,600,000 at each distance 0, 1, 2, interrupts on. Interrupts were not counted, so this does not show the rule is unneeded | Handled, rule kept |
| `mult`/`div` while another runs | Not stated | Not needed | The last one's result counts, the first is dropped | Safe |
| `mtlo`/`mthi` while `mult`/`div` runs | Not stated | Not handled | **The operation stops and leaves partial values**, see below | No known case |
| `jal` link read in its delay slot | Not stated | - | New `ra` (link + 8) | Safe |
| `mtc0` side effects | Up to 3 instructions; interrupts 2 | Not handled | Not tested: only the read-back above | Inline asm only |
| Changing IEc and IM in one `mtc0` | Not recommended | Not handled | Not tested | Inline asm only |
| Data bus error | Exceptions are precise | - | **EPC is 3 instructions after the load**, not the load | Debugging only |
| Divide by zero | Undefined | `bnez` + `break 7` (no `teq`: it traps) | See below | Handled |

Table 13.1 prints "3" in the Hazard column of the load, `mfc0`/`mtc0` and FP rows, on the PDF page
too. It is a tick in the wrong font: the text says a tick marks a hazard, and the load row is one.

`mtlo`/`mthi` on a running multiply, 3 x 7 with LO = 0x1234 before it:

| Sequence | Reads |
|---|---|
| `mult; mtlo; mflo` | The new LO |
| `mult; mtlo; mfhi` | HI from before the `mult` (0x66) |
| `mult; mthi; mflo` | 3: a partial value |
| `mult; 3 nops; mthi; mflo` | 0xA8000000: a partial value |
| `mult; 20 nops; mthi; mflo` | 21: it had finished |
| 21 / 7: `div; mthi; mflo` | 0x15: the dividend, not the quotient |
| 22 / 7: `div; mtlo; mfhi` | 0 |

Undefined divides, the same for each run: `7 / 0` LO `0xFFFFFFFF`, HI 7; `-7 / 0` LO 1, HI -7;
`divu 7 / 0` LO `0xFFFFFFFF`, HI 7; `0x80000000 / -1` LO `0x80000000`, HI 0.

## Instructions

Asm and disasm are checked with `llvm-mc` and `llvm-objdump`. **Result**: compared with a
hand-worked value on the hardware (84 checks, 0 failed, edge cases included). **Cycles**: that
instruction alone, cached, repeated, to within 0.01. "Independent" and "dependent" are a run of
the instruction without and with each one using the result before it; where one number is
given they are equal or the second does not apply. **Seen**: in clang's output in a sample
(`ps2link`, `chime`, this probe's C, small test files). "Not seen" does not mean LLVM cannot
emit it.

### Loads and stores

| Instruction | LLVM asm + disasm | LLVM emits | Result | Cycles |
|---|---|---|---|---|
| `lb` | Yes | Not seen | Correct | 1, uncached 7 |
| `lbu` | Yes | Seen | Correct | 1 |
| `lh` | Yes | Not seen | Correct | 1, uncached 7 |
| `lhu` | Yes | Seen | Correct | 1 |
| `lw` | Yes | Seen | Correct | 1, uncached 7 |
| `lwl`, `lwr` | Yes | Seen | Correct, all 4 offsets | 1, `lwl` uncached 7 |
| `sb`, `sh`, `sw`, `swl`, `swr` | Yes | Seen | Correct, `swl`/`swr` all 4 offsets | 2; cached and uncached alike |

Stores are 1.83 in a burst of 16 and 2.00 with a `nop` after each. Unaligned `lw`/`lh`/`sw`/`sh`
trap (AdEL 4, AdES 5): measured.

### ALU

All 1 cycle, independent and dependent: no operand or forwarding stall. `add`, `addi`, `sub` overflow traps (Ov 12).
`sllv`, `srlv`, `srav` use 5 bits of the amount, and cost the same for 0, 1 and 31.

| Instruction | LLVM asm + disasm | LLVM emits | Result | Cycles |
|---|---|---|---|---|
| `add`, `addi`, `sub` | Yes | Not seen | Correct | 1 |
| `addu`, `addiu`, `subu` | Yes | Seen | Correct | 1 |
| `and` | Yes | Not seen | Correct | 1 |
| `andi`, `or`, `ori`, `xor` | Yes | Seen | Correct | 1 |
| `xori`, `nor` | Yes | Not seen | Correct | 1 |
| `lui` | Yes | Seen | Correct | 1 |
| `slt`, `slti`, `sltu`, `sltiu` | Yes | Seen | Correct | 1 |
| `sll`, `srl`, `sllv` | Yes | Seen | Correct | 1 |
| `sra`, `srlv`, `srav` | Yes | Not seen | Correct | 1 |

### Multiply and divide

| Instruction | LLVM asm + disasm | LLVM emits | Result | Cycles |
|---|---|---|---|---|
| `mult`, `multu` | Yes | Seen | Correct | Issue 1; with `mflo` 8, 11 or 15 |
| `div`, `divu` | Yes | Seen | Correct | Issue 1; with `mflo` 38 |
| `mfhi`, `mflo` | Yes | Seen | Correct | 1 when idle |
| `mthi`, `mtlo` | Yes | Seen | Correct | 1 |

### Jumps and branches

Taken and not taken both cost the 2 cycles of branch + slot.

| Instruction | LLVM asm + disasm | LLVM emits | Result | Cycles with slot |
|---|---|---|---|---|
| `j`, `jal` | Yes | Seen | Correct | 2 |
| `jr` | Yes | Seen | Correct | 2 |
| `jalr` | Yes | Not seen | Correct | 2 |
| `beq`, `bne`, `blez`, `bltz`, `bgez` | Yes | Seen | Correct | 2 |
| `bgtz` | Yes | Not seen | Correct | 2 |
| `bltzal`, `bgezal` | Yes | Not seen | Correct | 2 |

### System and coprocessors

| Instruction | LLVM asm + disasm | LLVM emits | Hardware (ExcCode) | Cycles |
|---|---|---|---|---|
| `syscall` | Yes | Not seen | Traps (8) | Round trip above |
| `break` | Yes | Seen (divide by zero) | Traps (9) | Round trip above |
| `mfc0` | Yes | Inline asm | Works, delay measured | 1 |
| `mtc0` | Yes | Inline asm | Works on SR, read back | 1 |
| `rfe` | Yes | Inline asm | Works: ended every trap of the probe | 1 |
| `tlbp`, `tlbr`, `tlbwi`, `tlbwr` | Yes | No | **Reserved (10)**: no TLB | - |
| `bc0f`, `bc0t` | **Rejected** | No | Coprocessor unusable (11) | - |
| `mfc1`, `mtc1`, `cfc1`, `ctc1`, `lwc1`, `swc1`, `bc1f`, `bc1t`, `add.s` | Yes | **Yes without `-msoft-float`**: finding 1 | Coprocessor unusable (11), COP1 | - |
| `mfc2`, `mtc2`, `lwc2`, `swc2` | Yes | No | Coprocessor unusable (11), COP2 | - |
| `cfc2`, `ctc2`, `cop2`, `bc2f`, `bc2t` | **Rejected** | No | Coprocessor unusable (11), COP2 | - |
| `lwc3` | Yes, disassembles as `pref` | No | Coprocessor unusable (11), COP3 | - |
| `swc3` | Yes | No | Coprocessor unusable (11), COP3 | - |

Only `add.s` was run of the FP operations: the others trap the same way while CU1 is clear.

The COP1 and COP2 traps only show that the unit is **disabled**: SR has CU1 and CU2 clear
(`0x00000401`). They do not show there is no unit. Whether COP2 works with CU2 set is open.

### Not MIPS I

| Instruction | LLVM | Hardware (ExcCode) |
|---|---|---|
| `cache` | **Accepted**: finding 2 | **Reserved (10)** |
| `pref` | **Accepted**: finding 2 | Coprocessor unusable (11): it is `lwc3` |
| `ll`, `sc` | Rejected | Coprocessor unusable (11): the opcodes are `lwc0`, `swc0` |
| `sync`, `teq`, `beql`, `movz`, `mul`, `clz`, `ldc1` | Rejected | Reserved (10) |
| `daddu`, `ld`, `sd` | Not tried | Reserved (10) |
| Opcode 0x1E, function 0x01 | - | Reserved (10) |

The CE field of Cause means something only for coprocessor unusable: it held other values on
address errors and reserved instructions.

## Code scanned for hazards

All clang output, with `objdump -z` (without it, `...` hides nops):

| Code | Instructions | Load-use | HI/LO |
|---|---|---|---|
| mxlink `ps2link` (clang) | 2656 | 0 | 0 |
| mxlink `chime` (clang) | 269 | 0 | 0 |
| `r3000a.c` | 4204 | 0 | 0 |
| `r3000a_asm.S`, the control: hazards on purpose | 7792 | 24 | 183 |

The clang `ps2link.irx` also ran on hardware, see `tools/mxlink` (`IOP_CLANG=1`): hello, fio_stress,
fio_big, iop, all status 0.

## Open: to run on a real R3000A

- `mtc0` side effects: when an interrupt enable or a CU bit takes effect. Other COP0 registers.
- COP2 with CU2 set: is there a unit? Is COP1 absent?
- HI/LO hazard with a counted, high interrupt rate.
- Exception in a branch delay slot; branch in a delay slot.
- I-cache line size. D-cache snooping (Sony's guide claims it, DMA against cached data).
- Whether the 0x1F800000 RAM is separate from the D-cache.
- PRId on other R3000A models.
- Everything again on a second console, to tell the core from this unit.

## Running the probe

```
make -C probe/iop                          # r3000a.irx
make                                       # the EE side, probe/p25_iop.elf, with the other probes
ps2client execee host:p25_iop.elf host:r3000a.irx        # results, hazards, timing, caches
ps2client execee host:p25_iop.elf host:r3000a.irx inst   # every instruction on its own
ps2client execee host:p25_iop.elf host:r3000a.irx exc    # which opcodes trap, cost of a trap
```

Each result line starts with a letter: `i` one instruction, `f` results, `u` undefined results, `h` hazards, `t` timing,
`m`/`d` multiply/divide tables, `s` I-cache, `w`/`v`/`y` D-cache, `p` the 0x1F800000 region, `z` HI/LO
under interrupts, `x` traps.

- `exc` replaces the exception vector for a moment, with interrupts off, and catches everything itself.
- The first form runs for a while, partly without output; the PS2 is not stuck. `status` shows it running.
- **Never read 0x1F800400 and above outside `exc`.** These are bus errors, which go to `ioptrap`
  there and end the run. An earlier version of the probe did this.
- Its trap handler returns to EPC + 4. After a bus error that is not the instruction after the
  load, so `x_peek` has `nop`s to land in.

Sources on the Deckard: [PPC-Monitor architecture overview](https://israpps.github.io/PPC-Monitor/docs/Architecture%20Overview.html),
ps2sdk `iop/debug/ppctty/README.md`, `iop/deckard/xparam`, `common/include/iop_regs.h`.
