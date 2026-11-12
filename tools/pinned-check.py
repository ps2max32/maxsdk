#!/usr/bin/env python3
"""Code that runs with interrupts off reaches only pinned code (mx OS plan,
phase 4b).

  pinned-check.py <program.elf> [...]     (its map next to it: <program>.map)

A page of VM that is not resident cannot be brought in with interrupts off
(a handler, code under mx_di): the fault ends the program. The roots are
  - the functions marked MX_CORE_FN (input sections .text.mx.core*, from the
    map): interrupt handlers and what the kernel calls with interrupts off;
  - the calls a function makes after a di, until the next ei.
Every function reached from them by a call (jal, j) must be in core (below
0x0800_0000); each one in VM is reported with the root it is reached from.
Calls through a pointer (jalr) are not seen. Exit status 1 when anything is
reported. LLVM is the directory of llvm-objdump (default: the feature
LLVM's build)."""

import os
import re
import subprocess
import sys

VM_LO, VM_HI = 0x08000000, 0x10000000
HERE = os.path.dirname(os.path.abspath(__file__))
LLVM = os.environ.get("LLVM", os.path.join(HERE, "../../../build/toolchain/llvm/feature/bin"))


def marked(mapfile):
    """The functions in .text.mx.core* input sections."""
    out, sec = set(), None
    rx = re.compile(r"^\s*[0-9a-f]+\s+[0-9a-f]+\s+[0-9a-f]+\s+\d+\s(.*)$")
    for line in open(mapfile):
        m = rx.match(line)
        if not m:
            continue
        rest = m.group(1)
        body = rest.strip()
        if not rest.startswith(" "):
            sec = None
        elif re.search(r":\(\S+\)$", body):
            sec = re.search(r":\((\S+)\)$", body).group(1)
        elif sec and sec.startswith(".text.mx.core") and "=" not in body:
            out.add(body)
    return out


BRANCH = re.compile(r"^(b|beq|bne|beqz|bnez|blez|bgtz|bltz|bgez|beql|bnel|beqzl|bnezl|blezl|bgtzl|"
                    r"bltzl|bgezl|bc0f|bc0t|bc1f|bc1t|bc0fl|bc0tl|bc1fl|bc1tl)$")


def window_calls(ins, di_at):
    """The calls on the paths from the di at index di_at until interrupts
    are as before: an ei, or mx_irq_restore's branch over its ei (taken when
    they were off before the di: the caller's state again), or the
    function's end. Branches within the function are followed; the delay
    slot after a branch or jump runs on both paths. ins: [(addr, op, args)]."""
    index = {a: i for i, (a, _, _) in enumerate(ins)}
    out, seen, work = set(), set(), [di_at + 1]

    def target(args):
        t = re.search(r"0x([0-9a-f]+)", args)
        return index.get(int(t.group(1), 16)) if t else None

    while work:
        i = work.pop()
        while 0 <= i < len(ins) and i not in seen:
            seen.add(i)
            _, op, args = ins[i]
            if op == "ei":
                break
            if op == "jal":
                t = re.match(r"0x([0-9a-f]+)", args)
                if t:
                    out.add(int(t.group(1), 16))
            if op == "jr":
                if i + 1 < len(ins):  # its delay slot
                    seen.add(i + 1)
                break
            if op == "j" or BRANCH.match(op):
                to = target(args)
                # mx_irq_restore: a branch that skips an ei just after it
                # (forward over it, or back), taken when interrupts were
                # off before the di
                over_ei = op != "j" and op != "b" and to is not None and (
                    any(ins[k][1] == "ei" for k in range(i + 1, min(i + 3, len(ins)))) or
                    (i < to <= i + 12 and any(ins[k][1] == "ei" for k in range(i + 1, to))))
                if over_ei:
                    break  # mx_irq_restore: as before on both paths
                if op == "j" and to is None:
                    t = re.match(r"0x([0-9a-f]+)", args)
                    if t:
                        out.add(int(t.group(1), 16))  # a tail call
                if to is not None:
                    work.append(to)
                if i + 1 < len(ins):  # the delay slot
                    _, dop, dargs = ins[i + 1]
                    seen.add(i + 1)
                    if dop == "jal":
                        t = re.match(r"0x([0-9a-f]+)", dargs)
                        if t:
                            out.add(int(t.group(1), 16))
                if op in ("j", "b"):
                    break  # no fall through
                i += 2
                continue
            i += 1
    return out


def check(elf):
    """Functions by their address (static ones may share a name)."""
    dis = subprocess.run([os.path.join(LLVM, "llvm-objdump"), "-d", "--mcpu=r5900", "--no-show-raw-insn", elf],
                         capture_output=True, text=True, check=True).stdout
    name, calls, body = {}, {}, {}
    func = None
    fn_rx = re.compile(r"^([0-9a-f]+) <(.+)>:$")
    ins_rx = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
    for line in dis.splitlines():
        m = fn_rx.match(line)
        if m:
            func = int(m.group(1), 16)
            name[func] = m.group(2)
            calls[func], body[func] = set(), []
            continue
        m = ins_rx.match(line)
        if not m or func is None:
            continue
        op, args = m.group(2), m.group(3)
        body[func].append((int(m.group(1), 16), op, args))
        if op in ("jal", "j"):
            t = re.match(r"0x([0-9a-f]+)", args)
            if t and int(t.group(1), 16) in name or (t and op == "jal"):
                calls[func].add(int(t.group(1), 16))
    window = {}
    for f, ins in body.items():
        w = set()
        for i, (_, op, _) in enumerate(ins):
            if op == "di":
                w |= window_calls(ins, i)
        window[f] = w - {f}

    mapfile = os.path.splitext(elf)[0] + ".map"
    want = marked(mapfile) if os.path.exists(mapfile) else set()
    roots = {f: name[f] for f in name if name[f] in want and f < VM_LO}
    work = list(roots)
    for f, w in window.items():
        for c in w:
            if c not in roots:
                roots[c] = name[f] + " (with interrupts off)"
                work.append(c)
    seen = dict(roots)
    while work:
        f = work.pop()
        for c in calls.get(f, ()):
            if c not in seen:
                seen[c] = seen[f]
                work.append(c)
    elfname = os.path.basename(elf)
    bad = sorted((name.get(f, hex(f)), r) for f, r in seen.items() if VM_LO <= f < VM_HI)
    for f, r in bad:
        print("%s: %s is VM, reached from %s" % (elfname, f, r))
    return bool(bad)


bad = [check(e) for e in sys.argv[1:]]
if any(bad):
    print("pinned-check: code run with interrupts off must be core: MX_CORE_FN, or its object "
          "in ee/ld/maxsdk-vm.ld's pinned list (mx OS plan 4b)", file=sys.stderr)
sys.exit(1 if any(bad) else 0)
