#!/usr/bin/env python3
"""Import libraries (ILB) for ld.lld --irx from ps2sdk's IOP headers.

  mkilb.py <outdir> <include dir>...      (ps2sdk's iop/include and common/include)

Each header that declares an import table, `#define <lib>_IMPORTS_start
DECLARE_IMPORT_TABLE(<lib>, <major>, <minor>)`, and its entries, `#define I_<f>
DECLARE_IMPORT(<n>, <f>)`, becomes <outdir>/<lib>.ilb in Sony's ILB format:

  #IOP-ILB#
  L <library name>
  V <version, 4 hex digits>
  F 0000
  E <index, 3 digits> <entry name>

The x-variants (xstdio: the same library at a newer minor version) are left
out: the loader only checks the major version, and the base table's entries
include theirs. A header with two base tables (mcman.h) cannot say which entry
is whose; it is skipped, with a message.

<outdir>/ps2sdk.rsp lists the libraries for the link (clang @<file>), all but
accdvd: an alternative CD/DVD driver with cdvdman's entry names.
"""
import os
import re
import sys

TABLE = re.compile(
    r'^\s*#\s*define\s+(\w+)_IMPORTS_start\s+DECLARE_IMPORT_TABLE\(\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)',
    re.M)
ENTRY = re.compile(r'^\s*#\s*define\s+I_\w+\s+DECLARE_IMPORT\(\s*(\d+)\s*,\s*(\w+)\s*\)', re.M)


LEAVE_OUT = {'accdvd'}


def write(dest, text):
    tmp = dest + '.tmp%d' % os.getpid()
    with open(tmp, 'w') as o:
        o.write(text)
    os.replace(tmp, dest)


def main():
    outdir, incdirs = sys.argv[1], sys.argv[2:]
    os.makedirs(outdir, exist_ok=True)
    libs = []
    dirs = [(root, files) for d in incdirs for root, _, files in sorted(os.walk(d))]
    for root, files in dirs:
        for f in sorted(files):
            if not f.endswith('.h'):
                continue
            path = os.path.join(root, f)
            text = open(path, encoding='latin-1').read()
            tables = [(lib, int(ma), int(mi)) for prefix, lib, ma, mi in TABLE.findall(text) if prefix == lib]
            if not tables:
                continue
            if len(tables) > 1:
                print(f'mkilb.py: {f}: {len(tables)} import tables, skipped', file=sys.stderr)
                continue
            lib, major, minor = tables[0]
            entries = sorted({(int(n), name) for n, name in ENTRY.findall(text)})
            if not entries:
                continue
            out = [f'#IOP-ILB#', f'L {lib}', f'V {major:02x}{minor:02x}', 'F 0000']
            out += [f'E {n:03d} {name}' for n, name in entries]
            if lib in libs:
                print(f'mkilb.py: {f}: library {lib} again, skipped', file=sys.stderr)
                continue
            libs.append(lib)
            write(os.path.join(outdir, lib + '.ilb'), '\n'.join(out) + '\n')
    rsp = [os.path.join(outdir, lib + '.ilb') for lib in sorted(libs) if lib not in LEAVE_OUT]
    write(os.path.join(outdir, 'ps2sdk.rsp'), '\n'.join(rsp) + '\n')
    print(f'mkilb.py: {len(libs)} libraries in {outdir}')


main()
