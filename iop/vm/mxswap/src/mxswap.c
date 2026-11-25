/* mxswap: the VM's swap file on the IOP (common/include/mxswap.h).

   The EE puts compressed pages here; they wait in a queue in IOP memory
   (a ring: the module's first argument in KiB, 512 by default, less when
   the IOP has not that much free with 160 KiB left over; taken at the
   first OPEN, when the swap file's drive is there and mounted) and a thread of
   low priority writes each at its slot, in blocks (the second argument in
   KiB, 32 by default: a page at once). A get takes the newest copy of the page from the
   queue, else reads it from the file in the RPC thread (high priority).
   The file is one handle, used by one thread at a time (fmutex), let go
   between the writer's blocks: a read waits at most for one block, and
   sees what was written (no second handle's own cache). */
#include <irx.h>
#include <intrman.h>
#include <ioman.h>
#include <loadcore.h>
#include <sifrpc.h>
#include <stdio.h>
#include <sysclib.h>
#include <sysmem.h>
#include <thbase.h>
#include <thsemap.h>
#include <mxswap.h>

IRX_ID("mxswap", 1, 0);

#define NENT 256 /* pages in the queue at most */
static struct ent {
  u32 slot, bytes, off;
  int state; /* 0 free, 1 queued, 2 being written, 3 stale (a newer one came) */
} ent[NENT];
static u32 qhead, qtail, count; /* entries: the oldest at qhead */
static u8 *pool;
static u32 pool_bytes, head, tail; /* the ring: allocated from head, freed at tail */
static int fd = -1;
static u32 block = MXSWAP_PAGE; /* the writer's blocks: whole pages (4 KiB blocks: 25% slower on a USB stick, reads no sooner) */
static u32 want_kib;      /* the queue asked for */

/* the queue, at the first OPEN (the drives' file systems have taken their
   memory by then): what is free less 160 KiB for the others (a drive that
   comes later, sockets, the kernel's module loads), at most want_kib */
static int pool_take(void) {
  if (pool)
    return 0;
  u32 room = QueryMaxFreeMemSize();
  room = room > (160u << 10) ? (room - (160u << 10)) & ~1023u : 0;
  u32 n = (want_kib << 10) < room ? want_kib << 10 : room;
  if (n < (64u << 10) || !(pool = AllocSysMemory(0, n, 0))) {
    printf("mxswap: no memory for a queue (%u KiB free)\n", (unsigned)(QueryMaxFreeMemSize() >> 10));
    return -1;
  }
  pool_bytes = n;
  printf("mxswap: a queue of %u KiB (%u KiB free after it)\n", (unsigned)(n >> 10),
         (unsigned)(QueryMaxFreeMemSize() >> 10));
  return 0;
}
static int lk, fmutex, wsem, idle;
static struct mxswap_stat st;
/* the IOP's clock (the bus clock, 36.864 MHz), for STAT's times */
static u32 now(void) {
  iop_sys_clock_t c;
  GetSystemTime(&c);
  return c.lo;
}
static u32 us(u32 ticks) { return ticks / 36864u * 1000u + ticks % 36864u * 1000u / 36864u; }

static u32 rpc_in[MXSWAP_BUF / 4] __attribute__((aligned(16)));
static u32 rpc_out[MXSWAP_BUF / 4] __attribute__((aligned(16)));

static int mksema(int init, int max) {
  iop_sema_t s = {0, 0, init, max};
  return CreateSema(&s);
}

/* a page's bytes as the drive takes them: whole sectors, from an address the
   USB driver hands on as it is (one that is not a multiple of 4 went a
   sector per command through a copy: 124 ms for a page, not 40) */
#define SECTOR 512u
static u32 sectors(u32 bytes) { return (bytes + SECTOR - 1) & ~(SECTOR - 1); }

/* n bytes of the ring (rounded up to sectors, so the next one is aligned
   too): their offset, or -1 (no room now). lk held. */
static int ring_take(u32 n) {
  n = sectors(n);
  if (!count)
    head = tail = 0;
  u32 off;
  if (head >= tail) {
    if (pool_bytes - head >= n)
      off = head;
    else if (tail > n)
      off = 0;
    else
      return -1;
  } else if (tail - head > n)
    off = head;
  else
    return -1;
  head = off + n;
  return (int)off;
}

/* into the queue: 0, or MXSWAP_FULL (no room now). It does not wait: the
   RPC thread serves the gets too. */
static int put(u32 slot, u32 bytes, const u8 *data) {
  WaitSema(lk);
  int off = count < NENT ? ring_take(bytes) : -1;
  if (off < 0) {
    st.full++;
    SignalSema(lk);
    return MXSWAP_FULL;
  }
  /* older copies of the slot not being written: not written at all */
  for (u32 i = 0, k = qhead; i < count; i++, k = (k + 1) % NENT)
    if (ent[k].slot == slot && ent[k].state == 1)
      ent[k].state = 3;
  struct ent *e = &ent[qtail];
  qtail = (qtail + 1) % NENT;
  count++;
  memcpy(pool + off, data, bytes);
  e->slot = slot, e->bytes = bytes, e->off = (u32)off, e->state = 1;
  st.puts++;
  SignalSema(lk);
  SignalSema(wsem);
  return 0;
}

/* the newest copy of slot in the queue to out: 1, or 0 (not there) */
static int from_queue(u32 slot, u32 bytes, u8 *out) {
  int got = 0;
  WaitSema(lk);
  for (u32 i = count; i-- > 0 && !got;) {
    struct ent *e = &ent[(qhead + i) % NENT];
    if (e->slot == slot && (e->state == 1 || e->state == 2) && e->bytes == bytes) {
      memcpy(out, pool + e->off, bytes);
      got = 1;
    }
  }
  SignalSema(lk);
  return got;
}

#define STACK 0x2000 /* 2 KiB used at most, measured on a USB drive (exFAT) */

static void writer(void *arg) {
  (void)arg;
  for (;;) {
    WaitSema(wsem);
    WaitSema(lk);
    if (!count) {
      SignalSema(lk);
      continue;
    }
    struct ent *e = &ent[qhead];
    int do_write = e->state == 1;
    if (do_write)
      e->state = 2;
    SignalSema(lk);
    if (do_write) {
      /* in blocks: the file is let go between them, so a read (the RPC
         thread, higher priority) waits at most for one block */
      int at = (int)(e->slot * MXSWAP_PAGE), s1 = 0, r = 0;
      for (u32 done = 0, all = sectors(e->bytes); done < all && r >= 0;) {
        u32 n = all - done < block ? all - done : block;
        WaitSema(fmutex);
        s1 = fd < 0 ? -1 : lseek(fd, at + (int)done, 0);
        r = s1 == at + (int)done ? write(fd, pool + e->off + done, (int)n) : -1;
        SignalSema(fmutex);
        if (r != (int)n)
          r = -1;
        else
          done += n;
      }
      if (r < 0) {
        st.write_errors++;
        printf("mxswap: write of slot %u (%u bytes at %d): seek %d, write %d\n", (unsigned)e->slot,
               (unsigned)e->bytes, at, s1, r);
      } else
        st.writes++;
    }
    WaitSema(lk);
    tail = e->off + sectors(e->bytes);
    e->state = 0;
    qhead = (qhead + 1) % NENT;
    count--;
    int empty = !count;
    SignalSema(lk);
    if (empty)
      SignalSema(idle);
  }
}

/* every queued page written */
static void flush(void) {
  for (;;) {
    WaitSema(lk);
    int empty = !count;
    SignalSema(lk);
    if (empty)
      return;
    WaitSema(idle);
  }
}

static void *rpc(int fno, void *data, int size) {
  u32 *a = data;
  (void)size;
  rpc_out[0] = 0;
  switch (fno) {
  case MXSWAP_OPEN:
    if (pool_take() < 0) {
      rpc_out[0] = (u32)-12;
      break;
    }
    flush();
    if (fd >= 0)
      close(fd);
    fd = open((char *)(a + 4), 3 /* O_RDWR */);
    rpc_out[0] = fd < 0 ? (u32)fd : 0;
    if (fd >= 0 && a[0]) {
      /* as long as the swap file is to be: a byte at its end (a seek past
         the end, then writes, went wrong on FAT: reads of 0xff) */
      int size = lseek(fd, 0, 2), s1 = size, w = 1;
      static u8 zero;
      if (size >= 0 && (u32)size < a[0]) {
        s1 = lseek(fd, (int)a[0] - 1, 0);
        w = s1 == (int)a[0] - 1 ? write(fd, &zero, 1) : -1;
      }
      printf("mxswap: %s: %d bytes, %u wanted: seek %d, write %d\n", (char *)(a + 4), size, (unsigned)a[0], s1, w);
      if (size < 0 || w != 1)
        rpc_out[0] = (u32)-5;
    }
    break;
  case MXSWAP_PUT:
    if (fd < 0 || a[1] > MXSWAP_PAGE || !a[1])
      rpc_out[0] = (u32)-22;
    else
      rpc_out[0] = (u32)put(a[0], a[1], (u8 *)(a + MXSWAP_HEAD / 4));
    break;
  case MXSWAP_ROOM: {
    /* as a put's, taken back */
    WaitSema(lk);
    u32 h = head, l = tail;
    rpc_out[0] = count < NENT && ring_take(a[0]) >= 0;
    if (count)
      head = h, tail = l;
    else
      head = tail = 0;
    SignalSema(lk);
    break;
  }
  case MXSWAP_GET: {
    u32 slot = a[0], bytes = a[1];
    st.gets++;
    if (bytes > MXSWAP_PAGE || !bytes) {
      rpc_out[0] = (u32)-22;
      break;
    }
    u8 *out = (u8 *)(rpc_out + MXSWAP_HEAD / 4);
    if (from_queue(slot, bytes, out)) {
      st.from_queue++;
      rpc_out[1] = 1;
      break;
    }
    rpc_out[1] = 0;
    u32 t0 = now();
    WaitSema(fmutex);
    u32 t1 = now();
    int at = (int)(slot * MXSWAP_PAGE);
    int s1 = fd < 0 ? -1 : lseek(fd, at, 0), r = -1;
    u32 t2 = now();
    if (s1 == at)
      r = read(fd, out, (int)sectors(bytes));
    u32 t3 = now();
    SignalSema(fmutex);
    st.get_wait_us += us(t1 - t0), st.get_seek_us += us(t2 - t1), st.get_read_us += us(t3 - t2);
    if (r != (int)sectors(bytes)) {
      rpc_out[0] = (u32)(r < 0 ? r : -5);
      printf("mxswap: read of slot %u (%u bytes at %d): seek %d, read %d\n", (unsigned)slot, (unsigned)bytes, at,
             s1, r);
    }
    break;
  }
  case MXSWAP_FLUSH:
    flush();
    break;
  case MXSWAP_CLOSE:
    flush();
    if (fd >= 0)
      close(fd);
    fd = -1;
    break;
  case MXSWAP_STAT:
    WaitSema(lk);
    st.queued = count, st.pool_bytes = pool_bytes;
    st.queue_bytes = !count ? 0 : head > tail ? head - tail : pool_bytes - tail + head;
    memcpy(rpc_out, &st, sizeof st);
    SignalSema(lk);
    break;
  case MXSWAP_BLOCK:
    if (a[0] && !(a[0] & 511) && a[0] <= MXSWAP_PAGE)
      block = a[0];
    rpc_out[0] = block;
    break;
  default:
    rpc_out[0] = (u32)-22;
  }
  return rpc_out;
}

static void server(void *arg) {
  static SifRpcDataQueue_t queue;
  static SifRpcServerData_t sd;
  (void)arg;
  sceSifSetRpcQueue(&queue, GetThreadId());
  sceSifRegisterRpc(&sd, MXSWAP_SID, rpc, rpc_in, 0, 0, &queue);
  sceSifRpcLoop(&queue);
}

int _start(int argc, char **argv) {
  u32 kib = 512;
  for (int i = 1; i < argc && i < 3; i++) {
    u32 v = 0;
    for (const char *p = argv[i]; *p >= '0' && *p <= '9'; p++)
      v = v * 10 + (u32)(*p - '0');
    if (i == 1 && v)
      kib = v;
    if (i == 2 && v)
      block = v << 10;
  }
  want_kib = kib;
  lk = mksema(1, 1), fmutex = mksema(1, 1), wsem = mksema(0, NENT + 1);
  idle = mksema(0, 1000);
  if (lk < 0 || fmutex < 0 || wsem < 0 || idle < 0) {
    printf("mxswap: no semaphores\n");
    return MODULE_NO_RESIDENT_END;
  }
  /* the RPC thread (reads) above the network and the drivers' callers, the
     writer far below */
  /* both do file calls (ioman, the file system, the host link): deep stacks */
  iop_thread_t ts = {TH_C, 0, server, STACK, 0x28}, tw = {TH_C, 0, writer, STACK, 0x70};
  int a = CreateThread(&ts), b = CreateThread(&tw);
  if (a <= 0 || b <= 0)
    return MODULE_NO_RESIDENT_END;
  sceSifInitRpc(0);
  StartThread(a, 0);
  StartThread(b, 0);
  printf("mxswap: a queue of up to %u KiB at the first OPEN, writes in blocks of %u KiB\n", (unsigned)kib,
         (unsigned)(block >> 10));
  return MODULE_RESIDENT_END;
}
