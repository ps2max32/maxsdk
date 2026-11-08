/* How fast USB mass storage can read, without a file system: this module is a
   class driver of ps2sdk's usbd and sends READ(10) commands (Bulk-Only
   Transport) itself, in several ways. Only reads, except with "write" (below). Run by probe/p30_usbraw.c; results on the IOP's console.

   Ways to do a command:
     0 chain   CBW, wait; 4 KiB transfers, each started from the callback of the
               last (what usbmass_bd does); CSW, wait
     1 q4      CBW, wait; all data queued at once, 4 KiB each; CSW, wait
     2 q8      the same, 8 KiB each (a transfer may touch two 4 KiB pages)
     3 all4    CBW, data in 4 KiB and CSW all queued at once, one wait
     4 all8    the same, 8 KiB each

   With the argument "write" it times WRITE(10) instead, at the end of the
   device: each command first reads its sectors and writes the same bytes
   back, so the contents stay as they were (checked by a sum of the region
   before and after). With the argument "timing" it only times the packets of 64 KiB reads within
   USB frames instead (see timing()).

   The module stays resident (usbd keeps its functions); the program's end
   resets the IOP. */

#include <irx.h>
#include <intrman.h>
#include <loadcore.h>
#include <stdio.h>
#include <thbase.h>
#include <timrman.h>
#include <sysclib.h>
#include <sysmem.h>
#include <thsemap.h>
#include <usbd.h>
#include "usbd_macro.h"

IRX_ID("usbraw_probe", 1, 0);

#define KB 1024u
#define MODES 5
#define MOST_CMD (128 * KB)
#define TEST_BYTES (2048 * KB)
#define START_LBA 65536u
#define CHECK_LBA 32768u
#define CHECK_BYTES (256 * KB)
#define TICKS_MS 36864u /* of the timer: 36.864 MHz */
#define STUCK_MS 5000u

static const char *const mode_name[MODES] = {"chain", "q4", "q8", "all4", "all8"};

static int timer, sema, done;
static int ep_ctrl = -1, ep_in = -1, ep_out = -1; /* of the device under test */
static u8 config_id, intf_no, intf_alt;

#define DEVICES 2
static struct {
  int id, ctrl, in, out;
  u8 config, intf, alt;
} devs[DEVICES];
static volatile int dev_count;
static u8 *data;
static u8 cbw[32] __attribute__((aligned(16)));
static u8 csw[16] __attribute__((aligned(16)));
static u32 tag;

static volatile int pending, failed, stuck, finished;
static volatile u32 progress, moved, idle_count;

/* the chain of mode 0 */
static u8 *chain_at;
static u32 chain_left;

#define now() ((u32)GetTimerCounter(timer))

static void wake(void) { SignalSema(sema); }

static void done_cb(int result, int bytes, void *arg) {
  (void)arg;
  if (result != USB_RC_OK && !failed)
    failed = result;
  moved += (u32)bytes;
  if (--pending == 0)
    wake();
}

static void chain_cb(int result, int bytes, void *arg);

static int chain_next(void) {
  u32 n = chain_left > 4 * KB ? 4 * KB : chain_left;
  return sceUsbdBulkTransfer(ep_in, chain_at, n, chain_cb, 0);
}

static void chain_cb(int result, int bytes, void *arg) {
  (void)arg;
  moved += (u32)bytes;
  if (result == USB_RC_OK) {
    chain_left -= (u32)bytes, chain_at += bytes;
    if (chain_left) {
      int r = chain_next();
      if (r == USB_RC_OK)
        return;
      result = r;
    }
  }
  if (result != USB_RC_OK && !failed)
    failed = result;
  pending = 0;
  wake();
}

/* wait until pending is 0: 0 when all went well */
static int wait_all(void) {
  WaitSema(sema);
  if (stuck)
    return -1000;
  return failed ? -failed : 0;
}

static int start(int ep, void *at, u32 size) {
  pending++;
  int r = sceUsbdBulkTransfer(ep, at, size, done_cb, 0);
  if (r != USB_RC_OK)
    pending--;
  return r == USB_RC_OK ? 0 : -r;
}

static int one(int ep, void *at, u32 size) {
  int r = start(ep, at, size);
  return r ? r : wait_all();
}

static void make_cbw(const u8 *cmd, u32 cmd_len, u32 size, int in) {
  memset(cbw, 0, sizeof cbw);
  u32 w[3] = {0x43425355, ++tag, size};
  memcpy(cbw, w, 12);
  cbw[12] = in ? 0x80 : 0;
  cbw[14] = (u8)cmd_len;
  memcpy(cbw + 15, cmd, cmd_len);
}

static int check_csw(void) {
  u32 w[2];
  memcpy(w, csw, 8);
  if (w[0] != 0x53425355 || w[1] != tag)
    return -2000;
  return csw[12] ? -2100 - csw[12] : 0;
}

/* a small command, to set up the device */
static int small_cmd(const u8 *cmd, u32 cmd_len, void *to, u32 size) {
  int r;
  make_cbw(cmd, cmd_len, size, 1);
  failed = 0;
  if ((r = one(ep_out, cbw, 31)))
    return r;
  if (size && (r = one(ep_in, to, size)))
    return r;
  if ((r = one(ep_in, csw, 13)))
    return r;
  return check_csw();
}

static u32 phase[3]; /* ticks in the CBW, data and CSW of the last test */

/* one READ(10); the buffer is page aligned */
static int read_cmd(int mode, u32 lba, u32 sectors) {
  u8 cmd[10] = {0x28, 0, (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8), (u8)lba, 0,
                (u8)(sectors >> 8), (u8)sectors, 0};
  u32 size = sectors * 512, piece = (mode == 2 || mode == 4) ? 8 * KB : 4 * KB;
  int r;
  u32 t0, t1, t2;

  make_cbw(cmd, sizeof cmd, size, 1);
  failed = 0;
  progress = now();
  if (mode >= 3) {
    if ((r = start(ep_out, cbw, 31)))
      return r;
    for (u32 at = 0; at < size; at += piece)
      if ((r = start(ep_in, data + at, size - at > piece ? piece : size - at)))
        return r;
    if ((r = start(ep_in, csw, 13)))
      return r;
    if ((r = wait_all()))
      return r;
    return check_csw();
  }
  t0 = now();
  if ((r = one(ep_out, cbw, 31)))
    return r;
  t1 = now();
  if (mode == 0) {
    chain_at = data, chain_left = size, pending = 1;
    if ((r = chain_next()) != USB_RC_OK)
      return -r;
    r = wait_all();
  } else {
    r = 0;
    for (u32 at = 0; at < size && !r; at += piece)
      r = start(ep_in, data + at, size - at > piece ? piece : size - at);
    if (!r)
      r = wait_all();
  }
  if (r)
    return r;
  t2 = now();
  if ((r = one(ep_in, csw, 13)))
    return r;
  phase[0] += t1 - t0, phase[1] += t2 - t1, phase[2] += now() - t2;
  return check_csw();
}

static u32 idle_rate; /* counts a millisecond with nothing to do */

static int test(int mode, u32 cmd_bytes, u32 *lba) {
  u32 sectors = cmd_bytes / 512, cmds = TEST_BYTES / cmd_bytes;
  phase[0] = phase[1] = phase[2] = 0;
  u32 i0 = idle_count, t0 = now();
  for (u32 i = 0; i < cmds; i++, *lba += sectors) {
    int r = read_cmd(mode, *lba, sectors);
    if (r) {
      printf("usbraw: %s %u KiB: failed %d at command %u (lba %u)\n", mode_name[mode], cmd_bytes / KB,
             r, i, *lba);
      return r;
    }
  }
  u32 ms = (now() - t0) / TICKS_MS, idle = idle_count - i0;
  if (!ms)
    ms = 1;
  printf("usbraw: %-5s %3u KiB: %4u KiB/s, %6u us a command", mode_name[mode], cmd_bytes / KB,
         TEST_BYTES / KB * 1000u / ms, ms * 1000u / cmds);
  if (mode < 3)
    printf(" (cbw %u data %u csw %u)", phase[0] / cmds * 125 / 4608,
           phase[1] / cmds * 125 / 4608, phase[2] / cmds * 125 / 4608);
  printf(", IOP idle %u%%\n", idle_rate ? idle / ms * 100 / idle_rate : 0);
  return 0;
}

/* all ways must read the same */
static u32 sum_of(int mode) {
  u32 sum = 0, lba = CHECK_LBA;
  for (u32 at = 0; at < CHECK_BYTES; at += 64 * KB, lba += 128) {
    int r = read_cmd(mode, lba, 128);
    if (r) {
      printf("usbraw: check %s: failed %d\n", mode_name[mode], r);
      return 0;
    }
    for (u32 i = 0; i < 64 * KB / 4; i++)
      sum = sum * 31 + ((u32 *)data)[i];
  }
  return sum;
}

static int ctrl_wait(int r) {
  if (r != USB_RC_OK)
    return -r;
  return wait_all();
}

static void ctrl_cb(int result, int bytes, void *arg) {
  (void)bytes, (void)arg;
  failed = result;
  wake();
}

static void watchdog(void *arg) {
  (void)arg;
  while (!finished) {
    DelayThread(500000);
    if (progress && now() - progress > STUCK_MS * TICKS_MS) {
      printf("usbraw: stuck\n");
      stuck = 1;
      wake();
      return;
    }
  }
  ExitDeleteThread();
}

static void idle_thread(void *arg) {
  (void)arg;
  while (!finished)
    idle_count++;
  ExitDeleteThread();
}


/* When each 64-byte packet of a read arrives within its USB frame. The buffer
   is filled with a mark; with interrupts off the IOP watches the first word of
   each packet and notes HcFmRemaining (bit times left in the 1 ms frame, 12 MHz,
   counting down from 11999) and HcFmNumber when it changes. Shows whether the
   packets are spaced wider than the bus needs (about 630 bit times for 64
   bytes: 19 a frame) and how much of each frame is left unused. */
#define OHCI ((volatile u32 *)0xBF801600)
#define MARK 0x5A3C96E1u
#define TPKTS 1024
static u16 t_rem[TPKTS], t_fr[TPKTS];

static void timing_round(const char *what, u32 lba, int poll_gap) {
  u32 size = 64 * KB, n = 0;
  volatile u32 *w = (volatile u32 *)data;
  u8 cmd[10] = {0x28, 0, (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8), (u8)lba, 0, 0, 128, 0};
  int r, st;

  for (u32 i = 0; i < size / 4; i++)
    w[i] = MARK;
  make_cbw(cmd, sizeof cmd, size, 1);
  failed = 0;
  progress = now();
  if ((r = start(ep_out, cbw, 31)))
    goto out;
  for (u32 at = 0; at < size; at += 8 * KB)
    if ((r = start(ep_in, data + at, 8 * KB)))
      goto out;
  if ((r = start(ep_in, csw, 13)))
    goto out;
  CpuSuspendIntr(&st);
  u32 f0 = OHCI[15] & 0xffff;
  while (n < TPKTS && ((OHCI[15] - f0) & 0xffff) < 200) {
    if (w[n * 16] != MARK) {
      t_rem[n] = (u16)(OHCI[14] & 0x3fff);
      t_fr[n] = (u16)OHCI[15];
      n++;
    } else
      for (volatile int d = 0; d < poll_gap; d++)
        ;
  }
  CpuResumeIntr(st);
  r = wait_all();
  if (!r)
    r = check_csw();
out:
  if (r || n < TPKTS) {
    printf("usbraw: timing %s: result %d, %u packets seen\n", what, r, n);
    return;
  }
  /* full frames only: not the first and the last */
  u32 hist[24] = {0}, frames = 0, sp_n = 0, sp_sum = 0, sp_min = 99999, sp_max = 0;
  u32 first_sum = 0, first_min = 99999, first_max = 0, last_sum = 0, last_min = 99999, last_max = 0;
  u32 sp_hist[8] = {0}; /* <600 <650 <700 <750 <800 <850 <900 more */
  u32 i = 0;
  while (i < n && t_fr[i] == t_fr[0])
    i++;
  while (i < n) {
    u32 j = i;
    while (j < n && t_fr[j] == t_fr[i])
      j++;
    if (j >= n)
      break; /* the last frame */
    u32 c = j - i;
    hist[c < 23 ? c : 23]++;
    frames++;
    first_sum += t_rem[i], last_sum += t_rem[j - 1];
    if (t_rem[i] < first_min) first_min = t_rem[i];
    if (t_rem[i] > first_max) first_max = t_rem[i];
    if (t_rem[j - 1] < last_min) last_min = t_rem[j - 1];
    if (t_rem[j - 1] > last_max) last_max = t_rem[j - 1];
    for (u32 k = i + 1; k < j; k++) {
      u32 d = (u32)t_rem[k - 1] - t_rem[k];
      sp_sum += d, sp_n++;
      if (d < sp_min) sp_min = d;
      if (d > sp_max) sp_max = d;
      sp_hist[d < 600 ? 0 : d >= 900 ? 7 : (d - 550) / 50]++;
    }
    if (frames <= 2) {
      printf("usbraw: timing %s frame %u:", what, t_fr[i]);
      for (u32 k = i; k < j; k++)
        printf(" %u", t_rem[k]);
      printf("\n");
    }
    i = j;
  }
  if (!frames || !sp_n)
    return;
  printf("usbraw: timing %s: %u frames, packets a frame:", what, frames);
  for (int c = 0; c < 24; c++)
    if (hist[c])
      printf(" %ux%u", hist[c], c);
  printf("\nusbraw: timing %s: first packet at %u..%u (avg %u) left, last at %u..%u (avg %u)\n", what,
         first_min, first_max, first_sum / frames, last_min, last_max, last_sum / frames);
  printf("usbraw: timing %s: spacing %u..%u avg %u bit times; <600 %u <650 %u <700 %u <750 %u <800 %u "
         "<850 %u <900 %u more %u\n", what, sp_min, sp_max, sp_sum / sp_n, sp_hist[0], sp_hist[1],
         sp_hist[2], sp_hist[3], sp_hist[4], sp_hist[5], sp_hist[6], sp_hist[7]);
}

static void timing(void) {
  volatile u32 *dpcr = (volatile u32 *)0xBF8010F0, *dpcr2 = (volatile u32 *)0xBF801570;
  printf("usbraw: OHCI rev %x control %x fminterval %x periodicstart %x lsthreshold %x rhda %x\n",
         OHCI[0], OHCI[1], OHCI[13], OHCI[16], OHCI[17], OHCI[18]);
  printf("usbraw: DPCR %x DPCR2 %x 1680 %x\n", *dpcr, *dpcr2, *(volatile u32 *)0xBF801680);
  u32 lba = START_LBA;
  timing_round("normal", lba, 20), lba += 128;
  u32 ctl = OHCI[1], d2 = *dpcr2;
  OHCI[1] = ctl & ~0xcu; /* no periodic and isochronous lists: one spacing everywhere */
  DelayThread(2000);
  timing_round("noperiodic", lba, 20), lba += 128;
  static const u32 prios[] = {0, 1, 3, 5, 7};
  for (u32 p = 0; p < 5; p++) {
    char name[8] = {'p', 'r', 'i', 'o', (char)('0' + prios[p]), 0};
    *dpcr2 = (d2 & ~0x07000000u) | prios[p] << 24;
    timing_round(name, lba, 20), lba += 128;
  }
  *dpcr2 = d2;
  OHCI[1] = ctl;
  DelayThread(2000);
  timing_round("again", lba, 20);
}

static u32 last_lba, part_lba, part_sectors;

/* one WRITE(10) of the bytes in data: queued whole, or CBW, data and CSW one
   after the other (phase[] gets their times) */
static int write_cmd(int phased, u32 lba, u32 sectors) {
  u8 cmd[10] = {0x2A, 0, (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8), (u8)lba, 0,
                (u8)(sectors >> 8), (u8)sectors, 0};
  u32 size = sectors * 512;
  int r;
  make_cbw(cmd, sizeof cmd, size, 0);
  failed = 0;
  progress = now();
  if (!phased) {
    if ((r = start(ep_out, cbw, 31)))
      return r;
    for (u32 at = 0; at < size; at += 8 * KB)
      if ((r = start(ep_out, data + at, size - at > 8 * KB ? 8 * KB : size - at)))
        return r;
    if ((r = start(ep_in, csw, 13)))
      return r;
    if ((r = wait_all()))
      return r;
    return check_csw();
  }
  u32 t0 = now();
  if ((r = one(ep_out, cbw, 31)))
    return r;
  u32 t1 = now();
  for (u32 at = 0; at < size && !r; at += 8 * KB)
    r = start(ep_out, data + at, size - at > 8 * KB ? 8 * KB : size - at);
  if (!r)
    r = wait_all();
  if (r)
    return r;
  u32 t2 = now();
  if ((r = one(ep_in, csw, 13)))
    return r;
  phase[0] += t1 - t0, phase[1] += t2 - t1, phase[2] += now() - t2;
  return check_csw();
}

#define W_REGION 8192u /* sectors: the last 4 MiB of the device */
#define W_TEST (1024 * KB)

/* 0 when a read failed */
static u32 region_sum(u32 base) {
  u32 sum = 1;
  for (u32 lba = base; lba < base + W_REGION; lba += 128) {
    int r = read_cmd(4, lba, 128);
    if (r) {
      printf("usbraw: write: sum read failed %d at %u\n", r, lba);
      return 0;
    }
    for (u32 i = 0; i < 64 * KB / 4; i++)
      sum = sum * 31 + ((u32 *)data)[i];
  }
  return sum ? sum : 1;
}

static void write_test(void) {
  static const u32 sizes[] = {16 * KB, 32 * KB, 64 * KB, 128 * KB};
  u32 base = (last_lba + 1 - W_REGION) & ~255u;
  printf("usbraw: write: region %u..%u, partition %u..%u\n", base, base + W_REGION - 1, part_lba,
         part_lba + part_sectors - 1);
  if (last_lba < 2 * W_REGION || base + W_REGION - 1 > last_lba)
    return;
  u32 sum0 = region_sum(base);
  if (!sum0)
    return;
  u32 k = 0;
  for (u32 s = 0; s < 4; s++)
    for (int phased = 0; phased < 2; phased++, k++) {
      u32 sectors = sizes[s] / 512, cmds = W_TEST / sizes[s], t = 0;
      u32 lba = base + (k % 4) * (W_TEST / 512);
      phase[0] = phase[1] = phase[2] = 0;
      for (u32 i = 0; i < cmds; i++, lba += sectors) {
        int r = read_cmd(4, lba, sectors);
        if (r) {
          printf("usbraw: write: read failed %d at %u\n", r, lba);
          goto check;
        }
        u32 t0 = now();
        if ((r = write_cmd(phased, lba, sectors))) {
          printf("usbraw: write: failed %d at %u\n", r, lba);
          goto check;
        }
        t += now() - t0;
      }
      u32 us = t / cmds * 125 / 4608;
      printf("usbraw: write %s %3u KiB: %4u KiB/s, %6u us a command", phased ? "phased" : "queued",
             sizes[s] / KB, sizes[s] / KB * 1000000u / (us ? us : 1), us);
      if (phased)
        printf(" (cbw %u data %u csw %u)", phase[0] / cmds * 125 / 4608, phase[1] / cmds * 125 / 4608,
               phase[2] / cmds * 125 / 4608);
      printf("\n");
    }
check:;
  u32 sum1 = region_sum(base);
  printf("usbraw: write: region sum %x before, %x after: %s\n", sum0, sum1,
         sum0 == sum1 ? "the same" : "DIFFERS");
}

static int timing_only, write_only;

static void run(void) {
  static const u32 sizes[] = {4 * KB, 16 * KB, 32 * KB, 64 * KB, 128 * KB};
  u8 inq[36] = {0}, cap[8] = {0};
  static const u8 c_inq[6] = {0x12, 0, 0, 0, 36, 0}, c_tur[6] = {0}, c_cap[10] = {0x25};
  int r, i;

  r = sceUsbdSetConfiguration(ep_ctrl, config_id, ctrl_cb, 0);
  if ((r = ctrl_wait(r))) {
    printf("usbraw: set configuration: %d\n", r);
    return;
  }
  r = sceUsbdSetInterface(ep_ctrl, intf_no, intf_alt, ctrl_cb, 0);
  if ((r = ctrl_wait(r)))
    printf("usbraw: set interface: %d (a stall is allowed)\n", r);
  failed = 0;
  DelayThread(5000);
  if ((r = small_cmd(c_inq, 6, inq, 36))) {
    printf("usbraw: inquiry: %d\n", r);
    return;
  }
  inq[32] = 0;
  printf("usbraw: device '%s'\n", (char *)inq + 8);
  for (i = 0; i < 50; i++) {
    if (!(r = small_cmd(c_tur, 6, 0, 0)))
      break;
    if (r <= -3000 || r == -1000)
      return;
    DelayThread(100000);
  }
  printf("usbraw: unit ready after %d tries (%d)\n", i, r);
  if ((r = small_cmd(c_cap, 10, cap, 8))) {
    printf("usbraw: capacity: %d\n", r);
    return;
  }
  last_lba = (u32)cap[0] << 24 | (u32)cap[1] << 16 | (u32)cap[2] << 8 | cap[3];
  printf("usbraw: last lba %u, sector %u\n", last_lba,
         (u32)cap[4] << 24 | (u32)cap[5] << 16 | (u32)cap[6] << 8 | cap[7]);

  /* the file system of the first partition: its cluster size */
  if (!read_cmd(0, 0, 8)) {
    u32 part = data[454] | (u32)data[455] << 8 | (u32)data[456] << 16 | (u32)data[457] << 24;
    part_lba = part;
    part_sectors = data[458] | (u32)data[459] << 8 | (u32)data[460] << 16 | (u32)data[461] << 24;
    printf("usbraw: partition type %x at %u\n", data[450], part);
    if (!read_cmd(0, part, 8))
      printf("usbraw: '%c%c%c%c%c' sectors a cluster %u (FAT), shift %u (exFAT)\n", data[3], data[4],
             data[5], data[6], data[7], data[13], data[109]);
  }

  /* idle counts with nothing happening */
  u32 c0 = idle_count, t0 = now();
  DelayThread(200000);
  idle_rate = (idle_count - c0) / ((now() - t0) / TICKS_MS);

  if (timing_only) {
    timing();
    return;
  }
  if (write_only) {
    if (cap[6] != 2 || cap[7] || cap[5] || cap[4]) {
      printf("usbraw: write: sectors are not 512 bytes\n");
      return;
    }
    write_test();
    return;
  }
  u32 first = 0;
  for (int m = 0; m < MODES; m++) {
    u32 s = sum_of(m);
    printf("usbraw: check %-5s %x%s\n", mode_name[m], s, m && s != first ? " DIFFERS" : "");
    if (!m)
      first = s;
    if (!s)
      return;
  }
  u32 lba = START_LBA;
  for (u32 s = 0; s < sizeof sizes / sizeof *sizes; s++)
    for (int m = 0; m < MODES; m++)
      if (test(m, sizes[s], &lba))
        return;
  test(0, 64 * KB, &lba); /* the first again: has the device changed? */
}

static void worker(void *arg) {
  (void)arg;
  /* usbd calls connect from its own thread */
  for (int i = 0; i < 100 && !dev_count; i++)
    DelayThread(100000);
  DelayThread(3000000); /* the others */
  printf("usbraw: OHCI ports %x %x\n", *(volatile u32 *)0xBF801654, *(volatile u32 *)0xBF801658);
  if (!dev_count)
    printf("usbraw: no device\n");
  for (int i = 0; i < dev_count; i++) {
    printf("usbraw: device %d of %d, id %d\n", i + 1, dev_count, devs[i].id);
    ep_ctrl = devs[i].ctrl, ep_in = devs[i].in, ep_out = devs[i].out;
    config_id = devs[i].config, intf_no = devs[i].intf, intf_alt = devs[i].alt;
    if (ep_in < 0 || ep_out < 0) {
      printf("usbraw: no bulk endpoints\n");
      continue;
    }
    progress = 0;
    run();
    if (stuck)
      break;
  }
  finished = 1;
  progress = 0;
  SignalSema(done);
  ExitDeleteThread();
}

static void open_endpoint(int n, int id, UsbEndpointDescriptor *e) {
  if (e->bmAttributes != USB_ENDPOINT_XFER_BULK)
    return;
  if ((e->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT && devs[n].out < 0)
    devs[n].out = sceUsbdOpenPipeAligned(id, e);
  else if ((e->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN && devs[n].in < 0)
    devs[n].in = sceUsbdOpenPipe(id, e);
}

static int probe(int id) {
  UsbDeviceDescriptor *d = sceUsbdScanStaticDescriptor(id, 0, USB_DT_DEVICE);
  UsbConfigDescriptor *c = d ? sceUsbdScanStaticDescriptor(id, d, USB_DT_CONFIG) : 0;
  if (!c || dev_count >= DEVICES)
    return 0;
  UsbInterfaceDescriptor *i = (UsbInterfaceDescriptor *)((char *)c + c->bLength);
  printf("usbraw: probe id %d: %04x:%04x class %d protocol %x endpoints %d\n", id,
         d->idVendor, d->idProduct, i->bInterfaceClass,
         i->bInterfaceProtocol, i->bNumEndpoints);
  return i->bInterfaceClass == USB_CLASS_MASS_STORAGE && i->bInterfaceProtocol == 0x50 &&
         i->bNumEndpoints >= 2;
}

static int connect(int id) {
  UsbDeviceDescriptor *d = sceUsbdScanStaticDescriptor(id, 0, USB_DT_DEVICE);
  UsbConfigDescriptor *c = sceUsbdScanStaticDescriptor(id, d, USB_DT_CONFIG);
  UsbInterfaceDescriptor *i = (UsbInterfaceDescriptor *)((char *)c + c->bLength);
  UsbEndpointDescriptor *e = sceUsbdScanStaticDescriptor(id, 0, USB_DT_ENDPOINT);
  int d_no = dev_count;
  devs[d_no].id = id, devs[d_no].in = devs[d_no].out = -1;
  devs[d_no].ctrl = sceUsbdOpenPipe(id, 0);
  devs[d_no].config = c->bConfigurationValue;
  devs[d_no].intf = i->bInterfaceNumber, devs[d_no].alt = i->bAlternateSetting;
  for (int n = 0; n < i->bNumEndpoints; n++, e = (UsbEndpointDescriptor *)((char *)e + e->bLength))
    open_endpoint(d_no, id, e);
  dev_count = d_no + 1;
  return 0;
}

static int disconnect(int id) {
  (void)id;
  printf("usbraw: device gone\n");
  return 0;
}

static sceUsbdLddOps driver = {0, 0, "usbraw", probe, connect, disconnect, 0, 0, 0, 0, 0, 0};

int _start(int argc, char **argv) {
  timing_only = argc > 1 && !strcmp(argv[1], "timing");
  write_only = argc > 1 && !strcmp(argv[1], "write");
  iop_sema_t s = {0, 0, 0, 1};
  iop_sema_t d = {0, 0, 0, 1};
  iop_thread_t work = {TH_C, 0, worker, 0x1000, 0x40};
  iop_thread_t dog = {TH_C, 0, watchdog, 0x400, 0x20};
  iop_thread_t idl = {TH_C, 0, idle_thread, 0x200, 126};

  timer = AllocHardTimer(1, 32, 1);
  if (timer < 0)
    return (1 << 2) | MODULE_NO_RESIDENT_END;
  SetTimerMode(timer, 0);
  data = AllocSysMemory(0, MOST_CMD + 4 * KB, 0);
  if (!data)
    return (2 << 2) | MODULE_NO_RESIDENT_END;
  data = (u8 *)(((u32)data + 4 * KB - 1) & ~(4 * KB - 1));
  sema = CreateSema(&s);
  done = CreateSema(&d);
  sceUsbdRegisterLdd(&driver);
  StartThread(CreateThread(&idl), 0);
  StartThread(CreateThread(&dog), 0);
  StartThread(CreateThread(&work), 0);
  WaitSema(done);
  return MODULE_RESIDENT_END;
}
