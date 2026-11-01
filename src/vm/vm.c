/* Virtual memory (<mx/vm.h>, docs/vm.md).

   VM is MX_VM_BASE + npages 32 KiB pages. A page is one TLB entry, both of
   its 16 KiB halves (PageMask 16 KiB) valid: an entry with a V = 0 half
   hangs the PS2 (the TLB invalid exception, probe/p19_tlb.c), so a page is
   either in the TLB whole or not at all, and every miss is a refill. The
   TLB is written with tlbwi only, at entries
   TLB_FIRST..47 in turn, with a table of what each holds: PCSX2 keeps
   Random at 0 (tlbwr would overwrite the scratchpad's entry 0) and its tlbp
   finds nothing. The BIOS's own entries below TLB_FIRST (scratchpad, I/O,
   main memory cached and uncached) stay; its UCAB entries (31..38) may be
   replaced and are put back on a miss (alias[]).

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
#include <mx/vm.h>

#include "vm_int.h"

#define PAGE MX_VM_PAGE
#define PAGE_SHIFT 15
#define HALF (PAGE / 2)
#define PM16K 0x6000u
#define TLB_FIRST 31
#define TLB_N 48
#define SLOT_ALIAS 0xFFFFu   /* a TLB entry holding one of the BIOS's UCAB entries */
#define FRAME_TAKEN 0xFFFFu  /* page_of: taken, its page not there yet */

/* EntryLo bits */
#define LO_G 1u
#define LO_V 2u
#define LO_D 4u
#define LO_CACHED (3u << 3)

/* page flags */
#define PG_DIRTY 1 /* written since its frame was last written out */
#define PG_REF 2   /* used since the clock last passed */
#define PG_BUSY 4  /* in transit to or from the swap file */
#define PG_SWAP 8  /* its slot holds it */
#define PG_FILE 16 /* the program's ELF holds it (until PG_SWAP) */

static struct {
  int active;
  u32 npages, nframes;
  u8 *frames;
  u32 *lo;
  u16 *frame_of, *slot_of, *page_of;
  u8 *flags, *pins;
  u8 *page_slot; /* TLB entry of each page, 0: none */
  u16 slot_page[TLB_N];
  u32 next_slot;
  u32 nfree, free_hint, hand;
  int dbit;
  int fd;
  u32 nslots;
  int lock;
  struct {
    u32 pm, hi, lo0, lo1;
  } alias[8];
  struct mx_vm_stats st;
  u32 modified_seen;
  int code; /* the program runs code from VM (src/vm/image.c) */
  /* the segments of a program linked into VM, read from its ELF */
  int efd;
  u32 nseg;
  struct {
    u32 va, filesz, offset;
  } seg[4];
} vm;

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

static void tlb_write(u32 i, u32 pm, u32 hi, u32 lo0, u32 lo1) {
  __asm__ volatile("mtc0 %0, $0\n\tmtc0 %1, $5\n\tmtc0 %2, $10\n\tmtc0 %3, $2\n\tmtc0 %4, $3\n\t"
                   "sync.p\n\ttlbwi\n\tsync.p"
                   :
                   : "r"(i), "r"(pm), "r"(hi), "r"(lo0), "r"(lo1)
                   : "memory");
}

static void tlb_read(u32 i, u32 *pm, u32 *hi, u32 *lo0, u32 *lo1) {
  __asm__ volatile("mtc0 %4, $0\n\tsync.p\n\ttlbr\n\tsync.p\n\t"
                   "mfc0 %0, $5\n\tmfc0 %1, $10\n\tmfc0 %2, $2\n\tmfc0 %3, $3"
                   : "=r"(*pm), "=r"(*hi), "=r"(*lo0), "=r"(*lo1)
                   : "r"(i));
}

/* A TLB entry for something new: the next one in turn, freed of what it
   held. Interrupts off (or at EXL). */
static u32 tlb_take(void) {
  u32 s = vm.next_slot;
  vm.next_slot = s + 1 < TLB_N ? s + 1 : TLB_FIRST;
  u32 old = vm.slot_page[s];
  if (old && old != SLOT_ALIAS)
    vm.page_slot[old - 1] = 0;
  vm.slot_page[s] = 0;
  return s;
}

/* An entry that matches nothing: in kseg3, V = 0, one address an entry
   above those of the BIOS's unused entries (InitTLB: entry i at
   0xE0000000 + i * 8 KiB on the PS2, (i - 1) * 8 KiB on PCSX2, up to
   0xE005E000), so never two alike. */
static void tlb_clear(u32 s) { tlb_write(s, 0, 0xE0000000u + ((48 + s) << 13), 0, 0); }

/* Page pg's entry as lo[] says: written (a new one if it has none) when
   mapped, taken out of the TLB when not. Interrupts off (or at EXL). */
static void tlb_sync(u32 pg) {
  u32 lo = vm.lo[pg], s = vm.page_slot[pg];
  if (!(lo & LO_V)) {
    if (s) {
      tlb_clear(s);
      vm.slot_page[s] = 0;
      vm.page_slot[pg] = 0;
    }
    return;
  }
  if (!s) {
    s = tlb_take();
    vm.page_slot[pg] = (u8)s;
    vm.slot_page[s] = (u16)(pg + 1);
  }
  tlb_write(s, PM16K, MX_VM_BASE + pg * PAGE, lo, lo + (HALF >> 12 << 6));
}

/* A miss in the BIOS's UCAB range: its entry back. */
static int alias_refill(u32 va) {
  for (int i = 0; i < 8; i++) {
    /* an entry maps an even and an odd page: (PageMask | 0x1FFF) + 1 bytes */
    u32 pair = (vm.alias[i].pm | 0x1FFFu) + 1;
    if (va - vm.alias[i].hi < pair) {
      u32 s = tlb_take();
      vm.slot_page[s] = SLOT_ALIAS;
      tlb_write(s, vm.alias[i].pm, vm.alias[i].hi, vm.alias[i].lo0, vm.alias[i].lo1);
      return 1;
    }
  }
  return 0;
}

/* ---- pages and frames ------------------------------------------------------- */

/* Map resident page pg; write: the access is a store. Interrupts off. */
static void map(u32 pg, int write) {
  if (write || !vm.dbit)
    vm.flags[pg] |= PG_DIRTY;
  u32 f = vm.frame_of[pg] - 1u;
  vm.lo[pg] = mx_paddr_of(frame_ptr(f)).v >> 12 << 6 | LO_CACHED | LO_V | LO_G |
              (vm.flags[pg] & PG_DIRTY ? LO_D : 0);
  vm.flags[pg] |= PG_REF;
  tlb_sync(pg);
}

static void unmap(u32 pg) {
  vm.lo[pg] = 0;
  tlb_sync(pg);
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
  return q && q != FRAME_TAKEN ? (int)(q - 1) : -1;
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
  if (code == 2 || code == 3) {
    if (va - 0x30100000u < 0x01F00000u && alias_refill(va))
      return VMX_RETURN;
  }
  if (code < 1 || code > 3 || va - MX_VM_BASE >= vm.npages * PAGE) {
    mx__exception_note = va - MX_VM_BASE < MX_VM_LIMIT - MX_VM_BASE
                             ? "VM: outside the VM in use (mx_vm_size)"
                             : 0;
    return VMX_FATAL;
  }
  u32 pg = (va - MX_VM_BASE) >> PAGE_SHIFT;
  if (code == 1) { /* a store to a clean page */
    vm.modified_seen++;
    if (!vm.frame_of[pg] || (vm.flags[pg] & PG_BUSY)) {
      mx__exception_note = "VM: TLB modified on a page that is not resident";
      return VMX_FATAL;
    }
    vm.flags[pg] |= PG_DIRTY;
    vm.lo[pg] |= LO_D;
    tlb_sync(pg);
    return VMX_RETURN;
  }
  vm.st.refills++;
  if (vm.lo[pg] & LO_V) { /* mapped, only not in the TLB */
    tlb_sync(pg);
    return VMX_RETURN;
  }
  if (vm.frame_of[pg] && !(vm.flags[pg] & PG_BUSY)) { /* the clock took V away */
    map(pg, code == 3);
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
  mx_printf("\nVM: %s (page %u at %p)\n", what, pg, (void *)(uintptr_t)(MX_VM_BASE + pg * PAGE));
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

static void swap_io(u32 pg, u32 f, int out) {
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

/* Page pg (PG_FILE) into frame f from the program's ELF: the segment's
   bytes of the page, the rest zero. */
static void file_in(u32 pg, u32 f) {
  irq_check(0x18);
  u32 va = MX_VM_BASE + (pg << PAGE_SHIFT), i = 0;
  while (i < vm.nseg && !(va >= vm.seg[i].va && va - vm.seg[i].va < vm.seg[i].filesz))
    i++;
  if (i == vm.nseg)
    vm_fatal("a page of the program's ELF outside its segments", pg);
  u32 at = vm.seg[i].offset + (va - vm.seg[i].va), n = vm.seg[i].va + vm.seg[i].filesz - va;
  if (n > PAGE)
    n = PAGE;
  int got = -1, sought = -1;
  for (int tries = 0; tries < 4 && got != (int)n; tries++) {
    if (tries)
      vm.st.retries++;
    sought = mx_fio_lseek(vm.efd, (s32)at, MX_FIO_SEEK_SET);
    if (sought == (int)at)
      got = mx_fio_read(vm.efd, frame_ptr(f), n);
  }
  if (got != (int)n) {
    mx_printf("\nVM: the program's ELF at %u: seek %d, read %d of %u\n", at, sought, got, n);
    vm_fatal("reading the program's ELF failed", pg);
  }
  if (n < PAGE)
    __builtin_memset(frame_ptr(f) + n, 0, PAGE - n);
}

/* src/vm/image.c: pages [va, va + filesz) come from the ELF open as fd, at
   offset; va on a page. The rest of the segment is new pages (zero). */
int mx__vm_file_pages(int fd, u32 va, u32 filesz, u32 offset) {
  if (vm.nseg == sizeof vm.seg / sizeof vm.seg[0] || (va & (PAGE - 1)) || va < MX_VM_BASE ||
      va - MX_VM_BASE + filesz > vm.npages * PAGE)
    return -1;
  vm.efd = fd;
  vm.seg[vm.nseg].va = va;
  vm.seg[vm.nseg].filesz = filesz;
  vm.seg[vm.nseg].offset = offset;
  vm.nseg++;
  u32 a = (va - MX_VM_BASE) >> PAGE_SHIFT, b = (va - MX_VM_BASE + filesz + PAGE - 1) >> PAGE_SHIFT;
  int was = mx_di();
  for (u32 pg = a; pg < b; pg++)
    vm.flags[pg] |= PG_FILE;
  mx_irq_restore(was);
  return 0;
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

/* Take a frame by the clock: a page used since the last pass loses V (and
   is kept), the first one not used goes (written out if dirty). Returns
   the frame, marked taken. The lock is held. */
static u32 evict(void) {
  mx_vm_unpin_done();
  for (u32 n = 0; n < 3 * vm.nframes; n++) {
    int was = mx_di();
    int f = frame_take(); /* freed meanwhile */
    if (f >= 0) {
      mx_irq_restore(was);
      return (u32)f;
    }
    u32 fr = vm.hand;
    vm.hand = fr + 1 < vm.nframes ? fr + 1 : 0;
    int q = page_in(fr);
    if (q < 0 || vm.pins[q] || (vm.flags[q] & PG_BUSY)) {
      mx_irq_restore(was);
      continue;
    }
    u32 pg = (u32)q;
    if (vm.flags[pg] & PG_REF) {
      vm.flags[pg] &= (u8)~PG_REF;
      unmap(pg);
      mx_irq_restore(was);
      continue;
    }
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
  vm_fatal("out of memory: every frame is pinned", 0);
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

static void lock(void) { mx_bios_wait_sema(vm.lock); }
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
  u32 pg = (f->badvaddr - MX_VM_BASE) >> PAGE_SHIFT;
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

static u32 first_page(const void *p) { return ((uintptr_t)p - MX_VM_BASE) >> PAGE_SHIFT; }
static u32 last_page(const void *p, u32 bytes) {
  return ((uintptr_t)p + (bytes ? bytes : 1) - 1 - MX_VM_BASE) >> PAGE_SHIFT;
}

static int in_vm(const void *p, u32 bytes) {
  return vm.active && (uintptr_t)p - MX_VM_BASE < vm.npages * PAGE &&
         bytes <= vm.npages * PAGE - ((uintptr_t)p - MX_VM_BASE);
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
      if (vm.page_of[f0 + i] == FRAME_TAKEN ||
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
  for (u32 pg = a; pg <= b; pg++) {
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
    mx_irq_restore(was);
  }
  unlock();
  return 0;
}

void mx_vm_unpin(const void *p, u32 bytes) {
  if (!mx_is_vm(p) || !in_vm(p, bytes))
    return;
  int was = mx_di();
  for (u32 pg = first_page(p), b = last_page(p, bytes); pg <= b; pg++)
    if (vm.pins[pg] && !--vm.pins[pg])
      vm.st.pinned--;
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
            "addresses must be core (mx_core_alloc, mx_dma_alloc, static data); VM data "
            "only through mx_vm_pin (<mx/dma.h>)\n",
            p, bytes);
  MX_TEST_END(255);
  mx_exit(255);
}

/* ---- set up ---------------------------------------------------------------------- */

void mx_vm_stats(struct mx_vm_stats *s) {
  *s = vm.st;
  s->pages = vm.npages;
  s->frames = vm.nframes;
  s->free_frames = vm.nfree;
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
    else if (q != FRAME_TAKEN && vm.frame_of[q - 1] != f + 1)
      r = 1u << 28 | f;
  }
  if (!r && nfree != vm.nfree)
    r = 2u << 28;
  for (u32 pg = 0; pg < vm.npages && !r; pg++) {
    u32 f = vm.frame_of[pg];
    if (f && vm.page_of[f - 1] != pg + 1)
      r = 1u << 28 | (f - 1);
    else if ((vm.lo[pg] & LO_V) &&
             (!f || vm.lo[pg] >> 6 != mx_paddr_of(frame_ptr(f - 1)).v >> 12))
      r = 3u << 28 | pg;
  }
  for (u32 s = TLB_FIRST; s < TLB_N && !r; s++) {
    u32 q = vm.slot_page[s];
    if (q && q != SLOT_ALIAS) {
      if (vm.page_slot[q - 1] != s)
        r = 4u << 28 | s;
      else {
        u32 pm, hi, lo0, lo1;
        tlb_read(s, &pm, &hi, &lo0, &lo1);
        if (pm != PM16K || (hi & ~0x1FFFu) != MX_VM_BASE + (q - 1) * PAGE ||
            lo0 != vm.lo[q - 1])
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
  void *p = mx_core_alloc(bytes, 16);
  if (p)
    __builtin_memset(p, 0, bytes);
  return p;
}

int mx_vm_init(const struct mx_vm_config *cfg) {
  if (vm.active || mx_bios_get_memory_size() != 32 * 1024 * 1024)
    return -1;
  u32 size = cfg->size ? cfg->size : 128u << 20;
  if (size > MX_VM_LIMIT - MX_VM_BASE)
    size = MX_VM_LIMIT - MX_VM_BASE;
  u32 np = size / PAGE;
  if (!np)
    return -1;
  vm.fd = -1;
  if (cfg->swap[0]) {
    vm.fd = mx_fio_open(cfg->swap, 3 /* RDWR */ | MX_FIO_CREAT | MX_FIO_TRUNC);
    if (vm.fd < 0) {
      mx_printf("mx_vm_init: cannot open the swap file %s (%d)\n", cfg->swap, vm.fd);
      return -1;
    }
  }
  vm.lo = table(np * 4);
  vm.frame_of = table(np * 2);
  vm.slot_of = table(np * 2);
  vm.flags = table(np);
  vm.pins = table(np);
  vm.page_slot = table(np);
  u32 keep = cfg->core_heap ? cfg->core_heap : 1u << 20;
  u32 bytes;
  u8 *mem = mx__heap_take(keep, PAGE, &bytes);
  if (cfg->frames && cfg->frames < bytes)
    bytes = cfg->frames & ~(PAGE - 1);
  u32 nf = bytes / PAGE;
  if (nf > 0xFFFE)
    nf = 0xFFFE;
  vm.page_of = table(nf * 2);
  if (!vm.lo || !vm.frame_of || !vm.slot_of || !vm.flags || !vm.pins || !vm.page_slot ||
      !vm.page_of || nf < 4) {
    mx_printf("mx_vm_init: not enough memory (%u frames)\n", nf);
    return -1;
  }
  if (vm.fd < 0 && np > nf)
    np = nf;
  vm.npages = np;
  vm.frames = mem;
  vm.nframes = vm.nfree = nf;
  vm.lock = -1;
  struct {
    s32 count, max_count, init_count, wait_threads;
    u32 attr, option;
  } sp = {0, 1, 1, 0, 0, 0};
  vm.lock = mx_bios_create_sema(&sp);

  /* the TLB: the BIOS's UCAB entries are put back on a miss */
  int was = mx_di();
  for (u32 i = 0; i < 8; i++) {
    tlb_read(TLB_FIRST + i, &vm.alias[i].pm, &vm.alias[i].hi, &vm.alias[i].lo0,
             &vm.alias[i].lo1);
    vm.alias[i].lo0 |= LO_G, vm.alias[i].lo1 |= LO_G;
    vm.slot_page[TLB_FIRST + i] = SLOT_ALIAS;
  }
  vm.next_slot = TLB_FIRST + 8;
  mx_irq_restore(was);
  mx_bios_set_syscall(VM_RESUME_SYSCALL, (void *)mx__vm_resume_k);
  for (int c = 1; c <= 3; c++)
    mx_bios_set_tlb_handler(c, mx__vm_vector);
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
  vm.st.zero_fills = vm.st.refills = vm.st.faults = 0;
  mx_irq_restore(was);
  if (!faults) {
    vm.active = 0;
    mx_exception_init();
    mx_printf("mx_vm_init: TLB misses are not raised (PCSX2: run with the EE interpreter)\n");
    return -1;
  }
  return 0;
}
