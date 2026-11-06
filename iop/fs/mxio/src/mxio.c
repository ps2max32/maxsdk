/* mxio: the file service (common/include/mxio.h). */
#include "irx_imports.h"
#include <mxfast.h>
#include <mxio.h>
#include <io_common.h>
#include <sifrpc.h>

IRX_ID("mxio", 1, 0);

#define PIECE 16384u /* of a read from a driver */

static struct {
  int used, host, h;
  u32 pos; /* of a driver's file: where it is now */
} files[MXIO_FILES];

static u32 rpc_in[MXIO_BUFFER / 4] __attribute__((aligned(16)));
static s32 rpc_out[MXIO_ANSWER] __attribute__((aligned(16)));
static u8 piece[2][PIECE] __attribute__((aligned(16)));
static int piece_dma[2];

/* The fast path, if mxnet is loaded: it is found when asked for, so that mxio
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
