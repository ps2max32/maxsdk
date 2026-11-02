#!/usr/bin/env python3
"""tools/prof.py <elf> <log>: the "prof:" lines of mx_prof_report as functions.

Each bucket (16 bytes of code) goes to the function at its address (llvm-nm
of the ELF); prints the functions by samples, with their share."""
import bisect, os, re, subprocess, sys

elf, log = sys.argv[1], sys.argv[2]
llvm = os.environ.get("LLVM", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "../../../build/toolchain/llvm/feature/bin"))
syms = []
for line in subprocess.run([os.path.join(llvm, "llvm-nm"), "-n", elf], capture_output=True,
                           text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
total, by = 0, {}
for line in open(log, errors="replace"):
    m = re.search(r"prof: total (\d+)", line)
    if m:
        total = int(m.group(1))
    m = re.search(r"prof: ([0-9a-f]{8}) (\d+)", line)
    if m:
        pc, n = int(m.group(1), 16), int(m.group(2))
        name = "(kernel)" if pc >= 0x80000000 else "?"
        if pc < 0x80000000:
            i = bisect.bisect_right(addrs, pc + 8) - 1
            if i >= 0:
                name = syms[i][1]
        by[name] = by.get(name, 0) + n
for name, n in sorted(by.items(), key=lambda x: -x[1])[:40]:
    print(f"{100.0 * n / max(total, 1):5.1f}%  {n:6d}  {name}")
