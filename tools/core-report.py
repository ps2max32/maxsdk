#!/usr/bin/env python3
"""What an MXOS program has in its core and in VM, from its lld map file
(mx OS plan, phase 4a: what the core holds and why).

  core-report.py <program.map> [...]

Per program: the core (from 0x0010_0000 to _end, the main stack included)
by kind (the output section: code, read-only data, data, bss, packets, the
stack) and by where it comes from (crt0, libmxk/libmx, libmxc, picolibc,
compiler-rt, VU and HLSL objects, the program's own MX_CORE_* sections), the
physical run the kernel gives it (_end and its 256 KiB core heap, rounded up
to a MiB: apps/mxos/ee/exec.c's core_place), and the VM's segments."""

import re
import sys

CORE_LO, VM_LO = 0x00100000, 0x08000000
CORE_HEAP = 256 << 10
MIB = 1 << 20

KIND = {
    ".text": "code", ".vutext": "code (VU)", ".rodata": "rodata", ".init_array": "rodata",
    ".note.mxk": "rodata", ".data": "data", ".sdata": "data", ".mx.packets": "packets",
    ".bss": "bss", ".stack": "main stack",
}


def origin(path, section):
    if section.startswith((".text.mx.core", ".rodata.mx.core", ".data.mx.core")):
        return "program: MX_CORE_*"
    if section.startswith(".bss.mx.lines"):
        return "DMA lines"
    if section.startswith(".mx.packets"):
        return "packets"
    if path.endswith("crt0.o"):
        return "crt0"
    m = re.search(r"([^/]+\.a)\(", path)
    if m:
        lib = m.group(1)
        if lib in ("libmxk.a", "libmx.a"):
            return "maxsdk (" + lib + ")"
        if lib == "libmxc.a":
            return "libmxc"
        if lib == "libc.a":
            return "picolibc"
        if "clang_rt" in lib or "builtins" in lib:
            return "compiler-rt"
        if "glue" in path:
            return "libmxc glue"
        return lib
    if "/vu/" in path:
        return "VU objects"
    if "/hlsl/" in path:
        return "HLSL objects"
    if "glue.c.o" in path:
        return "libmxc glue"
    return "program"


def report(path):
    out = None
    kinds, origins = {}, {}
    vm = {}
    end = None
    rx = re.compile(r"^\s*([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+\d+\s(.*)$")
    for line in open(path):
        m = rx.match(line)
        if not m:
            continue
        vma, size, rest = int(m.group(1), 16), int(m.group(2), 16), m.group(3)
        if not rest.strip():
            continue
        if not rest.startswith(" "):
            out = rest.split()[0]
            if out == ".stack" and CORE_LO <= vma < VM_LO:
                kinds["main stack"] = kinds.get("main stack", 0) + size
                origins["main stack"] = origins.get("main stack", 0) + size
            if CORE_LO <= vma < VM_LO:
                end = max(end or 0, vma + size)
            elif vma >= VM_LO and vma < 0x10000000 and out.startswith(".vm."):
                vm[out] = vm.get(out, 0) + size
            continue
        body = rest.strip()
        if "=" in body.split("(")[0] or not size:
            continue
        m2 = re.match(r"(.*):\((\S+)\)$", body)
        if not m2 or not (CORE_LO <= vma < VM_LO) or out == ".stack":
            continue
        k = KIND.get(out, out)
        o = origin(m2.group(1), m2.group(2))
        kinds[k] = kinds.get(k, 0) + size
        origins[o] = origins.get(o, 0) + size

    name = path.rsplit("/", 1)[-1]
    kib = lambda n: "%7.1f" % (n / 1024)
    print("%s: core %s KiB to _end; run %d MiB (with %d KiB core heap)" %
          (name, kib(end - CORE_LO).strip(), (((end + CORE_HEAP + MIB - 1) & ~(MIB - 1)) - CORE_LO) // MIB,
           CORE_HEAP >> 10))
    print("  by kind:")
    for k, n in sorted(kinds.items(), key=lambda x: -x[1]):
        print("   %s KiB  %s" % (kib(n), k))
    print("  by origin:")
    for k, n in sorted(origins.items(), key=lambda x: -x[1]):
        print("   %s KiB  %s" % (kib(n), k))
    print("  VM: " + ", ".join("%s %s KiB" % (k, kib(n).strip()) for k, n in sorted(vm.items())))
    print()


for p in sys.argv[1:]:
    report(p)
