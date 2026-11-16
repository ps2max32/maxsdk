/* mxio: the file service (common/include/mxio.h). */
#include <errno.h>
#include <irx.h>
#include <intrman.h>
#include <ioman.h>
#include <loadcore.h>
#include <sifcmd.h>
#include <sifman.h>
#include <sysclib.h>
#include <thbase.h>
#include <timrman.h>
#include <mxfast.h>
#include <mxio.h>
#include <mxmc.h>
#include <io_common.h>
#include <sifrpc.h>
#include <stdio.h>

IRX_ID("mxio", 1, 0);

/* Of a read from a driver. A read is one command on USB, where each costs about
   1 ms: 870 KiB/s at 16 KiB, 922 at 64 KiB (probe/iop/fileraw.c). */
#define PIECE 65536u

static struct {
  int used, host, h;
  u32 pos;     /* of a driver's file: where it is now */
  char dev[12]; /* its device, "usb0:" (MXIO_WATCH's files) */
} files[MXIO_FILES];

static u32 rpc_in[MXIO_BUFFER / 4] __attribute__((aligned(16)));
static s32 rpc_out[MXIO_ANSWER] __attribute__((aligned(16)));
static u8 piece[2][PIECE] __attribute__((aligned(16)));
static int piece_dma[2];

/* The fast path, if mxfast is loaded: it is found when asked for, so that mxio
   also works without a network */
static void **fast(void) {
  static iop_library_t lib;
  static void **table;
  if (!table) {
    memset(&lib, 0, sizeof lib);
    lib.version = 0x100;
    memcpy(lib.name, "mxfast\0", 8);
    table = QueryLibraryEntryTable(&lib);
  }
  return table;
}
#define FAST(type, i) ((mxfast_##type##_fn *)fast()[4 + (i)])

static int to_ee(void *src, u32 ee, u32 size) {
  SifDmaTransfer_t d;
  int st, id;
  d.src = src, d.dest = (void *)ee, d.size = (size + 15) & ~15u, d.attr = 0;
  do {
    CpuSuspendIntr(&st);
    id = (int)sceSifSetDma(&d, 1);
    CpuResumeIntr(st);
  } while (!id);
  return id;
}

static void wait_dma(int id) {
  while (id && sceSifDmaStat(id) >= 0)
    DelayThread(50);
}

static int do_open(int flags, const char *path) {
  int fd;
  for (fd = 0; fd < MXIO_FILES && files[fd].used; fd++)
    ;
  if (fd == MXIO_FILES)
    return -24;
  files[fd].pos = 0;
  files[fd].host = 0;
  if (!memcmp(path, "host:", 5) && fast()) {
    int mode = flags & 2 ? (flags & FIO_O_TRUNC ? 3 : 1) : 0;
    int h = FAST(open, 0)(path + 5, mode);
    if (h >= 0) {
      files[fd].used = 1, files[fd].host = 1, files[fd].h = h;
      rpc_out[1] = MXFAST_FRAME, rpc_out[2] = MXFAST_CHUNK;
      return fd;
    }
    /* no host, or it went away: the driver may still have it */
  }
  int h = open(path, flags);
  if (h < 0)
    return h;
  files[fd].used = 1, files[fd].h = h;
  u32 n = 0;
  while (n < sizeof files[fd].dev - 1 && path[n] && path[n] != ':')
    n++;
  memcpy(files[fd].dev, path, n);
  files[fd].dev[n] = ':', files[fd].dev[n + 1 < sizeof files[fd].dev ? n + 1 : n] = 0;
  rpc_out[1] = MXIO_MAX_WRITE, rpc_out[2] = MXIO_MAX_WRITE;
  return fd;
}

static int seek_to(int fd, u32 pos) {
  if (files[fd].pos == pos)
    return 0;
  int r = lseek(files[fd].h, (int)pos, 0);
  if (r < 0)
    return r;
  files[fd].pos = pos;
  return 0;
}

/* A driver's read in pieces: the next one is read while DMA takes the last */
static int driver_read(int fd, u32 pos, u32 size, u32 ee) {
  int r = seek_to(fd, pos), i = 0;
  u32 done = 0;
  if (r < 0)
    return r;
  while (done < size) {
    u32 n = size - done > PIECE ? PIECE : size - done;
    wait_dma(piece_dma[i]);
    piece_dma[i] = 0;
    r = read(files[fd].h, piece[i], (int)n);
    if (r <= 0)
      break;
    piece_dma[i] = to_ee(piece[i], ee + done, (u32)r);
    done += (u32)r;
    if ((u32)r < n)
      break;
    i ^= 1;
  }
  wait_dma(piece_dma[0]);
  wait_dma(piece_dma[1]);
  piece_dma[0] = piece_dma[1] = 0;
  files[fd].pos = pos + done;
  return r < 0 ? r : (int)done;
}

static int driver_write(int fd, u32 pos, u32 size) {
  int r = seek_to(fd, pos);
  if (r < 0)
    return r;
  r = write(files[fd].h, (u8 *)rpc_in + MXIO_HEAD, (int)size);
  if (r > 0)
    files[fd].pos = pos + (u32)r;
  return r;
}

/* ---- the devices (MXIO_WATCH) ------------------------------------------- */

/* bdm's block devices (ps2sdk's bdm.h, by its export table: bdm is loaded
   after mxio, or not at all) */
struct block_device {
  void *priv;
  char *name;
  unsigned int devNr, parNr;
  unsigned char parId;
  unsigned int sectorSize;
  u64 sectorOffset, sectorCount;
  void *read, *write, *flush, *stop;
  char *path; /* the device's prefix: "usb" for usb0: */
};
static void **bdm(void) {
  static iop_library_t lib;
  static void **table;
  if (!table) {
    memset(&lib, 0, sizeof lib);
    lib.version = 0x101;
    memcpy(lib.name, "bdm\0\0\0\0\0", 8);
    table = QueryLibraryEntryTable(&lib);
  }
  return table;
}
/* cdvdman (the ROM's, in the IOP from its reset): the clock */
static void **cdvdman(void) {
  static iop_library_t lib;
  static void **table;
  if (!table) {
    memset(&lib, 0, sizeof lib);
    lib.version = 0x101;
    memcpy(lib.name, "cdvdman\0", 8);
    table = QueryLibraryEntryTable(&lib);
  }
  return table;
}
typedef int cd_clock_fn(void *clock);
#define CD_READ_CLOCK 24
#define CD_WRITE_CLOCK 25

typedef void bdm_cb_fn(int event);
typedef void bdm_register_fn(bdm_cb_fn *cb);
typedef void bdm_get_bd_fn(struct block_device **pbd, unsigned int count);

static struct mxio_devs devs __attribute__((aligned(64)));
static u32 devs_ee;
static int watch_tid;
static volatile int bdm_changed = 1;

static void bdm_event(int event) {
  (void)event;
  bdm_changed = 1;
  if (watch_tid > 0)
    WakeupThread(watch_tid);
}

/* 1: the device's root opens; 0: it does not; -1: an error that says nothing
   about the device being there (mcman: -EMFILE when its 3 open files are
   taken, -EIO from a transfer) */
static int open_root(const char *dev) {
  char p[16];
  u32 n = strlen(dev);
  memcpy(p, dev, n);
  p[n] = '/', p[n + 1] = 0;
  int fd = dopen(p, 0);
  if (fd >= 0) {
    dclose(fd);
    return 1;
  }
  /* mcman checks the card before it takes a file: no card is -ENXIO even
     with every file taken; an unformatted card is -EFORMAT */
  return fd == -ENXIO || fd == -ENODEV || fd == -EFORMAT ? 0 : -1;
}

static int present(const char *dev) {
  return open_root(dev) > 0;
}

/* a card slot: after an error that is not "no card", its last state */
static int card_present(int slot) {
  static int was[2];
  int r = open_root(slot ? "mc1:" : "mc0:");
  if (r >= 0)
    was[slot] = r;
  return was[slot];
}

static void add(struct mxio_devs *t, const char *name, int kind, int fs, u64 size) {
  if (t->n >= MXIO_DEVS)
    return;
  struct mxio_dev *d = &t->d[t->n++];
  memset(d, 0, sizeof *d);
  u32 n = strlen(name);
  memcpy(d->name, name, n < sizeof d->name ? n : sizeof d->name - 1);
  d->kind = (unsigned char)kind, d->present = 1, d->fs = (unsigned char)fs;
  d->size_lo = (u32)size, d->size_hi = (u32)(size >> 32);
}

static int kind_of(const char *path) {
  if (!strncmp(path, "ata", 3))
    return 4; /* MX_DEV_HDD */
  if (!strncmp(path, "mx4sio", 6) || !strncmp(path, "sdc", 3))
    return 3; /* MX_DEV_MX4SIO */
  return 2;   /* MX_DEV_USB */
}

/* MXOS's card driver (mxmc.irx, common/include/mxmc.h), when it is the one
   loaded: whether a card is there without taking one of its files, its size
   and free space */
static void **mxmc(void) {
  static iop_library_t lib;
  static void **table;
  if (!table) {
    memset(&lib, 0, sizeof lib);
    lib.version = 0x101;
    memcpy(lib.name, "mxmc\0\0\0\0", 8);
    table = QueryLibraryEntryTable(&lib);
  }
  return table;
}

/* the memory cards: through mxmc, else (the ROM's or ps2sdk's mcman) by
   opening their root */
static void cards(struct mxio_devs *t) {
  void **m = mxmc();
  for (int slot = 0; slot < 2; slot++) {
    const char *name = slot ? "mc1:" : "mc0:";
    if (!m) {
      if (card_present(slot))
        add(t, name, 1, 1, 8u << 20);
      continue;
    }
    struct mxmc_card c;
    if (((mxmc_card_fn *)m[MXMC_CARD])(slot, &c) != MXMC_READY)
      continue;
    add(t, name, 1, 1, c.size);
    t->d[t->n - 1].free_lo = c.free;
  }
}

/* what is there now, in t; the block devices only when bdm said so */
static struct mxio_devs scan_t, bdm_t;
static void scan(struct mxio_devs *t, int bdm_too) {
  memset(t, 0, sizeof *t);
  t->version = MXIO_DEVS_VERSION;
  cards(t);
  void **b = bdm();
  if (b && bdm_too) {
    memset(&bdm_t, 0, sizeof bdm_t);
    struct block_device *bd[16];
    memset(bd, 0, sizeof bd);
    ((bdm_get_bd_fn *)b[8])(bd, 16);
    /* each prefix: its units as fatfs numbers them (mounted ones, in the
       order they came); a disk with partitions mounts those */
    for (int i = 0; i < 16; i++) {
      if (!bd[i] || !bd[i]->path)
        continue;
      int seen = 0;
      for (int k = 0; k < i; k++)
        seen |= bd[k] && bd[k]->path && !strcmp(bd[k]->path, bd[i]->path);
      if (seen)
        continue;
      int parts = 0;
      for (int k = i; k < 16; k++)
        parts |= bd[k] && bd[k]->path && !strcmp(bd[k]->path, bd[i]->path) && bd[k]->parNr;
      int unit = 0;
      for (int k = i; k < 16 && unit < 4; k++) {
        if (!bd[k] || !bd[k]->path || strcmp(bd[k]->path, bd[i]->path) || (parts && !bd[k]->parNr))
          continue;
        char name[12];
        u32 n = strlen(bd[i]->path);
        if (n > 6)
          n = 6;
        memcpy(name, bd[i]->path, n);
        name[n] = (char)('0' + unit), name[n + 1] = ':', name[n + 2] = 0;
        if (present(name))
          add(&bdm_t, name, kind_of(bd[i]->path), 2, bd[k]->sectorCount * bd[k]->sectorSize);
        unit++;
      }
    }
  }
  for (u32 i = 0; i < bdm_t.n && t->n < MXIO_DEVS; i++)
    t->d[t->n++] = bdm_t.d[i];
}

/* the files open on each device: flags without a new gen */
static void count_files(struct mxio_devs *t) {
  for (u32 i = 0; i < t->n; i++) {
    struct mxio_dev *d = &t->d[i];
    d->files = 0;
    for (int f = 0; f < MXIO_FILES; f++)
      d->files += files[f].used && !files[f].host && !strcmp(files[f].dev, d->name);
    d->flags = d->files ? MXIO_DEV_BUSY : 0;
  }
}

static int same(const struct mxio_dev *a, const struct mxio_dev *b) {
  return !strcmp(a->name, b->name) && a->present == b->present && a->size_lo == b->size_lo &&
         a->size_hi == b->size_hi;
}

/* the new list against the one the EE has: gen up when a device came or
   went; one that went stays, not present, until the next change */
static void publish(struct mxio_devs *t) {
  int changed = t->n != devs.n;
  for (u32 i = 0; i < t->n && !changed; i++)
    changed = !same(&t->d[i], &devs.d[i]);
  if (changed) {
    /* those that went */
    for (u32 i = 0; i < devs.n; i++) {
      if (!devs.d[i].present)
        continue;
      int there = 0;
      for (u32 k = 0; k < t->n; k++)
        there |= !strcmp(t->d[k].name, devs.d[i].name);
      if (!there && t->n < MXIO_DEVS) {
        t->d[t->n] = devs.d[i];
        t->d[t->n++].present = 0;
      }
    }
    changed = t->n != devs.n;
    for (u32 i = 0; i < t->n && !changed; i++)
      changed = !same(&t->d[i], &devs.d[i]);
  }
  t->gen = devs.gen + (changed ? 1 : 0);
  count_files(t);
  int flags = 0;
  for (u32 i = 0; i < t->n && !flags && !changed; i++)
    flags = t->d[i].flags != devs.d[i].flags || t->d[i].free_lo != devs.d[i].free_lo;
  if (!changed && !flags)
    return;
  if (changed) {
    printf("mxio: devices (%u):", t->gen);
    for (u32 i = 0; i < t->n; i++)
      printf(" %s%s", t->d[i].name, t->d[i].present ? "" : "(gone)");
    printf("\n");
  }
  devs = *t;
  if (devs_ee)
    wait_dma(to_ee(&devs, devs_ee, sizeof devs));
}

static void watch(void *arg) {
  (void)arg;
  int hooked = 0;
  for (;;) {
    if (!hooked && bdm()) {
      ((bdm_register_fn *)bdm()[9])(bdm_event);
      hooked = 1, bdm_changed = 1;
    }
    int b = bdm_changed;
    bdm_changed = 0;
    if (b) /* a drive is mounted a moment after bdm calls */
      DelayThread(200 * 1000);
    scan(&scan_t, b);
    publish(&scan_t);
    /* the cards every second, or at once when bdm calls */
    iop_sys_clock_t c;
    USec2SysClock(1000 * 1000, &c);
    SetAlarm(&c, (unsigned int (*)(void *))iWakeupThread, (void *)watch_tid);
    SleepThread();
    CancelAlarm((unsigned int (*)(void *))iWakeupThread, (void *)watch_tid);
  }
}

static void watch_start(void) {
  if (watch_tid > 0)
    return;
  iop_thread_t t = {TH_C, 0, watch, 0x800, 0x60}; /* below the file service */
  watch_tid = CreateThread(&t);
  if (watch_tid > 0)
    StartThread(watch_tid, 0);
}

static void *rpc(int fno, void *data, int size) {
  u32 *a = data;
  int fd = (int)a[0];
  (void)size;
  if ((fno == MXIO_CLOSE || fno == MXIO_READ || fno == MXIO_WRITE || fno == MXIO_SIZE) &&
      (fd < 0 || fd >= MXIO_FILES || !files[fd].used)) {
    rpc_out[0] = -9;
    return rpc_out;
  }
  switch (fno) {
  case MXIO_OPEN:
    rpc_out[0] = do_open((int)a[0], (char *)(a + 1));
    break;
  case MXIO_CLOSE:
    rpc_out[0] = files[fd].host ? FAST(close, 1)(files[fd].h) : close(files[fd].h);
    files[fd].used = 0;
    break;
  case MXIO_READ:
    rpc_out[0] = files[fd].host ? FAST(read, 2)(files[fd].h, a[1], a[2], a[3])
                                : driver_read(fd, a[1], a[2], a[3]);
    break;
  case MXIO_WRITE:
    rpc_out[0] = files[fd].host
                     ? FAST(write, 3)(files[fd].h, a[1], a[2], (u8 *)rpc_in + MXIO_HEAD - MXFAST_DATA_AT)
                     : driver_write(fd, a[1], a[2]);
    break;
  case MXIO_SIZE:
    if (files[fd].host)
      rpc_out[0] = FAST(size, 4)(files[fd].h);
    else {
      rpc_out[0] = lseek(files[fd].h, 0, 2);
      lseek(files[fd].h, (int)files[fd].pos, 0);
    }
    break;
  case MXIO_CLOSE_ALL:
    for (fd = 0; fd < MXIO_FILES; fd++)
      if (files[fd].used) {
        if (files[fd].host)
          FAST(close, 1)(files[fd].h);
        else
          close(files[fd].h);
        files[fd].used = 0;
      }
    rpc_out[0] = 0;
    break;
  case MXIO_LOG:
    rpc_out[0] = fast() ? FAST(log, 5)((char *)a) : -6;
    break;
  case MXIO_STATS:
    if (fast())
      FAST(stats, 6)((int *)rpc_out);
    break;
  case MXIO_WATCH:
    devs_ee = a[0];
    /* what is there now (the cards: the drives come when bdm mounts them),
       so that the EE's first list is not empty */
    scan(&scan_t, 0);
    publish(&scan_t);
    devs.gen++; /* the EE's area: the list written to it now */
    if (devs_ee)
      wait_dma(to_ee(&devs, devs_ee, sizeof devs));
    watch_start();
    if (watch_tid > 0)
      WakeupThread(watch_tid);
    rpc_out[0] = MXIO_DEVS_VERSION;
    break;
  case MXIO_CLOCK:
  case MXIO_CLOCK_SET: {
    void **t = cdvdman();
    static u8 clock[8] __attribute__((aligned(4)));
    if (!t) {
      rpc_out[0] = -19;
      break;
    }
    if (fno == MXIO_CLOCK_SET)
      memcpy(clock, a, 8);
    rpc_out[0] = ((cd_clock_fn *)t[fno == MXIO_CLOCK ? CD_READ_CLOCK : CD_WRITE_CLOCK])(clock);
    memcpy(&rpc_out[1], clock, 8);
    break;
  }
  default:
    rpc_out[0] = -22;
  }
  return rpc_out;
}

static void thread(void *arg) {
  static SifRpcDataQueue_t queue;
  static SifRpcServerData_t server;
  (void)arg;
  sceSifSetRpcQueue(&queue, GetThreadId());
  sceSifRegisterRpc(&server, MXIO_SID, rpc, rpc_in, 0, 0, &queue);
  sceSifRpcLoop(&queue);
}

int _start(int argc, char **argv) {
  (void)argc, (void)argv;
  iop_thread_t t = {TH_C, 0, thread, 0x1000, 0x50}; /* below the network driver */
  sceSifInitRpc(0);
  int id = CreateThread(&t);
  if (id <= 0)
    return MODULE_NO_RESIDENT_END;
  StartThread(id, 0);
  return MODULE_RESIDENT_END;
}
