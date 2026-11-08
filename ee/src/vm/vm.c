/* Virtual memory (<mx/vm.h>, docs/vm.md).

   VM is MX_VM_BASE + npages 32 KiB pages, and the file window
   MX_VM_MAP_BASE + nmap pages (mx_vm_map_file): pages npages.. of the
   tables, read from their files and never written (va_page, page_va). A page is one TLB entry, both of
   its 16 KiB halves (PageMask 16 KiB) valid: an entry with a V = 0 half
   hangs the PS2 (the TLB invalid exception, probe/p19_tlb.c), so a page is
   either in the TLB whole or not at all, and every miss is a refill. The
   TLB is written with tlbwi only, at entries
   TLB_FIRST..47 in turn, with a table of what each holds: PCSX2 keeps
   Random at 0 (tlbwr would overwrite the scratchpad's entry 0) and its tlbp
   finds nothing. The BIOS's own entries 0..21 (scratchpad, I/O, main
   memory cached) stay; its 9 entries of uncached main memory (22..30) and
   8 of UCAB (31..38) become one each (22, 23: 16 MiB halves), so the VM
   has 24..47 (tlb_init).

   Per page: lo[] is its EntryLo0 when mapped (V set; EntryLo1 is the next
   16 KiB), frame_of[] its frame, slot_of[] its slot in the swap file,
   flags[] and pins[]. Per frame: page_of[]. A page is
   - not resident: no frame; zero (never written out) or in its slot;
   - resident and mapped: may be in the TLB, REF set; writable once written
     (D), or always when dirty tracking is off;
   - resident, not mapped: the clock took it out of the TLB to see whether
     it is used again;
   - busy: being written to or read from the swap file.

   The exception (mx__vm_exception, at EXL = 1) does as little as it can:
   it refills the TLB, marks a page dirty and maps a resident page again.
   Everything else (a new page's zero fill, eviction by the clock: second
   chance, the swap file) is done by mx__vm_slow in the faulting thread,
   under the VM lock, with interrupts on.
   Thread code changes the tables with interrupts off; the exception can
   come at any time between those sections and sees a page as busy while
   its frame is in transit. */

#include <mx/bios.h>
#include <mx/debug.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/thread.h>
#include <mx/timer.h>
#include <mx/vm.h>

#include "vm_int.h"

void mx__thread_keep(int id); /* thread.c: mx_exit leaves it */

#define PAGE MX_VM_PAGE
#define PAGE_SHIFT 15
#define HALF (PAGE / 2)
#define PM16K 0x6000u
#define PM4K 0u
#define BLK 8192u /* with a trace: what a TLB entry maps (PageMask 4 KiB) */
#define BPP (PAGE / BLK)
#define TLB_FIRST VMF_TLB_FIRST /* 24 */
#define PM16M 0x01FFE000u
#define TLB_N 48
#define FRAME_TAKEN 0xFFFFu  /* page_of: taken, its page not there yet */
#define FRAME_PINNED 0xFFFEu /* page_of: pinned memory (<mx/mem.h>), out of the pool */

/* EntryLo bits */
#define LO_G 1u
#define LO_V 2u
#define LO_D 4u
#define LO_CACHED (3u << 3)

/* page flags */
#define PG_DIRTY 1 /* written since its frame was last written out */
#define PG_REF 2   /* mapped since it came in (the trace's pins) */
#define PG_BUSY 4  /* in transit to or from the swap file */
#define PG_SWAP 8  /* its slot holds it */
#define PG_FILE 16 /* a file holds it (the program's ELF until PG_SWAP, a mapped file) */
#define NSEG 12    /* 4 segments of the program's ELF, 8 mapped files */
#define PG_RAW 32  /* pinned to be overwritten whole: its frame not filled */

static struct {
  int active;
  u32 npages, nframes;
  u8 *frames;
  u32 *lo;
  u16 *frame_of, *slot_of, *page_of;
  u8 *flags, *pins;
  u8 *page_slot; /* TLB entry of each page (with a trace: of each block), 0: none */
  u32 bpp;              /* TLB entries a page can have: 1, BPP with a trace */
  u32 nfree, free_hint;
  int dbit;
  int fd;
  u32 nslots;
  int lock;
  struct mx_vm_stats st;
  u32 modified_seen;
  int code; /* the program runs code from VM (ee/src/vm/image.c) */
  int wsema;        /* wakes the writer thread; -1: none */
  int swapsema;     /* the swap file's seek and transfer (swap_io) */
  u32 clean_frames; /* what it keeps free or clean */
  /* pages read from files (PG_FILE): the segments of a program linked
     into VM (its ELF) and the mapped files; fd -1: a free entry */
  u32 nseg;
  struct {
    u32 pg, npg, filesz, offset;
    int fd;
  } seg[NSEG];
  u32 nmap, map_next; /* pages of the file window, the next one not given out */
  u32 nheap, heap_next; /* pages of the heap window, the next one not given out */
  u32 pinned_frames;    /* frames out of the pool as pinned memory */
  struct {
    u32 *buf, cap, n, lost;
    u8 *seen; /* per block, this interval: epoch << 2 | written << 1 | read */
    u32 epoch, ms, every_ms, cyc, t_last;
  } tr; /* the trace (mx_vm_config.trace) */
} vm;

/* tlb.S's fast refill reads it (vm_int.h) */
struct mx__vm_fast mx__vm_fast MX_ALIGNED(16);
#define F mx__vm_fast
_Static_assert(VMF_BASE_PAGE == MX_VM_BASE >> PAGE_SHIFT, "tlb.S");

/* The tables' pages: VM's (0 .. npages), the file window's (.. + nmap),
   the heap window's (.. + nheap). */
static u32 page_va(u32 pg) {
  if (pg < vm.npages)
    return MX_VM_BASE + pg * PAGE;
  if (pg < vm.npages + vm.nmap)
    return MX_VM_MAP_BASE + (pg - vm.npages) * PAGE;
  return MX_VM_HEAP_BASE + (pg - vm.npages - vm.nmap) * PAGE;
}

/* The page of va, or ~0 (outside the windows in use). */
static u32 va_page(u32 va) {
  if (va - MX_VM_BASE < vm.npages * PAGE)
    return (va - MX_VM_BASE) >> PAGE_SHIFT;
  if (va - MX_VM_MAP_BASE < vm.nmap * PAGE)
    return vm.npages + ((va - MX_VM_MAP_BASE) >> PAGE_SHIFT);
  if (va - MX_VM_HEAP_BASE < vm.nheap * PAGE)
    return vm.npages + vm.nmap + ((va - MX_VM_HEAP_BASE) >> PAGE_SHIFT);
  return ~0u;
}

/* a page of the file window: read-only */
static int is_mapped(u32 pg) { return pg - vm.npages < vm.nmap; }
static u32 npages_all(void) { return vm.npages + vm.nmap + vm.nheap; }

void mx__vm_code(void) { vm.code = 1; }

/* A frame got new contents (written through the D-cache, or by DMA): with
   code in VM, out of the D-cache and the I-cache before the page is mapped.
   The I-cache is indexed by the virtual address, and every page starts at
   the same index, so a line of the frame's page before could still hit.
   Thread code with interrupts on (positive syscalls turn them on). */
static void code_sync(void) {
  if (vm.code) {
    mx_bios_flush_cache(0);
    mx_bios_flush_cache(2);
  }
}

/* tlb.S */
void mx__vm_vector(void);
void mx__vm_resume_k(void);
void mx__vm_resume(struct mx__vm_x *f) __attribute__((noreturn));
void mx__vm_slow(struct mx__vm_x *f) __attribute__((noreturn, used));
int mx__vm_exception(struct mx__vm_x *x) __attribute__((used));
struct mx__vm_x mx__vm_xs MX_ALIGNED(16);
u8 mx__vm_xstack[8192] MX_ALIGNED(16);

int mx_vm_active(void) { return vm.active; }
u32 mx_vm_size(void) { return vm.npages * PAGE; }

static u8 *frame_ptr(u32 f) { return vm.frames + f * PAGE; }

/* ---- the TLB -------------------------------------------------------------- */

/* The running process's address space (docs/mx-os-plan.md, step 2b): EntryHi's
   ASID. The VM's entries are its (G = 0); the kernel sets it for a libmxk
   program (mx__asid_set), 0 otherwise. EntryHi is also the CPU's ASID: tlbwi
   and tlbr change it, so both set it back. */
u32 mx__asid;

void mx__asid_set(u32 asid) {
  mx__asid = asid & 0xFFu;
  __asm__ volatile("mtc0 %0, $10\n\tsync.p" : : "r"(mx__asid) : "memory");
}

static void tlb_write(u32 i, u32 pm, u32 hi, u32 lo0, u32 lo1) {
  __asm__ volatile("mtc0 %0, $0\n\tmtc0 %1, $5\n\tmtc0 %2, $10\n\tmtc0 %3, $2\n\tmtc0 %4, $3\n\t"
                   "sync.p\n\ttlbwi\n\tsync.p\n\tmtc0 %5, $10\n\tsync.p"
                   :
                   : "r"(i), "r"(pm), "r"(hi), "r"(lo0), "r"(lo1), "r"(mx__asid)
                   : "memory");
}

static void tlb_read(u32 i, u32 *pm, u32 *hi, u32 *lo0, u32 *lo1) {
  __asm__ volatile("mtc0 %4, $0\n\tsync.p\n\ttlbr\n\tsync.p\n\t"
                   "mfc0 %0, $5\n\tmfc0 %1, $10\n\tmfc0 %2, $2\n\tmfc0 %3, $3\n\t"
                   "mtc0 %5, $10\n\tsync.p"
                   : "=r"(*pm), "=r"(*hi), "=r"(*lo0), "=r"(*lo1)
                   : "r"(i), "r"(mx__asid));
}

/* A TLB entry for something new: the next one in turn, freed of what it
   held. Interrupts off (or at EXL). */
static u32 tlb_take(void) {
  u32 s = F.next_slot;
  F.next_slot = F.next_of[s];
  u32 old = F.slot_page[s];
  if (old)
    vm.page_slot[old - 1] = 0;
  F.slot_page[s] = 0;
  return s;
}

/* An entry that matches nothing: in kseg3, V = 0, one address an entry
   above those of the BIOS's unused entries (InitTLB: entry i at
   0xE0000000 + i * 8 KiB on the PS2, (i - 1) * 8 KiB on PCSX2, up to
   0xE005E000), so never two alike. */
static void tlb_clear(u32 s) { tlb_write(s, 0, 0xE0000000u + ((48 + s) << 13), 0, 0); }

/* With a trace: block b of page pg (mapped) into TLB entry s. Writable
   only once a store to it was seen in this interval, so the first one is
   seen in each (TLB modified). */
static u32 tr_seen(u32 bi) {
  u32 v = vm.tr.seen[bi];
  return (v >> 2) == (vm.tr.epoch & 63) ? v & 3 : 0;
}
static void blk_write(u32 s, u32 pg, u32 b) {
  u32 lo = vm.lo[pg] + (b * BLK >> 12 << 6);
  if (vm.dbit && !(tr_seen(pg * BPP + b) & 2))
    lo &= ~LO_D;
  tlb_write(s, PM4K, (page_va(pg) + b * BLK) | mx__asid, lo, lo + (4096 >> 12 << 6));
}

/* Page pg's entry as lo[] says: written (a new one if it has none) when
   mapped, taken out of the TLB when not. With a trace, the entries of its
   blocks: those in the TLB rewritten, no new ones (blk_fill). Interrupts
   off (or at EXL). */
static void tlb_sync(u32 pg) {
  u32 lo = vm.lo[pg];
  for (u32 b = 0; b < vm.bpp; b++) {
    u32 bi = pg * vm.bpp + b, s = vm.page_slot[bi];
    if (!(lo & LO_V)) {
      if (s) {
        tlb_clear(s);
        F.slot_page[s] = 0;
        vm.page_slot[bi] = 0;
      }
    } else if (vm.bpp > 1) {
      if (s)
        blk_write(s, pg, b);
    } else {
      if (!s) {
        s = tlb_take();
        vm.page_slot[bi] = (u8)s;
        F.slot_page[s] = (u16)(bi + 1);
      }
      tlb_write(s, PM16K, page_va(pg) | mx__asid, lo, lo + (HALF >> 12 << 6));
    }
  }
}

/* ---- the trace ------------------------------------------------------------------- */

static void tr_push(u32 e) {
  if (vm.tr.n < vm.tr.cap)
    vm.tr.buf[vm.tr.n++] = e;
  else if (vm.tr.cap)
    vm.tr.lost++;
}

/* A touch of block bi: recorded once an interval, read or written.
   Interrupts off (or at EXL). */
static void tr_note(u32 bi, int write) {
  u32 had = tr_seen(bi), bit = write ? 2u : 1u;
  if (!(had & bit)) {
    vm.tr.seen[bi] = (u8)((vm.tr.epoch & 63) << 2 | had | bit | 1);
    tr_push((write ? (u32)MX_VM_TR_WRITE : (u32)MX_VM_TR_READ) << 30 | bi);
  }
}

/* A touch of va (page pg, mapped): recorded, then its block into the TLB.
   At EXL. */
static void blk_fill(u32 pg, u32 va, int write) {
  u32 b = (va - page_va(pg)) / BLK, bi = pg * BPP + b;
  tr_note(bi, write);
  u32 s = vm.page_slot[bi];
  if (!s) {
    s = tlb_take();
    vm.page_slot[bi] = (u8)s;
    F.slot_page[s] = (u16)(bi + 1);
  }
  blk_write(s, pg, b);
}

/* At each vblank: once an interval has passed, a tick, and the VM's
   entries out of the TLB, so that each block touched in the next interval
   is a refill. Interrupts off. */
static void tr_tick(void) {
  u32 c = mx_count();
  vm.tr.cyc += c - vm.tr.t_last;
  vm.tr.t_last = c;
  u32 ms = vm.tr.ms;
  while (vm.tr.cyc >= MX_EE_HZ / 1000) {
    vm.tr.cyc -= MX_EE_HZ / 1000;
    ms++;
  }
  vm.tr.ms = ms;
  static u32 last;
  if (ms - last < vm.tr.every_ms)
    return;
  last = ms;
  for (u32 i = 0; i < TLB_N; i++) {
    u32 q = F.slot_page[i];
    if (q) {
      tlb_clear(i);
      vm.page_slot[q - 1] = 0;
      F.slot_page[i] = 0;
    }
  }
  if (!(++vm.tr.epoch & 63))
    __builtin_memset(vm.tr.seen, 0, npages_all() * BPP);
  tr_push((u32)MX_VM_TR_TICK << 30 | (ms & 0x3FFFFFFFu));
}

int mx__irq_vbon_hook(void (*fn)(void)); /* ee/src/irq/irq.c */
void mx__irq_vbon_unhook(void (*fn)(void));

/* At each vblank: the VM's entries out of the TLB, so a page in use
   refills (and gets a new stamp, for evict) at least once a vblank: 24
   refills, some 4000 cycles. With a trace, its tick does that each
   interval instead. Interrupts off. */
static void vblank(void) {
  if (vm.tr.cap) {
    tr_tick();
    return;
  }
  for (u32 i = 0; i < TLB_N; i++) {
    u32 q = F.slot_page[i];
    if (q) {
      tlb_clear(i);
      vm.page_slot[q - 1] = 0;
      F.slot_page[i] = 0;
    }
  }
}

void mx_vm_trace_mark(u32 tag) {
  if (!vm.tr.cap)
    return;
  int was = mx_di();
  tr_push((u32)MX_VM_TR_MARK << 30 | (tag & 0x3FFFFFFFu));
  mx_irq_restore(was);
}

void mx_vm_trace_restart(void) {
  if (!vm.tr.cap)
    return;
  int was = mx_di();
  vm.tr.n = vm.tr.lost = 0;
  mx_irq_restore(was);
}

int mx_vm_trace_save(const char *path) {
  if (!vm.tr.cap)
    return -1;
  int was = mx_di();
  u32 n = vm.tr.n, lost = vm.tr.lost;
  mx_irq_restore(was);
  struct mx_vm_trace_head h = {MX_VM_TRACE_MAGIC, 1, PAGE, BLK, vm.npages, vm.nmap,
                               vm.nframes, vm.tr.every_ms, n, lost};
  int fd = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0)
    return -1;
  int r = mx_fio_write(fd, &h, sizeof h);
  if (!r && n)
    r = mx_fio_write(fd, vm.tr.buf, n * 4); /* the events before n stay as they are */
  mx_fio_close(fd);
  return r ? -1 : (int)n;
}

/* The BIOS's TLB (ps2sdk's InitTLB32MB table; docs/vm.md) as mx_vm_init
   found it, written back by mx__vm_shutdown before the program returns to
   ps2link; and its TLB handlers. */
static struct {
  u32 pm, hi, lo0, lo1;
} bios_tlb[TLB_N];
static void *bios_handler[4];
static u8 tlb_fixed[TLB_N]; /* 1: a BIOS entry kept as it was; 2: one of the VM's fixed ones */
static int tlb_saved;

/* the range of VA an entry maps (both halves) */
static u32 ent_lo(u32 i) {
  u32 pair = (bios_tlb[i].pm | 0x1FFFu) + 1;
  return bios_tlb[i].hi & ~(pair - 1);
}
static u32 ent_hi(u32 i) { return ent_lo(i) + (bios_tlb[i].pm | 0x1FFFu); }
/* the physical memory it maps (EntryLo0's frame, both halves): a program's
   core may be elsewhere than its addresses (mx__core_off) */
static u32 ent_phys(u32 i) { return bios_tlb[i].lo0 >> 6 << 12; }

/* The VM's TLB: the BIOS's entries are read and saved; what the program,
   ps2link and the kernel use stays where it is (the scratchpad, kseg3,
   VU and GS memory, ROM, and the cached main memory that holds the BIOS's
   area, the program, the physical allocator and the stack: their code
   keeps running while this changes them); its 8 pairs of 4 KiB I/O pages
   become one entry (64 KiB halves: the write protection of two pages, the
   timers 2/3 and the SIF registers, goes), its 9 uncached and 8 UCAB
   entries one each (16 MiB halves), its cached main memory that holds only
   frames (phys [flo, fhi)) goes, and one entry maps all of main memory
   cached at MX_PHYS_WIN. Each kind is cleared before its new entry is
   written (two entries matching one address are undefined). Every other
   entry is the VM's, taken in turn (next_of). Interrupts off; nothing in
   between touches the addresses changed. */
static void tlb_init(u32 flo, u32 fhi) {
  for (u32 i = 0; i < TLB_N; i++)
    tlb_read(i, &bios_tlb[i].pm, &bios_tlb[i].hi, &bios_tlb[i].lo0, &bios_tlb[i].lo1);
  tlb_saved = 1;
  int io = -1, unc = -1, ucab = -1;
  for (u32 i = 0; i < TLB_N; i++) {
    u32 lo = ent_lo(i), hi = ent_hi(i), valid = (bios_tlb[i].lo0 | bios_tlb[i].lo1) & LO_V;
    tlb_fixed[i] = 0;
    if (!valid)
      continue; /* unused (kseg3, V = 0) */
    if (lo >= 0x10000000u && hi <= 0x1000FFFFu) {
      tlb_clear(i);
      io = io < 0 ? (int)i : io;
    } else if (lo >= 0x20000000u && hi <= 0x21FFFFFFu) {
      tlb_clear(i);
      unc = unc < 0 ? (int)i : unc;
    } else if (lo >= 0x30000000u && hi <= 0x31FFFFFFu) {
      tlb_clear(i);
      ucab = ucab < 0 ? (int)i : ucab;
    } else if (hi < 0x02000000u && ent_phys(i) >= flo && ent_phys(i) + (hi - lo) < fhi) {
      tlb_clear(i); /* only frames: reached through MX_PHYS_WIN */
    } else
      tlb_fixed[i] = 1;
  }
  /* the slots the BIOS's I/O, uncached and UCAB entries had (or free ones) */
  for (u32 i = 0; i < TLB_N && (io < 0 || unc < 0 || ucab < 0); i++)
    if (!tlb_fixed[i] && (int)i != io && (int)i != unc && (int)i != ucab) {
      if (io < 0)
        io = (int)i;
      else if (unc < 0)
        unc = (int)i;
      else
        ucab = (int)i;
    }
  tlb_write((u32)io, 0x1E000u, 0x10000000u, 0x00400017u, 0x00400417u); /* I/O: uncached, D V G */
  tlb_write((u32)unc, PM16M, 0x20000000u, 0x00000017u, 0x00040017u);   /* uncached */
  tlb_write((u32)ucab, PM16M, 0x30000000u, 0x0000003Fu, 0x0004003Fu);  /* UCAB */
  tlb_fixed[io] = tlb_fixed[unc] = tlb_fixed[ucab] = 2;
  int win = -1;
  for (u32 i = 0; i < TLB_N && win < 0; i++)
    if (!tlb_fixed[i])
      win = (int)i;
  tlb_write((u32)win, PM16M, MX_PHYS_WIN, 0x0000001Fu, 0x0004001Fu); /* cached, D V G */
  tlb_fixed[win] = 2;
  /* the others: the VM's, in a ring */
  int first = -1, prev = -1;
  for (u32 i = 0; i < TLB_N; i++) {
    F.slot_page[i] = 0;
    if (tlb_fixed[i])
      continue;
    tlb_clear(i);
    if (first < 0)
      first = (int)i;
    if (prev >= 0)
      F.next_of[prev] = (u8)i;
    prev = (int)i;
  }
  F.next_of[prev] = (u8)first;
  F.next_slot = (u32)first;
}

u32 mx__vm_tlb_entries(void) {
  u32 n = 0;
  for (u32 i = 0; i < TLB_N; i++)
    n += !tlb_fixed[i];
  return n;
}

/* The BIOS's TLB and handlers back (mx_exit, the crash report): the VM's
   entries and the fixed ones it wrote are cleared first, then the BIOS's
   written; the entries kept never changed, so the code running here stays
   mapped. VM is off after it: nothing of it may be touched. */
static int swap_left = -1; /* the swap file, for mx__vm_swap_close */
void mx__vm_shutdown(void) {
  if (!tlb_saved)
    return;
  swap_left = vm.fd;
  int was = mx_di();
  vm.active = 0;
  F.npages = 0;
  for (u32 i = 0; i < TLB_N; i++) {
    F.slot_page[i] = 0;
    if (tlb_fixed[i] != 1)
      tlb_clear(i);
  }
  for (u32 i = 0; i < TLB_N; i++)
    if (tlb_fixed[i] != 1)
      tlb_write(i, bios_tlb[i].pm, bios_tlb[i].hi, bios_tlb[i].lo0, bios_tlb[i].lo1);
  tlb_saved = 0;
  mx_irq_restore(was);
  for (int c = 1; c <= 3; c++)
    if (bios_handler[c])
      mx_bios_set_tlb_handler(c, (void (*)(void))bios_handler[c]);
}

/* After mx__vm_shutdown, at a normal end: on FAT and exFAT a file that is not
   closed keeps its old size, and its clusters stay taken. */
void mx__vm_swap_close(void) {
  if (swap_left >= 0)
    mx_fio_close(swap_left);
  swap_left = -1;
}

/* ---- pages and frames ------------------------------------------------------- */

/* Map resident page pg; write: the access is a store. Interrupts off. */
static void map(u32 pg, int write) {
  if ((write || !vm.dbit) && !is_mapped(pg)) /* mapped files: never D */
    vm.flags[pg] |= PG_DIRTY;
  u32 f = vm.frame_of[pg] - 1u;
  /* not global (no LO_G): the process's, under its ASID */
  vm.lo[pg] = mx_paddr_of(frame_ptr(f)).v >> 12 << 6 | LO_CACHED | LO_V |
              (vm.flags[pg] & PG_DIRTY ? LO_D : 0);
  vm.flags[pg] |= PG_REF;
  F.stamp[pg] = F.refills;
  tlb_sync(pg);
}

static void unmap(u32 pg) {
  vm.lo[pg] = 0;
  tlb_sync(pg);
  if (vm.tr.seen) /* the trace: touched again, recorded again */
    for (u32 b = 0; b < BPP; b++)
      vm.tr.seen[pg * BPP + b] = 0;
}

/* A free frame, marked taken, or -1. Interrupts off. */
static int frame_take(void) {
  if (!vm.nfree)
    return -1;
  for (u32 n = 0; n < vm.nframes; n++) {
    u32 f = vm.free_hint;
    vm.free_hint = f + 1 < vm.nframes ? f + 1 : 0;
    if (!vm.page_of[f]) {
      vm.nfree--;
      vm.page_of[f] = FRAME_TAKEN;
      return (int)f;
    }
  }
  return -1;
}

/* The page in frame f, or -1 (free or taken). */
static int page_in(u32 f) {
  u32 q = vm.page_of[f];
  return q && q < FRAME_PINNED ? (int)(q - 1) : -1;
}

static void frame_give(u32 pg, u32 f) {
  vm.frame_of[pg] = (u16)(f + 1);
  vm.page_of[f] = (u16)(pg + 1);
}

static void zero_frame(u32 f) {
  u64 *p = (u64 *)frame_ptr(f);
  for (u32 i = 0; i < PAGE / 8; i += 8) {
    p[i] = p[i + 1] = p[i + 2] = p[i + 3] = 0;
    p[i + 4] = p[i + 5] = p[i + 6] = p[i + 7] = 0;
  }
}

/* Frame `to` = frame `from` (-1: zero), from thread code. Both frames are
   the caller's (taken, or holding a busy page), so nothing else touches
   them and interrupts stay on. */
static void copy_frame(u32 to, int from) {
  if (from < 0)
    zero_frame(to);
  else
    __builtin_memcpy(frame_ptr(to), frame_ptr((u32)from), PAGE);
  code_sync();
}

/* ---- the exception ------------------------------------------------------------ */

static int to_thread(struct mx__vm_x *x);

/* Tests (tests/ps2/vm4_exc.c): a break instruction goes to the thread as a
   page fault does, which calls the test's function with the break code
   and goes on after the break. All of the slow path but the TLB. */
static void (*break_fn)(u32 code);
void mx__vm_break_init(void (*fn)(u32 code)) {
  break_fn = fn;
  mx_bios_set_syscall(VM_RESUME_SYSCALL, (void *)mx__vm_resume_k);
  mx_bios_set_common_handler(9, mx__vm_vector);
}

int mx__vm_exception(struct mx__vm_x *x) {
  u32 code = x->cause >> 2 & 31, va = x->badvaddr;
  if (code == 9 && break_fn && !(x->cause >> 31))
    return to_thread(x);
  u32 pg = va_page(va);
  if (code < 1 || code > 3 || pg == ~0u) {
      mx__exception_note = va - MX_VM_BASE < MX_VM_LIMIT - MX_VM_BASE
                             ? "VM: outside the VM in use (mx_vm_size)"
                         : va - MX_VM_MAP_BASE < MX_VM_MAP_LIMIT - MX_VM_MAP_BASE
                             ? "VM: in the file window, but not in a mapped file"
                         : va - MX_VM_HEAP_BASE < MX_VM_HEAP_LIMIT - MX_VM_HEAP_BASE
                             ? "VM: in the heap window, but not in a heap (mx_heap_create)"
                             : 0;
    return VMX_FATAL;
  }
  if (code == 1) { /* a store to a clean page */
    vm.modified_seen++;
    if (is_mapped(pg)) {
      mx__exception_note = "VM: a store to a mapped file (mx_vm_map_file: read-only)";
      return VMX_FATAL;
    }
    if (!vm.frame_of[pg] || (vm.flags[pg] & PG_BUSY)) {
      mx__exception_note = "VM: TLB modified on a page that is not resident";
      return VMX_FATAL;
    }
    if (vm.bpp > 1)
      blk_fill(pg, va, 1);
    vm.flags[pg] |= PG_DIRTY;
    vm.lo[pg] |= LO_D;
    F.stamp[pg] = F.refills;
    tlb_sync(pg);
    return VMX_RETURN;
  }
  F.refills++;
  F.stamp[pg] = F.refills;
  if (x->badvaddr == x->epc + (x->cause >> 31 ? 4u : 0u)) /* an instruction fetch */
    F.refills_i++;
  if (vm.lo[pg] & LO_V) { /* mapped, only not in the TLB */
    if (vm.bpp > 1)
      blk_fill(pg, va, code == 3);
    else
      tlb_sync(pg);
    return VMX_RETURN;
  }
  if (vm.frame_of[pg] && !(vm.flags[pg] & PG_BUSY)) { /* the clock took V away */
    map(pg, code == 3);
    if (vm.bpp > 1)
      blk_fill(pg, va, code == 3);
    return VMX_RETURN;
  }
  /* a new page or one in the swap file: in the thread */
  return to_thread(x);
}

/* Continue in mx__vm_slow on the faulting thread's stack, if it can wait
   there. */
static int to_thread(struct mx__vm_x *x) {
  u32 st = x->status, sp = (u32)x->gpr[29][0];
  if (!(st & 1) || !(st & (1u << 16))) {
    mx__exception_note = "VM: a page fault on a new page or one in the swap file with "
                         "interrupts off (an interrupt handler, mx_di); touch VM only with "
                         "interrupts on";
    return VMX_FATAL;
  }
  if (x->epc >= 0x80000000u) {
    mx__exception_note = "VM: a page fault in the BIOS (a VM pointer passed to a syscall)";
    return VMX_FATAL;
  }
  if (mx_is_vm((void *)(uintptr_t)sp) || sp < 0x00100000u + VMX_SIZE) {
    mx__exception_note = "VM: a page fault on a thread whose stack is VM (stacks must be core)";
    return VMX_FATAL;
  }
  struct mx__vm_x *f = (struct mx__vm_x *)((sp - VMX_SIZE - 64) & ~63u);
  *f = *x;
  x->frame = f;
  return VMX_SLOW;
}

/* ---- the swap file ------------------------------------------------------------- */

static void vm_fatal(const char *what, u32 pg) __attribute__((noreturn));
static void vm_fatal(const char *what, u32 pg) {
  mx_printf("\nVM: %s (page %u at %p)\n", what, pg, (void *)(uintptr_t)page_va(pg));
  MX_TEST_END(255);
  mx_exit(255);
}

/* Debugging (the debug screens of tests/ps2/vm7_swap.c, vm8_dma.c): the
   last swap operations, op << 31 | slot << 16 | frame << 4 | step (1
   sought, 2 done). */
volatile u32 mx__vm_swaplog[8], mx__vm_swapn;

/* Thread code of the VM runs with interrupts on (file I/O waits for the
   IOP's interrupts). The first place it finds them off is kept here,
   where << 24 | Status (low 24 bits), and shown in slot 28 of the debug
   screen (<mx/debug.h>, if a test shows it): the PS2 once hung with them
   off after a swap write in a gather. Places: 0x10-0x12 swap_io (in, after
   the seek, after the transfer), 0x20 vacate, 0x21-0x22 gather (after
   vacate, after a copy), 0x30-0x31 the slow path (in, out), 0x40-0x41
   mx_vm_pin (in, after the gather). */
volatile u32 mx__vm_irq_lost;
static void irq_check(u32 where) {
  u32 st = mx_status();
  if ((st & 1) && (st >> 16 & 1))
    return;
  if (!mx__vm_irq_lost) {
    mx__vm_irq_lost = where << 24 | (st & 0xFFFFFFu);
    mx_dbg_word(28, mx__vm_irq_lost);
  }
}

extern volatile u32 mx__fio_calls;

/* time and file server calls of the pager's I/O, into *us and io_calls
   (the calls of another thread meanwhile count too) */
struct io_mark {
  u32 t0, c0;
};
static struct io_mark io_begin(void) { return (struct io_mark){mx_count(), mx__fio_calls}; }
static void io_end(struct io_mark m, u32 *us) {
  *us += (mx_count() - m.t0) / (MX_EE_HZ / 1000000u);
  vm.st.io_calls += mx__fio_calls - m.c0;
}

/* The swap file's seek and transfer go together: the writer thread and a
   faulting thread may both use it (each call of the file server is one at
   a time, a seek and the transfer after it are not). */
static void swap_io1(u32 pg, u32 f, int out);
static void swap_io(u32 pg, u32 f, int out) {
  struct io_mark m = io_begin();
  mx_bios_wait_sema(vm.swapsema);
  swap_io1(pg, f, out);
  mx_bios_signal_sema(vm.swapsema);
  io_end(m, out ? &vm.st.us_swap_out : &vm.st.us_swap_in);
}

static void swap_io1(u32 pg, u32 f, int out) {
  irq_check(0x10);
  u32 slot = vm.slot_of[pg] - 1u;
  volatile u32 *lg = &mx__vm_swaplog[mx__vm_swapn++ & 7];
  *lg = (u32)out << 31 | slot << 16 | f << 4;
  /* again if the file server does not answer (it can drop what it sends
     under load, <mx/sif.h>): a seek and a transfer at the slot, so a repeat
     does no harm */
  int at = -1, r = -1;
  for (int tries = 0; tries < 4; tries++) {
    if (tries)
      vm.st.retries++;
    at = mx_fio_lseek(vm.fd, (s32)(slot * PAGE), MX_FIO_SEEK_SET);
    *lg |= 1;
    irq_check(0x11);
    if (at != (int)(slot * PAGE))
      continue;
    r = out ? mx_fio_write(vm.fd, frame_ptr(f), PAGE) : mx_fio_read(vm.fd, frame_ptr(f), PAGE);
    *lg |= 2;
    irq_check(0x12);
    if (r == (out ? 0 : (int)PAGE))
      return;
  }
  mx_printf("\nVM: swap file %s of slot %u (of %u): seek %d, %s %d; the file has %d bytes\n",
            out ? "write" : "read", slot, vm.nslots, at, out ? "write" : "read", r,
            mx_fio_lseek(vm.fd, 0, MX_FIO_SEEK_END));
  vm_fatal(out ? "swap file write failed" : "swap file read failed", pg);
}

/* Page pg (PG_FILE) into frame f from its file (the program's ELF, a
   mapped file): the file's bytes of the page, the rest zero. */
static void file_in1(u32 pg, u32 f);
static void file_in(u32 pg, u32 f) {
  struct io_mark m = io_begin();
  file_in1(pg, f);
  io_end(m, &vm.st.us_file_in);
}

static void file_in1(u32 pg, u32 f) {
  irq_check(0x18);
  u32 i = 0;
  while (i < vm.nseg && !(vm.seg[i].fd >= 0 && pg - vm.seg[i].pg < vm.seg[i].npg))
    i++;
  if (i == vm.nseg)
    vm_fatal("a file page outside the files", pg);
  u32 off = (pg - vm.seg[i].pg) * PAGE;
  u32 at = vm.seg[i].offset + off, n = vm.seg[i].filesz - off;
  if (n > PAGE)
    n = PAGE;
  int fd = vm.seg[i].fd;
  int got = -1, sought = -1;
  for (int tries = 0; tries < 4 && got != (int)n; tries++) {
    if (tries)
      vm.st.retries++;
    sought = mx_fio_lseek(fd, (s32)at, MX_FIO_SEEK_SET);
    if (sought == (int)at)
      got = mx_fio_read(fd, frame_ptr(f), n);
  }
  if (got != (int)n) {
    mx_printf("\nVM: a file at %u: seek %d, read %d of %u\n", at, sought, got, n);
    vm_fatal("reading a file page failed", pg);
  }
  if (n < PAGE)
    __builtin_memset(frame_ptr(f) + n, 0, PAGE - n);
}

/* Pages [pg, pg + (filesz in pages)) come from fd at offset. Returns the
   entry, or -1. The lock is held (or VM is starting). */
static int file_pages(int fd, u32 pg, u32 filesz, u32 offset) {
  u32 i = 0; /* a free entry (unmapped, its pages given back) */
  while (i < vm.nseg && (vm.seg[i].fd >= 0 || vm.seg[i].npg))
    i++;
  if (i == NSEG)
    return -1;
  u32 npg = (filesz + PAGE - 1) / PAGE;
  vm.seg[i].pg = pg;
  vm.seg[i].npg = npg;
  vm.seg[i].filesz = filesz;
  vm.seg[i].offset = offset;
  vm.seg[i].fd = fd;
  if (i == vm.nseg)
    vm.nseg++;
  int was = mx_di();
  for (u32 p = pg; p < pg + npg; p++)
    vm.flags[p] |= PG_FILE;
  mx_irq_restore(was);
  return (int)i;
}

/* ee/src/vm/image.c: pages [va, va + filesz) come from the ELF open as fd, at
   offset; va on a page. The rest of the segment is new pages (zero). */
int mx__vm_file_pages(int fd, u32 va, u32 filesz, u32 offset) {
  if ((va & (PAGE - 1)) || va < MX_VM_BASE || va - MX_VM_BASE + filesz > vm.npages * PAGE)
    return -1;
  return file_pages(fd, (va - MX_VM_BASE) >> PAGE_SHIFT, filesz, offset) < 0 ? -1 : 0;
}

u32 mx_vm_unpin_done(void);

/* Write page pg (busy, unmapped) out of frame f if it is dirty. */
static void write_out(u32 pg, u32 f) {
  if (!(vm.flags[pg] & PG_DIRTY)) {
    vm.st.drops++;
    return;
  }
  if (vm.fd < 0)
    vm_fatal("out of memory: all frames in use and no swap file", pg);
  if (!vm.slot_of[pg])
    vm.slot_of[pg] = (u16)++vm.nslots;
  swap_io(pg, f, 1);
  vm.st.swap_outs++;
  vm.flags[pg] |= PG_SWAP;
}

/* ---- the writer thread (mx_vm_config.clean_frames) ------------------------ */

static void lock(void);
static void unlock(void);

/* The pages eviction takes next: the up to k (16 at most) resident, not
   pinned, not busy ones used longest ago (by their stamps: the last
   refill), oldest first, into out. Returns how many. Interrupts off. */
static u32 oldest(u32 k, u32 *out) {
  u32 n = 0, age[16];
  if (k > 16)
    k = 16;
  if (!k)
    return 0;
  for (u32 f = 0; f < vm.nframes; f++) {
    int q = page_in(f);
    if (q < 0 || vm.pins[q] || (vm.flags[q] & PG_BUSY))
      continue;
    u32 a = F.refills - F.stamp[q], i = n < k ? n++ : k;
    if (i == k && a <= age[k - 1])
      continue;
    if (i == k)
      i--;
    for (; i && age[i - 1] < a; i--)
      age[i] = age[i - 1], out[i] = out[i - 1];
    age[i] = a, out[i] = (u32)q;
  }
  return n;
}

/* A dirty page to write ahead: of the pages eviction takes next (as many
   as clean_frames, less the free frames), the oldest dirty one; -1 if
   none. The lock is held. */
static int dirty_candidate(void) {
  int was = mx_di();
  u32 k = vm.nfree < vm.clean_frames ? vm.clean_frames - vm.nfree : 0, pg[16];
  u32 n = oldest(k, pg);
  int q = -1;
  for (u32 i = 0; i < n && q < 0; i++)
    if (vm.flags[pg[i]] & PG_DIRTY)
      q = (int)pg[i];
  mx_irq_restore(was);
  return q;
}

static u8 wstack[8192] MX_ALIGNED(16);

static void writer(void *arg) {
  (void)arg;
  for (;;) {
    mx_bios_wait_sema(vm.wsema);
    lock();
    for (;;) {
      int q = dirty_candidate();
      if (q < 0)
        break;
      u32 pg = (u32)q, f = vm.frame_of[pg] - 1u;
      /* pinned for the write (not evicted, not moved); clean from now: a
         store meanwhile makes it dirty again (TLB modified; on PCSX2, which
         ignores D, it is unmapped and dirty when mapped again) */
      int was = mx_di();
      vm.pins[pg]++;
      vm.flags[pg] &= (u8)~PG_DIRTY;
      if (vm.dbit) {
        vm.lo[pg] &= ~LO_D;
        tlb_sync(pg);
      } else
        unmap(pg);
      mx_irq_restore(was);
      if (!vm.slot_of[pg])
        vm.slot_of[pg] = (u16)++vm.nslots;
      unlock();
      swap_io(pg, f, 1);
      lock();
      was = mx_di();
      vm.pins[pg]--;
      vm.flags[pg] |= PG_SWAP;
      mx_irq_restore(was);
      vm.st.writer_outs++;
    }
    unlock();
  }
}

static void writer_start(u32 clean) {
  int me = mx_thread_self(), prio = mx_thread_prio(me);
  if (prio < 1) {
    mx_printf("mx_vm_init: clean_frames: the thread is at priority %d, the writer cannot run "
              "above it; no writer\n",
              prio);
    return;
  }
  struct {
    s32 count, max_count, init_count, wait_threads;
    u32 attr, option;
  } sp = {0, 1, 0, 0, 0, 0};
  vm.wsema = mx_bios_create_sema(&sp);
  vm.clean_frames = clean < vm.nframes / 2 ? clean : vm.nframes / 2;
  int id = vm.wsema < 0 ? -1 : mx_thread_start(writer, 0, wstack, sizeof wstack, prio - 1);
  if (id < 0) {
    mx_printf("mx_vm_init: clean_frames: the writer thread did not start\n");
    vm.wsema = -1;
  } else
    mx__thread_keep(id);
}

/* Take a frame: a free one, or the one of the page used longest ago (LRU
   by the stamps of its refills: a refill is a use, and each vblank the
   VM's entries leave the TLB (vblank), so a page in use refills at least
   that often), written out if dirty. Returns the frame, marked taken. The
   lock is held. Clean pages are not preferred: a program's code in VM is
   clean (read from its ELF) and among the hottest; with clean ones taken
   first, Quake III's loading went to frames of 10 s on the PS2
   (2026-11-01). In Quake III's trace (tools/vmsim.py) LRU paged a third of
   what the clock paged that came before. */
static u32 evict(void) {
  mx_vm_unpin_done();
  if (vm.wsema >= 0)
    mx_bios_signal_sema(vm.wsema); /* a frame goes: the writer cleans the next ones */
  int was = mx_di();
  int f = frame_take(); /* freed meanwhile */
  if (f >= 0) {
    mx_irq_restore(was);
    return (u32)f;
  }
  u32 pg;
  if (!oldest(1, &pg)) {
    mx_irq_restore(was);
    vm_fatal("out of memory: every frame is pinned", 0);
  }
  u32 fr = vm.frame_of[pg] - 1u;
  unmap(pg);
  vm.flags[pg] |= PG_BUSY;
  mx_irq_restore(was);
  write_out(pg, fr);
  was = mx_di();
  vm.flags[pg] &= (u8)~(PG_BUSY | PG_DIRTY | PG_REF);
  vm.frame_of[pg] = 0;
  vm.page_of[fr] = FRAME_TAKEN;
  mx_irq_restore(was);
  return fr;
}

/* Make page pg resident at frame f (a free frame, taken), then map it. The
   lock is held. */
static void place(u32 pg, u32 f, int write) {
  int was = mx_di();
  vm.flags[pg] |= PG_BUSY; /* another thread's fault waits */
  mx_irq_restore(was);
  if (vm.flags[pg] & PG_SWAP) {
    swap_io(pg, f, 0);
    code_sync();
    vm.st.swap_ins++;
  } else if (vm.flags[pg] & PG_FILE) {
    file_in(pg, f);
    code_sync();
    vm.st.file_ins++;
  } else {
    copy_frame(f, -1);
    vm.st.zero_fills++;
  }
  was = mx_di();
  vm.flags[pg] &= (u8)~PG_BUSY;
  frame_give(pg, f);
  map(pg, write);
  mx_irq_restore(was);
}

/* Page pg resident and mapped. The lock is held. */
static void fault_in(u32 pg, int write) {
  int was = mx_di();
  if (vm.frame_of[pg]) {
    map(pg, write);
    mx_irq_restore(was);
    return;
  }
  int f = frame_take();
  mx_irq_restore(was);
  place(pg, f >= 0 ? (u32)f : evict(), write);
}

static void lock(void) {
  u32 t0 = mx_count();
  mx_bios_wait_sema(vm.lock);
  vm.st.us_lock += (mx_count() - t0) / (MX_EE_HZ / 1000000u);
}
static void unlock(void) { mx_bios_signal_sema(vm.lock); }

/* Debugging (the same): where the slow path is: 1 entered, 2 has the
   lock, 3 done, 0 resumed. */
volatile u32 mx__vm_where;
/* Debugging: the last page fault handled in the thread: the instruction
   and the address. */
volatile u32 mx__vm_fault_epc, mx__vm_fault_va;
void mx__vm_slow(struct mx__vm_x *f) {
  if ((f->cause >> 2 & 31) == 9) {
    u32 insn = *(const u32 *)(uintptr_t)f->epc;
    f->epc += 4;
    break_fn(insn >> 6 & 0xFFFFF);
    mx__vm_resume(f);
  }
  u32 pg = va_page(f->badvaddr);
  mx__vm_fault_epc = f->epc, mx__vm_fault_va = f->badvaddr;
  mx__vm_where = 1;
  irq_check(0x30);
  lock();
  mx__vm_where = 2;
  vm.st.faults++;
  fault_in(pg, (f->cause >> 2 & 31) == 3);
  unlock();
  mx__vm_where = 3;
  irq_check(0x31);
  mx__vm_resume(f);
}

/* ---- pins and pieces ------------------------------------------------------------- */

static u32 first_page(const void *p) { return va_page((u32)(uintptr_t)p); }
static u32 last_page(const void *p, u32 bytes) {
  return va_page((u32)(uintptr_t)p + (bytes ? bytes : 1) - 1);
}

/* [p, p + bytes) in one window in use */
static int in_vm(const void *p, u32 bytes) {
  u32 a = first_page(p), b = last_page(p, bytes);
  return vm.active && a != ~0u && b != ~0u && b >= a && is_mapped(a) == is_mapped(b) &&
         (uintptr_t)p + bytes >= (uintptr_t)p;
}

/* Frames [f0, f0 + n) hold pages a.. in order? */
static int contiguous(u32 a, u32 n) {
  u32 f0 = vm.frame_of[a];
  for (u32 i = 0; i < n; i++)
    if (!f0 || vm.frame_of[a + i] != f0 + i)
      return 0;
  return 1;
}

/* Move the page in frame fr (not pinned) elsewhere: to a free frame
   outside [lo, hi), or to the swap file; fr is then free. The lock is
   held. */
static void vacate(u32 fr, u32 lo, u32 hi) {
  u32 q = (u32)page_in(fr);
  int was = mx_di();
  int to = -1;
  for (u32 f = 0; f < vm.nframes && to < 0; f++)
    if (!vm.page_of[f] && (f < lo || f >= hi))
      to = (int)f;
  unmap(q);
  vm.flags[q] |= PG_BUSY;
  if (to >= 0) {
    vm.nfree--;
    vm.page_of[to] = FRAME_TAKEN;
  }
  mx_irq_restore(was);
  if (to >= 0)
    copy_frame((u32)to, (int)fr);
  else
    write_out(q, fr);
  irq_check(0x20);
  was = mx_di();
  vm.flags[q] &= (u8)~PG_BUSY;
  vm.page_of[fr] = 0;
  vm.nfree++;
  if (to >= 0) {
    frame_give(q, (u32)to);
    map(q, 0);
  } else {
    vm.flags[q] &= (u8)~(PG_DIRTY | PG_REF);
    vm.frame_of[q] = 0;
  }
  mx_irq_restore(was);
}

/* Pages a..a+n-1 into frames [f0, f0 + n) in order. The lock is held;
   none of the frames holds a pinned page outside the range (nor, then,
   inside it: pinned pages of the range are where they belong or the range
   was not pinned). */
static void gather(u32 a, u32 n, u32 f0) {
  for (u32 i = 0; i < n; i++) {
    u32 fr = f0 + i, pg = a + i;
    if (vm.frame_of[pg] == fr + 1)
      continue;
    if (page_in(fr) >= 0)
      vacate(fr, f0, f0 + n);
    irq_check(0x21);
    int was = mx_di();
    vm.page_of[fr] = FRAME_TAKEN;
    vm.nfree--;
    if (vm.frame_of[pg]) { /* resident elsewhere: copy it here */
      u32 from = vm.frame_of[pg] - 1u;
      unmap(pg);
      vm.flags[pg] |= PG_BUSY;
      mx_irq_restore(was);
      copy_frame(fr, (int)from);
      irq_check(0x22);
      was = mx_di();
      vm.flags[pg] &= (u8)~PG_BUSY;
      vm.page_of[from] = 0;
      vm.nfree++;
      frame_give(pg, fr);
      map(pg, 0);
      mx_irq_restore(was);
    } else {
      mx_irq_restore(was);
      place(pg, fr, 0);
    }
  }
}

/* A run of n frames that holds no pinned page outside [a, a + n). */
static int find_run(u32 a, u32 n) {
  for (u32 f0 = 0; f0 + n <= vm.nframes; f0++) {
    u32 i = 0;
    for (; i < n; i++) {
      int q = page_in(f0 + i);
      if (vm.page_of[f0 + i] >= FRAME_PINNED ||
          (q >= 0 && vm.pins[q] && ((u32)q < a || (u32)q >= a + n)))
        break;
    }
    if (i == n)
      return (int)f0;
    f0 += i; /* past the pinned one */
  }
  return -1;
}

int mx_vm_pin(const void *p, u32 bytes, u32 flags) {
  if (!mx_is_vm(p))
    return 0;
  if (!in_vm(p, bytes))
    return -1;
  u32 a = first_page(p), b = last_page(p, bytes), n = b - a + 1;
  if ((flags & MX_VM_PIN_WRITE) && is_mapped(a))
    return -1; /* mapped files are read-only */
  irq_check(0x40);
  lock();
  if ((flags & MX_VM_PIN_DMA) && !contiguous(a, n)) {
    int f0 = -1;
    /* pinned pages may be in use by a transfer: they are not moved */
    for (u32 pg = a; pg <= b && f0 == -1; pg++)
      if (vm.pins[pg])
        f0 = -2;
    if (f0 == -1)
      f0 = find_run(a, n);
    if (f0 < 0) {
      unlock();
      return -1;
    }
    gather(a, n, (u32)f0);
    irq_check(0x41);
  }
  u32 lo = (u32)(uintptr_t)p, hi = lo + bytes;
  for (u32 pg = a; pg <= b; pg++) {
    u32 va = page_va(pg);
    if ((flags & MX_VM_PIN_OVERWRITE) && va >= lo && va + PAGE <= hi && !vm.frame_of[pg]) {
      /* overwritten whole: a frame, nothing read into it */
      int was = mx_di();
      int f = frame_take();
      mx_irq_restore(was);
      f = f >= 0 ? f : (int)evict();
      was = mx_di();
      frame_give(pg, (u32)f);
      vm.flags[pg] |= PG_RAW;
      map(pg, 1);
      mx_irq_restore(was);
      vm.st.overwrites++;
    } else
      fault_in(pg, (flags & MX_VM_PIN_WRITE) != 0);
    if (vm.pins[pg] == 255)
      vm_fatal("mx_vm_pin: pinned 255 times", pg);
    int was = mx_di();
    if (!vm.pins[pg]++)
      vm.st.pinned++;
    if (flags & MX_VM_PIN_WRITE) {
      vm.flags[pg] |= PG_DIRTY;
      map(pg, 1);
    }
    if (vm.tr.seen) /* the trace: the blocks the DMAC or the IOP touches */
      for (u32 b = 0; b < BPP; b++)
        if (va + (b + 1) * BLK > lo && va + b * BLK < hi)
          tr_note(pg * BPP + b, (flags & MX_VM_PIN_WRITE) != 0);
    mx_irq_restore(was);
  }
  unlock();
  return 0;
}

/* After a transfer into [p, p + bytes) pinned with MX_VM_PIN_OVERWRITE
   that wrote only its first done bytes: the rest of the pages not filled
   gets their contents back (from the swap file or the file; zero for a new
   page), through a frame taken for the time. */
void mx_vm_overwrite_done(const void *p, u32 bytes, u32 done) {
  if (!mx_is_vm(p) || !in_vm(p, bytes))
    return;
  u32 end = (u32)(uintptr_t)p + done;
  lock();
  for (u32 pg = first_page(p), b = last_page(p, bytes); pg <= b; pg++) {
    if (!(vm.flags[pg] & PG_RAW))
      continue;
    u32 va = page_va(pg), from = end > va ? end - va : 0;
    u8 *d = frame_ptr(vm.frame_of[pg] - 1u);
    if (from < PAGE) {
      if (vm.flags[pg] & (PG_SWAP | PG_FILE)) {
        int was = mx_di();
        int t = frame_take();
        mx_irq_restore(was);
        u32 tf = t >= 0 ? (u32)t : evict();
        if (vm.flags[pg] & PG_SWAP)
          swap_io(pg, tf, 0);
        else
          file_in(pg, tf);
        __builtin_memcpy(d + from, frame_ptr(tf) + from, PAGE - from);
        was = mx_di();
        vm.page_of[tf] = 0;
        vm.nfree++;
        mx_irq_restore(was);
        vm.st.restores++;
      } else
        __builtin_memset(d + from, 0, PAGE - from);
      code_sync();
    }
    int was = mx_di();
    vm.flags[pg] &= (u8)~PG_RAW;
    mx_irq_restore(was);
  }
  unlock();
}

void mx_vm_unpin(const void *p, u32 bytes) {
  if (!mx_is_vm(p) || !in_vm(p, bytes))
    return;
  int was = mx_di();
  for (u32 pg = first_page(p), b = last_page(p, bytes); pg <= b; pg++)
    if (vm.pins[pg] && !--vm.pins[pg]) {
      vm.st.pinned--;
      vm.flags[pg] &= (u8)~PG_RAW; /* overwritten (or not: then its contents are lost) */
    }
  mx_irq_restore(was);
}

/* Pins to drop once their DMA has passed (mx_vm_unpin_after). */
#define NLATER 32
static struct {
  const void *p;
  u32 bytes;
  mx_event e;
} later[NLATER];
static u32 nlater;

u32 mx_vm_unpin_done(void) {
  int was = mx_di();
  for (u32 i = 0; i < nlater;) {
    if (mx_event_done(later[i].e)) {
      mx_vm_unpin(later[i].p, later[i].bytes);
      later[i] = later[--nlater];
    } else
      i++;
  }
  u32 n = nlater;
  mx_irq_restore(was);
  return n;
}

void mx_vm_unpin_after(const void *p, u32 bytes, mx_event e) {
  if (!mx_is_vm(p))
    return;
  while (mx_vm_unpin_done() == NLATER)
    ; /* full: the oldest transfers end first */
  int was = mx_di();
  later[nlater].p = p, later[nlater].bytes = bytes, later[nlater].e = e;
  nlater++;
  mx_irq_restore(was);
}

static mx_paddr page_paddr(u32 pg) {
  return mx_paddr_of(frame_ptr(vm.frame_of[pg] - 1u));
}

static void pieces_fault(const void *p, u32 bytes, const char *what) __attribute__((noreturn));
static void pieces_fault(const void *p, u32 bytes, const char *what) {
  mx_printf("\nDMA: %u bytes at %p: %s\n", bytes, p, what);
  mx__paddr_fault(p, bytes);
}

u32 mx_vm_pieces(const void *p, u32 bytes, struct mx_vm_piece *pc, u32 max) {
  if (!mx_is_vm(p)) {
    if (!max)
      pieces_fault(p, bytes, "no room for its pieces");
    pc[0].addr = mx_paddr_of(p);
    pc[0].bytes = bytes;
    return 1;
  }
  if (!in_vm(p, bytes))
    pieces_fault(p, bytes, "VM not in use there");
  u32 n = 0, off = (uintptr_t)p & (PAGE - 1);
  for (u32 pg = first_page(p), left = bytes; left;) {
    if (!vm.pins[pg] || !vm.frame_of[pg])
      pieces_fault(p, bytes, "VM not pinned (mx_vm_pin)");
    u32 k = PAGE - off < left ? PAGE - off : left;
    mx_paddr a = mx_paddr_add(page_paddr(pg), off);
    if (n && pc[n - 1].addr.v + pc[n - 1].bytes == a.v)
      pc[n - 1].bytes += k;
    else {
      if (n == max)
        pieces_fault(p, bytes, "in more pieces than the transfer takes (MX_VM_PIN_DMA)");
      pc[n].addr = a;
      pc[n++].bytes = k;
    }
    left -= k, off = 0, pg++;
  }
  return n;
}

struct mx_vm_piece mx_vm_first_piece(const void *p, u32 bytes) {
  struct mx_vm_piece pc = {mx_paddr_of(0), bytes};
  if (!mx_is_vm(p)) {
    pc.addr = mx_paddr_of(p);
    return pc;
  }
  if (!in_vm(p, bytes))
    pieces_fault(p, bytes, "VM not in use there");
  u32 off = (uintptr_t)p & (PAGE - 1), got = 0;
  for (u32 pg = first_page(p); got < bytes; pg++, off = 0) {
    if (!vm.pins[pg] || !vm.frame_of[pg])
      pieces_fault(p, bytes, "VM not pinned (mx_vm_pin)");
    mx_paddr a = mx_paddr_add(page_paddr(pg), off);
    if (got && pc.addr.v + got != a.v)
      break;
    if (!got)
      pc.addr = a;
    got += PAGE - off < bytes - got ? PAGE - off : bytes - got;
  }
  pc.bytes = got;
  return pc;
}

mx_paddr mx__vm_paddr(const void *p, u32 bytes) {
  struct mx_vm_piece pc = mx_vm_first_piece(p, bytes);
  if (pc.bytes != bytes)
    pieces_fault(p, bytes, "VM in more than one physical piece (pin it with MX_VM_PIN_DMA)");
  return pc.addr;
}

void mx__paddr_fault(const void *p, u32 bytes) {
  mx_printf("\nDMA: %p (%u bytes) is not core memory: DMA tags, chains, rings and their "
            "addresses must be core or pinned (static data, mx_heap_alloc of MX_MEM_PINNED); VM data "
            "only through mx_vm_pin (<mx/dma.h>)\n",
            p, bytes);
  MX_TEST_END(255);
  mx_exit(255);
}

/* ---- pinned memory, heaps of pages (mem.c) ------------------------------------------ */

/* A run of n frames, from the top down, of free frames and pages that may
   move (not pinned, not busy): pinned memory gathers at the top. */
static int pin_run(u32 n) {
  for (int f0 = (int)vm.nframes - (int)n; f0 >= 0; f0--) {
    u32 i = 0;
    for (; i < n; i++) {
      u32 f = (u32)f0 + i;
      if (vm.page_of[f] >= FRAME_PINNED)
        break;
      int q = page_in(f);
      if (q >= 0 && (vm.pins[q] || (vm.flags[q] & PG_BUSY)))
        break;
    }
    if (i == n)
      return f0;
    f0 = f0 + (int)i - (int)n + 1; /* below the one in the way, after the f0-- */
  }
  return -1;
}

/* bytes (whole frames) of pinned memory: a run of frames out of the pool,
   the pages in it moved out (to free frames, or the swap file). Its address
   in MX_PHYS_WIN, or 0. Thread code with interrupts on. */
void *mx__vm_frames_take(u32 bytes) {
  u32 n = (bytes + PAGE - 1) / PAGE;
  if (!vm.active || !n || n > vm.nframes)
    return 0;
  irq_check(0x50);
  lock();
  int f0 = pin_run(n);
  if (f0 < 0) {
    unlock();
    return 0;
  }
  for (u32 i = 0; i < n; i++) {
    u32 fr = (u32)f0 + i;
    if (page_in(fr) >= 0)
      vacate(fr, (u32)f0, (u32)f0 + n);
    int was = mx_di();
    vm.page_of[fr] = FRAME_PINNED;
    vm.nfree--;
    mx_irq_restore(was);
  }
  vm.pinned_frames += n;
  unlock();
  return frame_ptr((u32)f0);
}

void mx__vm_frames_give(void *p, u32 bytes) {
  u32 n = (bytes + PAGE - 1) / PAGE, f0 = (u32)((u8 *)p - vm.frames) / PAGE;
  int was = mx_di();
  for (u32 i = 0; i < n; i++)
    if (vm.page_of[f0 + i] == FRAME_PINNED) {
      vm.page_of[f0 + i] = 0;
      vm.nfree++;
      vm.pinned_frames--;
    }
  mx_irq_restore(was);
}

/* bytes (whole pages) of the heap window for a heap of pages: its
   address, or 0 */
u32 mx__vm_va_take(u32 bytes) {
  u32 n = (bytes + PAGE - 1) / PAGE;
  int was = mx_di();
  u32 r = 0;
  if (vm.active && vm.heap_next + n <= vm.nheap) {
    r = MX_VM_HEAP_BASE + vm.heap_next * PAGE;
    vm.heap_next += n;
  }
  mx_irq_restore(was);
  return r;
}

/* [p, p + bytes) all resident (no fault to touch or lock it) */
int mx__vm_resident(const void *p, u32 bytes) {
  if (!mx_is_vm(p))
    return 1;
  if (!in_vm(p, bytes))
    return 0;
  int was = mx_di();
  int r = 1;
  for (u32 pg = first_page(p), b = last_page(p, bytes); pg <= b && r; pg++)
    r = vm.frame_of[pg] && !(vm.flags[pg] & PG_BUSY);
  mx_irq_restore(was);
  return r;
}

/* the bytes of [p, p + bytes) in resident pages */
u32 mx__vm_resident_bytes(const void *p, u32 bytes) {
  if (!mx_is_vm(p))
    return bytes;
  if (!in_vm(p, bytes))
    return 0;
  int was = mx_di();
  u32 n = 0;
  for (u32 pg = first_page(p), b = last_page(p, bytes); pg <= b; pg++)
    n += vm.frame_of[pg] ? PAGE : 0;
  mx_irq_restore(was);
  return n;
}

/* The pager thread: brings the ranges asked for in (mx__vm_prefetch), at
   the writer's priority (it sleeps in its I/O). */
#define NPREF 64
static struct {
  const void *p;
  u32 bytes;
} pref[NPREF];
static volatile u32 pref_in, pref_out;
static int psema = -1;
static u8 pstack[8192] MX_ALIGNED(16);

static void pager(void *arg) {
  (void)arg;
  for (;;) {
    mx_bios_wait_sema(psema);
    while (pref_out != pref_in) {
      const void *p = pref[pref_out % NPREF].p;
      u32 bytes = pref[pref_out % NPREF].bytes;
      pref_out++;
      if (!in_vm(p, bytes))
        continue;
      lock();
      for (u32 pg = first_page(p), b = last_page(p, bytes); pg <= b; pg++)
        if (!vm.frame_of[pg])
          fault_in(pg, 0);
      unlock();

    }
  }
}

void mx__vm_prefetch(const void *p, u32 bytes) {
  if (!vm.active || !mx_is_vm(p) || !in_vm(p, bytes) || mx__vm_resident(p, bytes))
    return;
  if (psema < 0) {
    struct {
      s32 count, max_count, init_count, wait_threads;
      u32 attr, option;
    } sp = {0, NPREF, 0, 0, 0, 0};
    int me = mx_thread_self(), prio = mx_thread_prio(me);
    psema = mx_bios_create_sema(&sp);
    int id = psema < 0 ? -1 : mx_thread_start(pager, 0, pstack, sizeof pstack, prio - 1);
    if (id < 0) {
      mx_printf("maxsdk: the VM's pager thread did not start\n");
      psema = -2;
    } else
      mx__thread_keep(id);
  }
  if (psema < 0 || pref_in - pref_out >= NPREF)
    return; /* asked again later */
  pref[pref_in % NPREF].p = p;
  pref[pref_in % NPREF].bytes = bytes;
  pref_in++;
  mx_bios_signal_sema(psema);
}

/* The whole pages in [p, p + bytes) of a heap of pages are free (mem.c):
   their contents go (no swap write; zero when touched again). The lock is
   taken; pinned pages stay. */
void mx__vm_discard(const void *p, u32 bytes) {
  u32 a = ((u32)(uintptr_t)p + PAGE - 1) & ~(PAGE - 1), e = ((u32)(uintptr_t)p + bytes) & ~(PAGE - 1);
  if (!vm.active || e <= a || !in_vm((const void *)(uintptr_t)a, e - a))
    return;
  lock();
  for (u32 pg = va_page(a), b = va_page(e - 1); pg <= b; pg++) {
    if (vm.pins[pg] || (vm.flags[pg] & PG_BUSY))
      continue;
    int was = mx_di();
    unmap(pg);
    if (vm.frame_of[pg]) {
      vm.page_of[vm.frame_of[pg] - 1u] = 0;
      vm.nfree++;
      vm.frame_of[pg] = 0;
    }
    vm.flags[pg] = 0;
    mx_irq_restore(was);
  }
  unlock();
}

/* [p, p + bytes)'s resident pages the oldest: eviction takes them first
   (a refill stamps a page again when it is used) */
void mx__vm_cold(const void *p, u32 bytes) {
  if (!vm.active || !in_vm(p, bytes))
    return;
  int was = mx_di();
  for (u32 pg = first_page(p), b = last_page(p, bytes); pg <= b; pg++)
    if (vm.frame_of[pg])
      F.stamp[pg] = F.refills - 0x40000000u;
  mx_irq_restore(was);
}

/* ---- mapped files ------------------------------------------------------------------ */

const void *mx_vm_map_file(const char *path, u32 *size) {
  if (!vm.active)
    return 0;
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return 0;
  int n = mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
  u32 npg = n > 0 ? ((u32)n + PAGE - 1) / PAGE : 0;
  const void *p = 0;
  lock();
  if (npg && vm.map_next + npg <= vm.nmap) {
    u32 pg = vm.npages + vm.map_next;
    if (file_pages(fd, pg, (u32)n, 0) >= 0) {
      vm.map_next += npg;
      p = (const void *)(uintptr_t)page_va(pg);
    }
  }
  unlock();
  if (!p) {
    mx_printf("mx_vm_map_file: %s: %s\n", path,
              n <= 0 ? "empty, or no size" : "no room (mx_vm_config.map_size, 8 files)");
    mx_fio_close(fd);
    return 0;
  }
  if (size)
    *size = (u32)n;
  return p;
}

void mx_vm_unmap_file(const void *p) {
  u32 pg = first_page(p);
  if (pg == ~0u || !is_mapped(pg))
    return;
  lock();
  u32 i = 0;
  while (i < vm.nseg && !(vm.seg[i].fd >= 0 && vm.seg[i].pg == pg))
    i++;
  if (i == vm.nseg) {
    unlock();
    return;
  }
  u32 npg = vm.seg[i].npg;
  int fd = vm.seg[i].fd;
  for (u32 q = pg; q < pg + npg; q++) {
    if (vm.pins[q])
      vm_fatal("mx_vm_unmap_file: a page is pinned", q);
    int was = mx_di();
    unmap(q);
    if (vm.frame_of[q]) {
      vm.page_of[vm.frame_of[q] - 1u] = 0;
      vm.frame_of[q] = 0;
      vm.nfree++;
    }
    vm.flags[q] = 0;
    mx_irq_restore(was);
  }
  /* unmapped, its pages kept (npg) until the window's end comes back to
     them: the window is given out from its start */
  vm.seg[i].fd = -1;
  for (int k = 0; k >= 0;) {
    k = -1;
    for (u32 j = 0; j < vm.nseg; j++)
      if (vm.seg[j].fd < 0 && vm.seg[j].npg &&
          vm.seg[j].pg + vm.seg[j].npg == vm.npages + vm.map_next)
        k = (int)j;
    if (k >= 0)
      vm.map_next -= vm.seg[k].npg, vm.seg[k].npg = 0;
  }
  unlock();
  mx_fio_close(fd);
}

void (*mx__core_fault_hook)(const void *p, const char *what);

void mx__core_fault(const void *p, const char *what) {
  if (mx__core_fault_hook) {
    mx__core_fault_hook(p, what);
    return;
  }
  mx_printf("\nVM: %s at %p is VM: what an interrupt handler runs or reads must be core "
            "(MX_CORE_FN, MX_CORE_DATA, pinned memory; <mx/cpu.h>, <mx/mem.h>)\n",
            what, p);
  MX_TEST_END(255);
  mx_exit(255);
}

/* ---- set up ---------------------------------------------------------------------- */

void mx__vm_report(void) {
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  /* two lines: long ones were lost on the PS2 (ps2link) */
  mx_printf("maxsdk: VM %u frames, %u faults: %u swap ins (%u ms), %u outs (%u ms), %u by the "
            "writer, %u drops, %u from files (%u ms), %u zero fills\n",
            s.frames, s.faults, s.swap_ins, s.us_swap_in / 1000, s.swap_outs + s.writer_outs,
            s.us_swap_out / 1000, s.writer_outs, s.drops, s.file_ins, s.us_file_in / 1000,
            s.zero_fills);
  mx_printf("maxsdk: VM lock waits %u ms; %u file server calls; %u TLB refills (%u of code; "
            "about %u ms; %u entries); %u frames pinned\n",
            s.us_lock / 1000, s.io_calls, s.refills, s.refills_i,
            s.refills / (MX_EE_HZ / 1000 / MX_VM_REFILL_CYCLES), s.tlb_entries, s.pinned_frames);
  if (vm.tr.cap)
    mx_printf("maxsdk: VM trace %u events (%u lost), every %u ms\n", vm.tr.n, vm.tr.lost,
              vm.tr.every_ms);
}

void mx_vm_stats(struct mx_vm_stats *s) {
  *s = vm.st;
  s->refills = F.refills;
  s->refills_i = F.refills_i;
  s->pages = vm.npages;
  s->frames = vm.nframes;
  s->free_frames = vm.nfree;
  s->pinned_frames = vm.pinned_frames;
  s->tlb_entries = mx__vm_tlb_entries();
  s->dirty_tracking = (u32)vm.dbit;
}

/* Tests: are the tables and the TLB consistent? 0, or the first problem
   found: 1 << 28 | frame (page_of and frame_of disagree), 2 << 28 (the free
   count), 3 << 28 | page (a mapped page's EntryLo is not its frame), 4 << 28
   | entry (page_slot and slot_page disagree), 5 << 28 | entry (the entry
   in the TLB is not the page's), 6 << 28 | i << 8 | j (TLB entries i and j
   match one address: on MIPS, undefined). Call with the VM lock free, from
   one thread. */
u32 mx__vm_check(void) {
  u32 r = 0;
  int was = mx_di();
  u32 nfree = 0;
  for (u32 f = 0; f < vm.nframes && !r; f++) {
    u32 q = vm.page_of[f];
    if (!q)
      nfree++;
    else if (q < FRAME_PINNED && vm.frame_of[q - 1] != f + 1)
      r = 1u << 28 | f;
  }
  if (!r && nfree != vm.nfree)
    r = 2u << 28;
  for (u32 pg = 0; pg < npages_all() && !r; pg++) {
    u32 f = vm.frame_of[pg];
    if (f && vm.page_of[f - 1] != pg + 1)
      r = 1u << 28 | (f - 1);
    else if ((vm.lo[pg] & LO_V) &&
             (!f || vm.lo[pg] >> 6 != mx_paddr_of(frame_ptr(f - 1)).v >> 12))
      r = 3u << 28 | pg;
  }
  for (u32 s = 0; s < TLB_N && !r; s++) {
    u32 q = F.slot_page[s];
    if (q) {
      u32 pg = (q - 1) / vm.bpp, b = (q - 1) % vm.bpp;
      if (vm.page_slot[q - 1] != s)
        r = 4u << 28 | s;
      else {
        u32 pm, hi, lo0, lo1;
        tlb_read(s, &pm, &hi, &lo0, &lo1);
        if (vm.bpp > 1 ? pm != PM4K || (hi & ~0x1FFFu) != page_va(pg) + b * BLK ||
                             (lo0 & ~LO_D) != ((vm.lo[pg] & ~LO_D) + (b * BLK >> 12 << 6)) ||
                             (lo0 & LO_D & ~vm.lo[pg])
                       : pm != PM16K || (hi & ~0x1FFFu) != page_va(pg) || lo0 != vm.lo[pg])
          r = 5u << 28 | s;
      }
    }
  }
  /* any two entries (V or not) whose ranges overlap */
  static u32 lo_va[TLB_N], hi_va[TLB_N];
  for (u32 i = 0; i < TLB_N && !r; i++) {
    u32 pm, hi, lo0, lo1;
    tlb_read(i, &pm, &hi, &lo0, &lo1);
    u32 pair = (pm | 0x1FFFu) + 1;
    lo_va[i] = hi & ~(pair - 1);
    hi_va[i] = lo_va[i] + pair - 1;
    for (u32 j = 0; j < i && !r; j++)
      if (lo_va[i] <= hi_va[j] && lo_va[j] <= hi_va[i])
        r = 6u << 28 | i << 8 | j;
  }
  mx_irq_restore(was);
  return r;
}

static void *table(u32 bytes) {
  void *p = mx__phys_alloc(bytes, 16);
  if (p)
    __builtin_memset(p, 0, bytes);
  return p;
}

/* mx_vm_init failed: what it took goes back (the tables, the swap file,
   the semaphores, the frames), so that the program has the memory as
   without VM and mx_vm_init may be tried again. The VM_RESUME syscall
   stays set: it is only reached from VM's own faults. */
static void undo_init(void) {
  mx__phys_free(vm.lo), vm.lo = 0;
  mx__phys_free(vm.frame_of), vm.frame_of = 0;
  mx__phys_free(vm.slot_of), vm.slot_of = 0;
  mx__phys_free(vm.flags), vm.flags = 0;
  mx__phys_free(vm.pins), vm.pins = 0;
  mx__phys_free(vm.page_slot), vm.page_slot = 0;
  mx__phys_free(F.stamp), F.stamp = 0;
  mx__phys_free(vm.page_of), vm.page_of = 0;
  mx__phys_free(vm.tr.seen), vm.tr.seen = 0;
  mx__phys_free(vm.tr.buf), vm.tr.buf = 0;
  if (vm.lock >= 0)
    mx_bios_delete_sema(vm.lock);
  if (vm.swapsema >= 0)
    mx_bios_delete_sema(vm.swapsema);
  vm.lock = vm.swapsema = -1;
  if (vm.fd >= 0)
    mx_fio_close(vm.fd);
  vm.fd = -1;
  mx__heap_untake();
}

/* The kernel, for a program whose core is at a run of its own: the frames
   are this physical memory, not the heap's top (step 2b). */
static u32 frames_lo, frames_hi;
void mx__vm_frames(u32 phys_lo, u32 phys_hi) {
  frames_lo = (phys_lo + PAGE - 1) & ~(PAGE - 1);
  frames_hi = phys_hi & ~(PAGE - 1);
}

int mx_vm_init(const struct mx_vm_config *cfg) {
  if (vm.active || mx_bios_get_memory_size() != 32 * 1024 * 1024)
    return -1;
  vm.lock = vm.swapsema = -1;
  u32 size = cfg->size ? cfg->size : 128u << 20;
  if (size > MX_VM_LIMIT - MX_VM_BASE)
    size = MX_VM_LIMIT - MX_VM_BASE;
  u32 np = size / PAGE;
  if (!np)
    return -1;
  vm.fd = -1;
  if (cfg->swap[0]) {
    /* not emptied: a swap file made ahead keeps its clusters, so paging on
       FAT and exFAT changes no file system data, and a power cut none */
    extern const char *mx__loader_swap;
    const char *swap = mx__loader_swap ? mx__loader_swap : cfg->swap;
    vm.fd = mx_fio_open(swap, 3 /* RDWR */ | MX_FIO_CREAT);
    if (vm.fd < 0) {
      mx_printf("mx_vm_init: cannot open the swap file %s (%d)\n", swap, vm.fd);
      return -1;
    }
  }
  u32 nmap = (cfg->map_size ? cfg->map_size : 64u << 20) / PAGE;
  if (nmap > (MX_VM_MAP_LIMIT - MX_VM_MAP_BASE) / PAGE)
    nmap = (MX_VM_MAP_LIMIT - MX_VM_MAP_BASE) / PAGE;
  u32 nheap = (cfg->heap_size ? cfg->heap_size : 64u << 20) / PAGE;
  if (nheap > (MX_VM_HEAP_LIMIT - MX_VM_HEAP_BASE) / PAGE)
    nheap = (MX_VM_HEAP_LIMIT - MX_VM_HEAP_BASE) / PAGE;
  vm.bpp = cfg->trace ? BPP : 1; /* slot_page holds a block + 1 */
  if ((np + nmap + nheap) * vm.bpp > 0xFFFE) {
    nheap = 0;
    if ((np + nmap) * vm.bpp > 0xFFFE)
      nmap = 0xFFFE / vm.bpp > np ? 0xFFFE / vm.bpp - np : 0;
  }
  u32 nt = np + nmap + nheap; /* the tables: VM's pages, the file window's, the heap window's */
  vm.lo = table(nt * 4);
  vm.frame_of = table(nt * 2);
  vm.slot_of = table(nt * 2);
  vm.flags = table(nt);
  vm.pins = table(nt);
  vm.page_slot = table(nt * vm.bpp);
  F.stamp = table(nt * 4);
  if (cfg->trace) {
    vm.tr.seen = table(nt * BPP);
    vm.tr.buf = mx__phys_alloc(cfg->trace, 16);
    if (!vm.tr.seen || !vm.tr.buf) {
      mx_printf("mx_vm_init: no core memory for the trace (%u bytes)\n", cfg->trace);
      undo_init();
      return -1;
    }
  }
  /* everything above the break is frames but what the VM's last tables
     and the physical allocator's little use after it need; or, the core at
     a run of its own (the kernel, mx__vm_frames), the physical memory the
     kernel gives */
  u32 keep = 64u << 10;
  u32 bytes;
  u8 *mem;
  if (frames_hi > frames_lo) {
    mem = (u8 *)(uintptr_t)(MX_PHYS_WIN + frames_lo);
    bytes = frames_hi - frames_lo;
  } else
    mem = mx__heap_take(keep, PAGE, &bytes);
  if (cfg->frames && cfg->frames < bytes)
    bytes = cfg->frames & ~(PAGE - 1);
  u32 nf = bytes / PAGE;
  if (nf > 0xFFFE)
    nf = 0xFFFE;
  vm.page_of = table(nf * 2);
  if (!vm.lo || !vm.frame_of || !vm.slot_of || !vm.flags || !vm.pins || !vm.page_slot || !F.stamp ||
      !vm.page_of || nf < 4) {
    undo_init();
    mx_printf("mx_vm_init: not enough memory (%u frames)\n", nf);
    return -1;
  }
  if (vm.fd < 0 && np > nf)
    np = nf;
  vm.npages = np;
  vm.nmap = nmap;
  vm.nheap = nheap;
  vm.heap_next = 0;
  vm.pinned_frames = 0;
  /* the frames through MX_PHYS_WIN: the BIOS's mapping of them goes */
  u32 flo = mx_paddr_of(mem).v, fhi = flo + nf * PAGE;
  vm.frames = (u8 *)(uintptr_t)(MX_PHYS_WIN + flo);
  vm.nframes = vm.nfree = nf;
  struct {
    s32 count, max_count, init_count, wait_threads;
    u32 attr, option;
  } sp = {0, 1, 1, 0, 0, 0};
  vm.lock = mx_bios_create_sema(&sp);
  vm.swapsema = mx_bios_create_sema(&sp);
  vm.wsema = -1;

  mx_bios_flush_cache(0); /* no dirty line of the frames under the BIOS's mapping */
  int was = mx_di();
  tlb_init(flo, fhi);
  mx_irq_restore(was);
  mx_bios_set_syscall(VM_RESUME_SYSCALL, (void *)mx__vm_resume_k);
  for (int c = 1; c <= 3; c++)
    bios_handler[c] = mx_bios_set_tlb_handler(c, mx__vm_vector);
  mx_bios_flush_cache(0);
  vm.dbit = 1;
  vm.active = 1;

  /* Does a load fault (PCSX2's recompiler does not raise TLB misses), and a
     store to a clean page (PCSX2 ignores D)? Page 0, then free again. */
  volatile u32 *t = (volatile u32 *)mx_vm_base();
  u32 v = t[0];
  t[1] = v + 1;
  int faults = vm.st.zero_fills == 1 && t[1] == 1;
  vm.dbit = vm.modified_seen != 0;
  was = mx_di();
  if (vm.frame_of[0]) {
    vm.page_of[vm.frame_of[0] - 1u] = 0; /* free again */
    vm.frame_of[0] = 0;
    vm.nfree++;
  }
  vm.flags[0] = 0;
  unmap(0);
  vm.st.zero_fills = vm.st.faults = 0;
  F.refills = F.refills_i = 0;
  mx_irq_restore(was);
  if (!faults) {
    mx__vm_shutdown();
    mx_exception_init();
    undo_init();
    mx_printf("mx_vm_init: TLB misses are not raised (PCSX2: run with the EE interpreter)\n");
    return -1;
  }
  /* the fast refill (tlb.S): without a trace (a page, one entry) */
  was = mx_di();
  F.lo = vm.lo;
  F.page_slot = vm.page_slot;
  F.npages = vm.bpp == 1 ? vm.npages : 0;
  mx_irq_restore(was);
  if (cfg->trace) {
    was = mx_di();
    vm.tr.every_ms = cfg->trace_ms ? cfg->trace_ms : 100;
    vm.tr.epoch = 1;
    vm.tr.t_last = mx_count();
    vm.tr.cap = cfg->trace / 4;
    mx_irq_restore(was);
  }
  mx__irq_vbon_hook(vblank);
  /* the VM's threads (the writer, the pager) run above the program's: it
     goes to priority 2 if it was above that (0 when started by PCSX2) */
  if (mx_thread_prio(mx_thread_self()) < 2)
    mx_thread_set_prio(mx_thread_self(), 2);
  if (cfg->clean_frames && vm.fd >= 0)
    writer_start(cfg->clean_frames);
  return 0;
}

/* The kernel at a program's end: the VM's vblank function off (its state is
   set back as linked then, docs/mx-os-plan.md step 2a). */
void mx__vm_unhook(void) { mx__irq_vbon_unhook(vblank); }
