/* mxata: the expansion bay's ATA interface (the internal HDD) on mxdev9, as a
   block device for bdm: docs/iop-drivers.md.

   One disk (device 0), UDMA. A transfer goes to the DMA channel in pieces of
   what the SPEED's buffer holds at that moment (at most 31 sectors), each one
   request in mxdev9's queue, so frames get the channel in between. The next
   piece is queued from the interrupts (the last one's DMA end, or the buffer's
   ready interrupt): the thread sleeps until the command ends. The register
   sequences follow ps2sdk's atad (iop/dev9/atad, Academic Free License 2.0:
   ../LICENSE). */
#include "irx_imports.h"
#include <atahw.h>
#include <errno.h>
#include <speedregs.h>

IRX_ID("mxata", 1, 1);

#define ATA ((ata_hwport_t *)ATA_DEV9_HDD_BASE)
#define SPD_R_DBUF_STAT 0x38 /* sectors the buffer has (read) or takes (write) */
#define SPD_R_GAMESTAR 0x20

#define E_TIMEOUT -1
#define E_IO -2
#define E_ICRC -3
#define E_NODEV -4

#define DIR_READ 0
#define DIR_WRITE 1

/* a command's phases, for the interrupts */
enum { P_NODATA, P_DATA, P_END };

static int done_sema, lock_sema;
static volatile int finished, result;
static volatile int phase;
static volatile int dma_out; /* a piece is in mxdev9's queue */
static u8 *xbuf;
static u32 left, piece;
static int xdir;
static struct mxdev9_dma dma;

static int gamestar; /* a compatible adaptor (not Sony's): ps2sdk's workaround */
static int lba48;
static u64 total;
static struct block_device bd;

/* commands, sectors, DMA pieces, waits for the buffer, bounced sectors
   (unaligned buffers), errors, timeouts */
static u32 st[7];

/* ---- the interrupts: the DMA's end, the ATA bits, the alarm */

static void finish(int r) {
  if (finished)
    return;
  finished = 1;
  result = r;
  mxdev9_intr_off(SPD_INTR_ATA);
  iSignalSema(done_sema);
}

static void end_wait(void) {
  USE_SPD_REGS;
  phase = P_END;
  if (SPD_REG16(SPD_R_INTR_STAT) & SPD_INTR_ATA0)
    finish(0);
  else
    mxdev9_intr_on(SPD_INTR_ATA0);
}

/* interrupts off: queue what the buffer has, or wait for it */
static void pump(void) {
  USE_SPD_REGS;
  u32 n = SPD_REG16(SPD_R_DBUF_STAT) & 0x1f;
  if (!n) {
    st[3]++;
    mxdev9_intr_on(SPD_INTR_ATA);
    return;
  }
  if (n > left)
    n = left;
  piece = n;
  dma.buf = xbuf;
  dma.blocks = (u16)(n * 4); /* 128-byte blocks */
  dma.block_words = 32;
  dma.device = 0;
  dma.to_mem = xdir == DIR_READ;
  dma_out = 1;
  st[2]++;
  mxdev9_dma(&dma);
}

static void dma_start(struct mxdev9_dma *d) {
  USE_SPD_REGS;
  (void)d;
  if (!gamestar)
    SPD_REG16(SPD_R_XFR_CTRL) |= 0x80;
}

static void dma_done(struct mxdev9_dma *d) {
  USE_SPD_REGS;
  (void)d;
  if (!gamestar)
    SPD_REG16(SPD_R_XFR_CTRL) &= ~0x80;
  dma_out = 0;
  xbuf += piece * 512;
  left -= piece;
  if (finished)
    return;
  if (left)
    pump();
  else
    end_wait();
}

static void ata_intr(void *arg, u32 bits) {
  (void)arg;
  mxdev9_intr_off(SPD_INTR_ATA);
  if (finished)
    return;
  if (phase != P_DATA) {
    finish(0);
    return;
  }
  if (bits & SPD_INTR_ATA1) {
    if (!dma_out)
      pump();
    return;
  }
  /* the command ended before its data */
  if (ATA->r_control & ATA_STAT_ERR)
    finish(ATA->r_error & ATA_ERR_ICRC ? E_ICRC : E_IO);
  else
    mxdev9_intr_on(SPD_INTR_ATA1);
}

static unsigned int alarm_cb(void *arg) {
  (void)arg;
  st[6]++;
  finish(E_TIMEOUT);
  return 0;
}

/* ---- commands, from a thread */

/* until the bits clear: sleeps longer the longer it takes, about 44 s in all */
static int wait_busy(int bits) {
  static const u32 delay[] = {0, 100, 1000, 10000, 100000};
  for (int i = 0; i < 80; i++) {
    if (!(ATA->r_control & bits))
      return 0;
    if (i >= 10)
      DelayThread(i / 10 < 5 ? delay[i / 10] : 1000000);
  }
  printf("mxata: busy (%02x)\n", ATA->r_control & 0xff);
  return E_TIMEOUT;
}

/* the registers and the command; intr: the device interrupts at its end */
static int issue(u8 command, u16 feature, u16 nsect, u64 lba, int ext, int intr) {
  int r = wait_busy(ATA_STAT_BUSY | ATA_STAT_DRQ);
  if (r < 0)
    return r;
  ATA->r_control = intr ? 0 : 2; /* nIEN */
  if (ext) {
    ATA->r_feature = (feature >> 8) & 0xff;
    ATA->r_nsector = (nsect >> 8) & 0xff;
    ATA->r_sector = (lba >> 24) & 0xff;
    ATA->r_lcyl = (lba >> 32) & 0xff;
    ATA->r_hcyl = (lba >> 40) & 0xff;
  }
  ATA->r_feature = feature & 0xff;
  ATA->r_nsector = nsect & 0xff;
  ATA->r_sector = lba & 0xff;
  ATA->r_lcyl = (lba >> 8) & 0xff;
  ATA->r_hcyl = (lba >> 16) & 0xff;
  ATA->r_select = ATA_SEL_LBA | (ext ? 0 : (lba >> 24) & 0xf);
  ATA->r_command = command;
  return 0;
}

/* the command's status at its end */
static int status(int r) {
  if (r)
    return r;
  if (ATA->r_status & ATA_STAT_BUSY)
    r = wait_busy(ATA_STAT_BUSY);
  u16 s = ATA->r_status; /* also clears the device's interrupt */
  if (!r && s & ATA_STAT_ERR)
    r = ATA->r_error & ATA_ERR_ICRC ? E_ICRC : E_IO;
  return r;
}

static void begin(int ph) {
  int st_;
  CpuSuspendIntr(&st_);
  finished = 0;
  result = 0;
  phase = ph;
  CpuResumeIntr(st_);
}

/* sleep until finish, at most 30 s */
static int wait_end(void) {
  WaitSema(done_sema);
  CancelAlarm(alarm_cb, 0);
  if (result == E_TIMEOUT) {
    /* a queued piece of the buffer ends by itself: wait for it, as its request is ours */
    for (int i = 0; dma_out && i < 1000; i++)
      DelayThread(1000);
  }
  return result;
}

static iop_sys_clock_t timeout;

static int nodata(u8 command, u16 feature, u16 nsect) {
  st[0]++;
  begin(P_NODATA);
  mxdev9_intr_on(SPD_INTR_ATA0);
  int r = issue(command, feature, nsect, 0, 0, 1);
  if (r < 0) {
    mxdev9_intr_off(SPD_INTR_ATA);
    return r;
  }
  SetAlarm(&timeout, alarm_cb, 0);
  return status(wait_end());
}

/* IDENTIFY DEVICE by PIO, at the start only */
static int identify(u16 *id) {
  int r = issue(ATA_C_IDENTIFY_DEVICE, 0, 0, 0, 0, 0);
  if (r < 0 || (r = wait_busy(ATA_STAT_BUSY)) < 0)
    return r;
  u16 s = ATA->r_status;
  if (s & ATA_STAT_ERR || !(s & ATA_STAT_DRQ))
    return E_IO;
  for (int i = 0; i < 256; i++)
    id[i] = ATA->r_data;
  return status(0);
}

static void set_dir(int dir) {
  USE_SPD_REGS;
  SPD_REG16(SPD_R_DBUF_STAT) = 3;
  SPD_REG16(SPD_R_IF_CTRL) = (SPD_REG16(SPD_R_IF_CTRL) & 1) | (dir == DIR_WRITE ? 0x4c : 0x4e);
  SPD_REG16(SPD_R_XFR_CTRL) = dir | (gamestar ? 0x86 : 0x6);
}

/* one DMA command: buf 4-byte aligned */
static int dma_cmd(u8 *buf, u64 lba, u32 n, int dir) {
  USE_SPD_REGS;
  int ext = lba48 && (lba + n > 0x10000000 || n > 256);
  u8 command = dir == DIR_WRITE ? (ext ? ATA_C_WRITE_DMA_EXT : ATA_C_WRITE_DMA)
                                : (ext ? ATA_C_READ_DMA_EXT : ATA_C_READ_DMA);
  int r = 0;
  for (int tries = 3; tries > 0; tries--) {
    st[0]++;
    xbuf = buf;
    left = n;
    xdir = dir;
    begin(P_DATA);
    if (gamestar)
      set_dir(dir);
    r = issue(command, 0, (u16)n, lba, ext, 1);
    if (r < 0)
      break;
    if (!gamestar)
      set_dir(dir);
    SetAlarm(&timeout, alarm_cb, 0);
    int s;
    CpuSuspendIntr(&s);
    pump();
    CpuResumeIntr(s);
    r = status(wait_end());
    SPD_REG16(SPD_R_IF_CTRL) &= ~SPD_IF_DMA_ENABLE;
    if (r != E_ICRC)
      break;
  }
  if (r)
    st[5]++;
  return r;
}

static int xfer(u8 *buf, u64 lba, u32 n, int dir) {
  static u8 bounce[4096] __attribute__((aligned(16)));
  if (!total)
    return E_NODEV;
  if (lba >= total || n > total - lba)
    return E_IO;
  WaitSema(lock_sema);
  mxdev9_led(1);
  int r = 0;
  u32 at = (u32)lba;
  st[1] += n;
  if (((u32)buf & 3) == 0) {
    u32 max = lba48 ? 0xffff : 256;
    while (!r && n) {
      u32 k = n > max ? max : n;
      r = dma_cmd(buf, lba, k, dir);
      buf += k * 512, lba += k, n -= k;
    }
  } else
    while (!r && n) {
      u32 k = n > sizeof bounce / 512 ? sizeof bounce / 512 : n;
      st[4] += k;
      if (dir == DIR_WRITE)
        memcpy(bounce, buf, k * 512);
      r = dma_cmd(bounce, lba, k, dir);
      if (!r && dir == DIR_READ)
        memcpy(buf, bounce, k * 512);
      buf += k * 512, lba += k, n -= k;
    }
  mxdev9_led(0);
  SignalSema(lock_sema);
  if (r)
    printf("mxata: %s at %u: %d\n", dir == DIR_WRITE ? "write" : "read", at, r);
  return r;
}

static void flush(void) {
  WaitSema(lock_sema);
  int r = nodata(lba48 ? ATA_C_FLUSH_CACHE_EXT : ATA_C_FLUSH_CACHE, 0, 0);
  SignalSema(lock_sema);
  if (r)
    printf("mxata: flush: %d\n", r);
}

static void standby(void) {
  flush();
  WaitSema(lock_sema);
  nodata(ATA_C_STANDBY_IMMEDIATE, 0, 0);
  SignalSema(lock_sema);
}

/* ---- bdm */

static int bd_read(struct block_device *b, u64 sector, void *buf, u16 count) {
  (void)b;
  return xfer(buf, sector, count, DIR_READ) ? -EIO : count;
}

static int bd_write(struct block_device *b, u64 sector, const void *buf, u16 count) {
  (void)b;
  return xfer((u8 *)buf, sector, count, DIR_WRITE) ? -EIO : count;
}

static void bd_flush(struct block_device *b) {
  (void)b;
  flush();
}

static int bd_stop(struct block_device *b) {
  (void)b;
  standby();
  return 0;
}

/* ---- the start */

static int reset(void) {
  USE_SPD_REGS;
  SPD_REG16(SPD_R_IF_CTRL) = SPD_IF_ATA_RESET;
  DelayThread(100);
  SPD_REG16(SPD_R_IF_CTRL) = 0;
  SPD_REG16(SPD_R_IF_CTRL) = 0x48;
  DelayThread(3000);
  int r = wait_busy(ATA_STAT_BUSY);
  if (r < 0)
    return r;
  ATA->r_control = 6; /* SRST, nIEN */
  DelayThread(100);
  ATA->r_control = 2;
  DelayThread(3000);
  if ((r = wait_busy(ATA_STAT_BUSY)) < 0)
    return r;
  /* device 0 */
  if ((ATA->r_select >> 4) & 1) {
    ATA->r_select = 0;
    (void)ATA->r_control;
    (void)ATA->r_control;
    if ((r = wait_busy(ATA_STAT_BUSY | ATA_STAT_DRQ)) < 0)
      return r;
  }
  /* its signature: 1, 1 and an ATA (not ATAPI) device */
  if (ATA->r_control & 0x88 || (ATA->r_nsector & 0xff) != 1 || (ATA->r_sector & 0xff) != 1 ||
      (ATA->r_lcyl & 0xff) || (ATA->r_hcyl & 0xff))
    return E_NODEV;
  ATA->r_lcyl = 0x55;
  ATA->r_hcyl = 0xaa;
  if ((ATA->r_lcyl & 0xff) != 0x55 || (ATA->r_hcyl & 0xff) != 0xaa)
    return E_NODEV;
  return 0;
}

static void udma_mode(int mode) {
  USE_SPD_REGS;
  static const u16 v[] = {0xa7, 0x85, 0x63, 0x62, 0x61};
  SPD_REG16(SPD_R_UDMA_MODE) = v[mode];
  SPD_REG16(SPD_R_IF_CTRL) |= 0x49;
}

static void init(void *arg) {
  USE_SPD_REGS;
  static u16 id[256];
  (void)arg;
  int r = reset();
  if (r < 0) {
    printf("mxata: no disk (%d)\n", r);
    ExitDeleteThread();
    return;
  }
  SPD_REG16(SPD_R_PIO_MODE) = 0x92; /* PIO mode 0 */
  if ((r = identify(id)) < 0) {
    printf("mxata: IDENTIFY: %d\n", r);
    ExitDeleteThread();
    return;
  }
  lba48 = (id[ATA_ID_COMMAND_SETS_SUPPORTED] & 0x400) != 0;
  if (lba48)
    total = (u64)id[ATA_ID_48BIT_SECTOTAL_HI] << 32 | (u32)id[ATA_ID_48BIT_SECTOTAL_MI] << 16 |
            id[ATA_ID_48BIT_SECTOTAL_LO];
  else
    total = (u32)id[ATA_ID_SECTOTAL_HI] << 16 | id[ATA_ID_SECTOTAL_LO];
  int mode = 0;
  for (int m = 4; m > 0; m--)
    if (id[ATA_ID_UDMA_CONTROL] & (1u << m)) {
      mode = m;
      break;
    }
  WaitSema(lock_sema);
  r = nodata(ATA_C_SET_FEATURES, 3, (u16)(0x40 | mode));
  if (!r)
    udma_mode(mode);
  nodata(ATA_C_IDLE, 0, 0xff); /* spins down after 21 min unused */
  SignalSema(lock_sema);
  if (r) {
    printf("mxata: UDMA %d: %d\n", mode, r);
    total = 0;
    ExitDeleteThread();
    return;
  }
  char model[41];
  for (int i = 0; i < 20; i++) {
    model[2 * i] = (char)(id[ATA_ID_MODEL_NUMBER_00 + i] >> 8);
    model[2 * i + 1] = (char)id[ATA_ID_MODEL_NUMBER_00 + i];
  }
  int len = 40;
  while (len && model[len - 1] == ' ')
    len--;
  model[len] = 0;
  printf("mxata: %s, %u MiB, UDMA %d%s%s\n", model, (u32)(total >> 11), mode,
         lba48 ? ", LBA48" : "", gamestar ? ", compatible adaptor" : "");
  bd.sectorCount = total;
  bdm_connect_bd(&bd);
  ExitDeleteThread();
}

static void off(void) {
  if (total)
    standby();
}

int _start(int argc, char **argv) {
  USE_SPD_REGS;
  (void)argc, (void)argv;
  if (!(mxdev9_caps() & SPD_CAPS_ATA) || !(SPD_REG16(SPD_R_REV_8) & 0x02)) {
    printf("mxata: no HDD connector\n");
    return MODULE_NO_RESIDENT_END;
  }
  if (mxdev9_caps() & SPD_CAPS_DVR) {
    printf("mxata: PSX (DVR) not supported\n");
    return MODULE_NO_RESIDENT_END;
  }
  /* Sony's adaptor reads 1 here; some others read 1 in the low byte only */
  gamestar = SPD_REG16(SPD_R_GAMESTAR) != 1;

  iop_sema_t s = {.attr = 0, .option = 0, .initial = 0, .max = 1};
  done_sema = CreateSema(&s);
  s.initial = 1;
  lock_sema = CreateSema(&s);
  USec2SysClock(30000000, &timeout);
  dma.start = dma_start;
  dma.done = dma_done;
  if (done_sema < 0 || lock_sema < 0 || mxdev9_intr(SPD_INTR_ATA, ata_intr, 0) < 0)
    return MODULE_NO_RESIDENT_END;
  mxdev9_on_off(off);

  bd.name = "ata";
  bd.path = "ata";
  bd.sectorSize = 512;
  bd.read = bd_read;
  bd.write = bd_write;
  bd.flush = bd_flush;
  bd.stop = bd_stop;

  iop_thread_t t = {.attr = TH_C, .thread = init, .stacksize = 0x1000, .priority = 40};
  int th = CreateThread(&t);
  if (th < 0 || StartThread(th, 0) < 0)
    return MODULE_NO_RESIDENT_END;
  return MODULE_RESIDENT_END;
}
