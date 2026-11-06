#!/usr/bin/env python3
"""tools/vmsim.py <trace> [options]: replay a VM trace (mx_vm_config.trace,
mx_vm_trace_save; <mx/vm.h>) with other page sizes and eviction policies.

The trace holds, per interval (trace_ms), the 8 KiB blocks the program
touched, in the order of their first touch, read or written. Replayed with
a page size (8, 16, 32 KiB: blocks grouped), a memory size (the run's
frames) and a policy, it gives the faults, the bytes and calls of the swap
file, and their time by the cost of the IOP's calls (docs/vm.md "Costs").

  --window A B   count only between mx_vm_trace_mark(A) and (B) (the replay
                 runs from the start, so memory is warm)
  --mem BYTES    memory for pages (default: the run's frames)
  --pages 8,16,32  page sizes in KiB
  --policies clock,lru,aging,opt
                 clock: second chance (ee/src/vm/vm.c until 2026-11-01); lru
                 (ee/src/vm/vm.c now: by its refills); aging: at each tick a page's age
                 is shifted right with its use in the interval on top (8
                 bits), the lowest goes; opt: Belady's
                 (evicts the page used again last: no policy faults less)
  --cost SEEK,READKB,WRITEKB  ms of a seek, ms per KiB read, written
                 (default 1.0,0.48,1.24: host:, probe/p21_iolat.c)

Also prints the working set of an interval at each size, from 8 KiB to
256 KiB pieces (how much more a larger piece holds than was touched), and
for 32 KiB pages, how many of their four blocks were touched and written
while resident."""
import argparse, heapq, struct, sys
from collections import OrderedDict

READ, WRITE, MARK, TICK = 0, 1, 2, 3
BLK = 8192

ap = argparse.ArgumentParser()
ap.add_argument("trace")
ap.add_argument("--window", nargs=2, type=int)
ap.add_argument("--mem", type=int)
ap.add_argument("--pages", default="8,16,32")
ap.add_argument("--policies", default="clock,lru,aging,opt")
ap.add_argument("--cost", default="1.0,0.48,1.24")
a = ap.parse_args()

data = open(a.trace, "rb").read()
magic, version, page, block, npages, nmap, nframes, interval, nev, lost = struct.unpack_from(
    "<10I", data)
if magic != 0x5456584D or version != 1 or block != BLK:
    sys.exit("vmsim: not a VM trace (version 1, 8 KiB blocks)")
ev = struct.unpack_from("<%dI" % nev, data, 40)
mem = a.mem or nframes * page
seek_ms, read_kb, write_kb = (float(x) for x in a.cost.split(","))
print("trace: %d events (%d lost), %d VM pages + %d of the file window of %d KiB, %d frames "
      "(%d KiB), every %d ms" % (nev, lost, npages, nmap, page // 1024, nframes,
                                  nframes * page // 1024, interval))

# accesses: (block, written) in order; the window as indexes into them;
# intervals as their start indexes
acc, starts, marks, ms = [], [0], {}, []
for e in ev:
    k, v = e >> 30, e & 0x3FFFFFFF
    if k == TICK:
        starts.append(len(acc))
        ms.append(v)
    elif k == MARK:
        marks.setdefault(v, len(acc))
    else:
        acc.append((v, k == WRITE))
w0, w1 = 0, len(acc)
if a.window:
    if a.window[0] not in marks:
        sys.exit("vmsim: marks in the trace: %s" % sorted(marks))
    w0 = marks[a.window[0]]
    if a.window[1] in marks:
        w1 = marks[a.window[1]]
    else:
        print("vmsim: no mark %d (the buffer was full?): the window runs to the end" %
              a.window[1])
nint = sum(1 for s in starts if w0 <= s < w1)
print("window: accesses %d..%d of %d, %d intervals (%.1f s)" %
      (w0, w1, len(acc), nint, nint * interval / 1000))

# the working set of an interval at each piece size
print("\nworking set per interval (mean KiB touched, written; and as pieces of each size):")
bounds = [s for s in starts if w0 <= s <= w1] + [w1]
for kb in (8, 16, 32, 64, 128, 256):
    per = kb * 1024 // BLK
    tot = wtot = 0
    for i in range(len(bounds) - 1):
        seg = acc[bounds[i]:bounds[i + 1]]
        tot += len({b // per for b, _ in seg})
        wtot += len({b // per for b, w in seg if w})
    n = max(len(bounds) - 1, 1)
    print("  %3d KiB pieces: %8.0f KiB touched, %8.0f KiB written" %
          (kb, tot * kb / n, wtot * kb / n))


def simulate(policy, pkb):
    per = pkb * 1024 // BLK
    nfr = mem // (pkb * 1024)
    seq = [(b // per, w) for b, w in acc]
    nxt = None
    if policy == "opt":  # the next use of each access's page
        nxt, last = [0] * len(seq), {}
        for i in range(len(seq) - 1, -1, -1):
            nxt[i] = last.get(seq[i][0], 1 << 62)
            last[seq[i][0]] = i
    res = {}               # page -> [dirty, ref, age]
    ticks = set(starts)
    lru = OrderedDict()
    ring, hand = [], 0     # clock: frames' pages
    heap = []              # opt: (-next use, page)
    nextuse = {}
    seen = set()           # pages resident before: read again (the swap file, or the ELF or
                           # a mapped file for clean ones; a page never written is zero, rare)
    st = dict(faults=0, cold=0, ins=0, outs=0, drops=0)

    def evict(i):
        nonlocal hand
        if policy == "lru":
            pg, _ = lru.popitem(last=False)
        elif policy == "aging":
            pg = min(res, key=lambda q: (256 if res[q][1] else 0) | res[q][2])
        elif policy == "opt":
            while True:
                nu, pg = heapq.heappop(heap)
                if pg in res and nextuse[pg] == -nu:
                    break
        else:
            while True:
                pg = ring[hand]
                if res[pg][1]:
                    res[pg][1] = False
                    hand = (hand + 1) % len(ring)
                    continue
                break
        dirty = res.pop(pg)[0]
        if i >= w0:
            st["outs" if dirty else "drops"] += 1
        seen.add(pg)
        return pg

    for i, (pg, w) in enumerate(seq):
        count = w0 <= i < w1
        if policy == "aging" and i in ticks:
            for r in res.values():
                r[2] = r[2] >> 1 | (128 if r[1] else 0)
                r[1] = False
        if pg not in res:
            if count:
                st["faults"] += 1
                if pg in seen:
                    st["ins"] += 1
                else:
                    st["cold"] += 1
            slot = None
            if len(res) >= nfr:
                old = evict(i if count else -1)
                if policy == "clock":
                    slot = hand
                    hand = (hand + 1) % len(ring)
            res[pg] = [False, True, 0]
            if policy == "clock":
                if slot is None:
                    ring.append(pg)
                else:
                    ring[slot] = pg
        else:
            res[pg][1] = True
        if w:
            res[pg][0] = True
        if policy == "lru":
            lru[pg] = True
            lru.move_to_end(pg)
        elif policy == "opt":
            nextuse[pg] = nxt[i]
            heapq.heappush(heap, (-nxt[i], pg))
    b = pkb * 1024
    t_in = st["ins"] * (seek_ms + read_kb * pkb)
    t_out = st["outs"] * (seek_ms + write_kb * pkb)
    return st, nfr, t_in, t_out, b


print("\n%-6s %4s %6s %8s %6s %7s %7s %7s %9s %9s %9s" %
      ("policy", "page", "frames", "faults", "cold", "ins", "outs", "drops", "in s", "out s",
       "total s"))
for pkb in (int(x) for x in a.pages.split(",")):
    for pol in a.policies.split(","):
        st, nfr, t_in, t_out, b = simulate(pol, pkb)
        print("%-6s %3dK %6d %8d %6d %7d %7d %7d %9.1f %9.1f %9.1f" %
              (pol, pkb, nfr, st["faults"], st["cold"], st["ins"], st["outs"], st["drops"],
               t_in / 1000, t_out / 1000, (t_in + t_out) / 1000))

# 32 KiB pages under the clock: of their four blocks, how many were touched
# and written while resident (one residency each, counted at eviction)
per = page // BLK
nfr = mem // page
res, ring, hand = {}, [], 0
touched = [0] * (per + 1)
written = [0] * (per + 1)
for i, (b, w) in enumerate(acc):
    pg, bit = b // per, 1 << (b % per)
    if pg not in res:
        if len(res) >= nfr:
            while res[ring[hand]][0]:
                res[ring[hand]][0] = False
                hand = (hand + 1) % nfr
            old = res.pop(ring[hand])
            if i >= w0 and i < w1:
                touched[bin(old[1]).count("1")] += 1
                written[bin(old[2]).count("1")] += 1
            ring[hand] = pg
            hand = (hand + 1) % nfr
        else:
            ring.append(pg)
        res[pg] = [True, 0, 0]
    r = res[pg]
    r[0] = True
    r[1] |= bit
    if w:
        r[2] |= bit
n = sum(touched)
if n:
    print("\n%d KiB pages evicted by the clock (%d): blocks touched / written while resident" %
          (page // 1024, n))
    for k in range(per + 1):
        print("  %d of %d: %5.1f%% touched, %5.1f%% written" %
              (k, per, 100 * touched[k] / n, 100 * written[k] / n))
