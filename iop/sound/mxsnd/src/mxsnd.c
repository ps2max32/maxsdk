/* mxsnd: the sound service (common/include/mxsnd.h), on libsd. Core 1's sound
   data input plays a looping 4 KB buffer by AutoDMA (block transfer, channel
   1): two halves of 512 frames, 48 kHz 16-bit stereo, each 256 left, 256
   right, 256 left, 256 right. When a half is done (every 10.7 ms) the play
   thread does the commands the EE has written, fills the other half from the
   stream ring, and writes the status to the EE. Core 0's output is core 1's
   external input; both cores' voices go to the output dry, effects off. */
#include <irx.h>
#include <intrman.h>
#include <libsd.h>
#include <loadcore.h>
#include <sifman.h>
#include <sifrpc.h>
#include <sysclib.h>
#include <thbase.h>
#include <thsemap.h>
#include <mxsnd.h>

IRX_ID("mxsnd", 1, 0);

#define BLOCK_CH 1 /* core 1's DMA: the sound data input */
#define VOICE_CH 0 /* core 0's DMA: uploads to SPU RAM */
#define HALF 512   /* frames */
#define SD_CORE_0 0
#define SD_CORE_1 1

static struct mxsnd_iop area __attribute__((aligned(64)));
static u8 block_buf[4 * HALF * 2] __attribute__((aligned(64)));
static u32 rpc_in[16] __attribute__((aligned(16)));
static u32 rpc_out[16] __attribute__((aligned(16)));

static int block_sema, lock;
static u32 ee_status;   /* EE physical address of struct mxsnd_area, 0: none */
static u32 seq;
static struct mxsnd_status st[2] __attribute__((aligned(16)));
static int st_dma[2];

/* the stream: its format, the position between source frames (16.16), the
   last frame (the left side of the interpolation) */
static u32 rate = 48000, step = 0x10000;
static int bits = 16, chans = 2, fbytes = 4;
static u32 frac;
static s16 last_l, last_r;
static int playing; /* bytes came since the start or the last drop */

static void set_format(u32 r, int b, int c) {
  rate = r ? r : 48000;
  bits = b == 8 ? 8 : 16;
  chans = c == 1 ? 1 : 2;
  fbytes = (bits / 8) * chans;
  step = (u32)(((unsigned long long)rate << 16) / 48000);
  frac = 0;
}

static int block_done(void *arg) {
  (void)arg;
  iSignalSema(block_sema);
  return 1;
}

/* the reverb: the mode, the voices sent into it (each core's 24 bits), the
   stream too */
static int reverb_mode, reverb_stream;
static u32 wet[2] = {0xFFFFFF, 0xFFFFFF};

static void mixes(void) {
  u16 send = reverb_mode ? 0x300 : 0; /* MSNDEL, MSNDER: the voices' wet into the effect */
  sceSdSetParam(SD_CORE_0 | SD_PARAM_MMIX, 0xC00 | send);
  sceSdSetParam(SD_CORE_1 | SD_PARAM_MMIX, 0xCCC | send | (reverb_mode && reverb_stream ? 0x30 : 0));
  for (int core = 0; core < 2; core++) {
    sceSdSetSwitch(core | SD_SWITCH_VMIXEL, wet[core]);
    sceSdSetSwitch(core | SD_SWITCH_VMIXER, wet[core]);
  }
}

static void reverb(int mode, int depth, int delay, int feedback) {
  for (int core = 0; core < 2; core++) {
    sceSdEffectAttr a = {core, (mode ? mode : reverb_mode) | SD_EFFECT_MODE_CLEAR, (short)depth, (short)depth,
                         delay, feedback};
    sceSdSetCoreAttr(core | SD_CORE_EFFECT_ENABLE, 0);
    if (mode) {
      sceSdSetEffectAttr(core, &a); /* the work area cleared (DMA channel 0) */
      sceSdSetCoreAttr(core | SD_CORE_EFFECT_ENABLE, 1);
    } else {
      sceSdSetParam(core | SD_PARAM_EVOLL, 0);
      sceSdSetParam(core | SD_PARAM_EVOLR, 0);
    }
  }
  reverb_mode = mode;
  mixes();
}

static void voice_reg(int v, u16 reg, u16 val) { sceSdSetParam(SD_VOICE(v / 24, v % 24) | reg, val); }

/* every voice off and silent (KOFF, then volume 0: no release heard) */
static void silence(void) {
  for (int core = 0; core < 2; core++) {
    sceSdSetSwitch(core | SD_SWITCH_KOFF, 0xFFFFFF);
    for (int n = 0; n < 24; n++) {
      sceSdSetParam(SD_VOICE(core, n) | SD_VPARAM_VOLL, 0);
      sceSdSetParam(SD_VOICE(core, n) | SD_VPARAM_VOLR, 0);
    }
  }
}

static void commands(void) {
  u32 w = area.head.cmd_w, r = st[seq & 1].cmd_r;
  u32 kon[2] = {0, 0}, koff[2] = {0, 0};
  for (; r != w; r++) {
    const struct mxsnd_cmd *c = &area.cmd[r & (MXSND_CMDS - 1)];
    int v = c->voice, core = v / 24;
    u32 bit = 1u << (v % 24);
    if (v >= MXSND_VOICES && c->op <= MXSND_C_STOP)
      continue;
    switch (c->op) {
    case MXSND_C_PLAY:
      voice_reg(v, SD_VPARAM_PITCH, c->pitch);
      voice_reg(v, SD_VPARAM_VOLL, c->voll);
      voice_reg(v, SD_VPARAM_VOLR, c->volr);
      voice_reg(v, SD_VPARAM_ADSR1, c->adsr1);
      voice_reg(v, SD_VPARAM_ADSR2, c->adsr2);
      sceSdSetAddr(SD_VOICE(core, v % 24) | SD_VADDR_SSA, c->addr);
      kon[core] |= bit, koff[core] &= ~bit;
      break;
    case MXSND_C_SET:
      voice_reg(v, SD_VPARAM_PITCH, c->pitch);
      voice_reg(v, SD_VPARAM_VOLL, c->voll);
      voice_reg(v, SD_VPARAM_VOLR, c->volr);
      break;
    case MXSND_C_STOP:
      koff[core] |= bit, kon[core] &= ~bit;
      break;
    case MXSND_C_SILENCE:
      silence();
      kon[0] = kon[1] = koff[0] = koff[1] = 0;
      break;
    case MXSND_C_FORMAT:
      set_format(c->addr, c->voll, c->volr);
      break;
    case MXSND_C_STREAM_VOL:
      sceSdSetParam(SD_CORE_1 | SD_PARAM_BVOLL, c->voll);
      sceSdSetParam(SD_CORE_1 | SD_PARAM_BVOLR, c->voll);
      break;
    case MXSND_C_STREAM_DROP:
      st[seq & 1].stream_r = c->addr;
      frac = 0, last_l = last_r = 0, playing = 0;
      break;
    case MXSND_C_REVERB:
      reverb_stream = c->voice == 1;
      reverb(c->addr <= 9 ? (int)c->addr : 0, c->voll & 0x7FFF, c->volr & 0x7F, (c->volr >> 8) & 0x7F);
      break;
    case MXSND_C_WET:
      if (v < MXSND_VOICES) {
        wet[core] = c->voll ? wet[core] | bit : wet[core] & ~bit;
        sceSdSetSwitch(core | SD_SWITCH_VMIXEL, wet[core]);
        sceSdSetSwitch(core | SD_SWITCH_VMIXER, wet[core]);
      }
      break;
    }
  }
  st[seq & 1].cmd_r = r;
  for (int core = 0; core < 2; core++) {
    if (koff[core])
      sceSdSetSwitch(core | SD_SWITCH_KOFF, koff[core]);
    if (kon[core])
      sceSdSetSwitch(core | SD_SWITCH_KON, kon[core]);
  }
}

/* frame i of the stream from position r (bytes) */
static void frame(u32 r, u32 i, s16 *l, s16 *rt) {
  u32 at = r + i * (u32)fbytes;
  if (bits == 16) {
    *l = *(s16 *)&area.stream[at & (MXSND_STREAM_BYTES - 2)];
    *rt = chans == 2 ? *(s16 *)&area.stream[(at + 2) & (MXSND_STREAM_BYTES - 2)] : *l;
  } else {
    *l = (s16)((area.stream[at & (MXSND_STREAM_BYTES - 1)] - 128) << 8);
    *rt = chans == 2 ? (s16)((area.stream[(at + 1) & (MXSND_STREAM_BYTES - 1)] - 128) << 8) : *l;
  }
}

/* 512 frames of the stream into the half at out, linearly interpolated from
   its rate to 48 kHz; silence for what has not come */
static void render(u8 *out) {
  s16 *lo = (s16 *)out, *ro = (s16 *)(out + 512);
  u32 r = st[seq & 1].stream_r, w = area.head.stream_w;
  u32 have = (w - r) / (u32)fbytes; /* whole source frames queued */
  if (have)
    playing = 1;
  u32 used = 0;
  for (int i = 0; i < HALF; i++) {
    if (i == 256)
      lo = (s16 *)(out + 1024), ro = (s16 *)(out + 1536);
    s16 l = 0, rt = 0;
    if (used < have) {
      s16 nl, nr;
      frame(r, used, &nl, &nr);
      l = (s16)(last_l + ((((int)nl - last_l) * (int)(frac >> 4)) >> 12));
      rt = (s16)(last_r + ((((int)nr - last_r) * (int)(frac >> 4)) >> 12));
      frac += step;
      while (frac >= 0x10000 && used < have) {
        frac -= 0x10000;
        last_l = nl, last_r = nr;
        used++;
        if (used < have)
          frame(r, used, &nl, &nr);
      }
    } else if (playing)
      st[seq & 1].starved++;
    *lo++ = l;
    *ro++ = rt;
  }
  st[seq & 1].stream_r = r + used * (u32)fbytes;
}

/* the status into the EE's area, the older record */
static void status(void) {
  struct mxsnd_status *s = &st[seq & 1];
  s->endx0 = sceSdGetSwitch(SD_CORE_0 | SD_SWITCH_ENDX);
  s->endx1 = sceSdGetSwitch(SD_CORE_1 | SD_SWITCH_ENDX);
  s->frames += HALF;
  s->seq = seq + 1;
  int slot = (int)(s->seq & 1);
  if (ee_status) {
    /* the record of this slot from two rounds ago must be on its way */
    while (st_dma[slot] && sceSifDmaStat(st_dma[slot]) >= 0)
      DelayThread(50);
    SifDmaTransfer_t d = {s, (void *)(ee_status + (u32)slot * sizeof *s), sizeof *s, 0};
    int stt, id;
    do {
      CpuSuspendIntr(&stt);
      id = (int)sceSifSetDma(&d, 1);
      CpuResumeIntr(stt);
    } while (!id);
    st_dma[slot] = id;
  }
  /* the next round starts from this one's values, in the other buffer */
  seq++;
  st[seq & 1] = *s;
}

static void play(void *arg) {
  (void)arg;
  for (;;) {
    WaitSema(block_sema);
    WaitSema(lock);
    int half = 1 - (int)(sceSdBlockTransStatus(BLOCK_CH, 0) >> 24);
    commands();
    render(block_buf + half * 2048);
    status();
    SignalSema(lock);
  }
}

static void *rpc(int fno, void *data, int size) {
  u32 *a = data;
  (void)size;
  rpc_out[0] = (u32)-22;
  switch (fno) {
  case MXSND_INIT: {
    WaitSema(lock);
    ee_status = a[0];
    silence();
    wet[0] = wet[1] = 0xFFFFFF;
    reverb_stream = 0;
    reverb(0, 0, 0, 0);
    struct mxsnd_status *s = &st[seq & 1];
    /* the rings go on from where they are: the EE's counts start there */
    s->stream_r = area.head.stream_w;
    s->cmd_r = area.head.cmd_w;
    s->starved = 0;
    frac = 0, last_l = last_r = 0, playing = 0;
    set_format(48000, 16, 2);
    sceSdSetParam(SD_CORE_1 | SD_PARAM_BVOLL, 0x3FFF);
    sceSdSetParam(SD_CORE_1 | SD_PARAM_BVOLR, 0x3FFF);
    struct mxsnd_init *in = (struct mxsnd_init *)rpc_out;
    in->version = MXSND_VERSION;
    in->area = (u32)&area;
    in->stream_r = s->stream_r;
    in->cmd_r = s->cmd_r;
    SignalSema(lock);
    break;
  }
  case MXSND_UPLOAD: {
    u32 spu = a[0], n = a[1];
    if (spu < MXSND_SPU_START || n > MXSND_UPLOAD_BYTES || spu + n > MXSND_SPU_END) {
      rpc_out[0] = (u32)-22;
      break;
    }
    WaitSema(lock); /* the reverb's clear (the play thread) uses DMA channel 0 too */
    sceSdVoiceTrans(VOICE_CH, SD_TRANS_WRITE | SD_TRANS_MODE_DMA, area.upload, (u32 *)spu, n);
    sceSdVoiceTransStatus(VOICE_CH, 1); /* waits */
    SignalSema(lock);
    rpc_out[0] = 0;
    break;
  }
  case MXSND_STOP:
    WaitSema(lock);
    rpc_out[0] = ee_status && ee_status >= a[0] && ee_status < a[1];
    if (rpc_out[0]) {
      ee_status = 0;
      silence();
      st[seq & 1].stream_r = area.head.stream_w; /* what was queued is dropped */
      frac = 0, last_l = last_r = 0, playing = 0;
    }
    SignalSema(lock);
    break;
  case MXSND_PEEK: {
    u32 spu = a[0], n = a[1];
    if (n > MXSND_UPLOAD_BYTES || (n & 63) || spu + n > 0x200000) {
      rpc_out[0] = (u32)-22;
      break;
    }
    WaitSema(lock);
    sceSdVoiceTrans(VOICE_CH, SD_TRANS_READ | SD_TRANS_MODE_DMA, area.upload, (u32 *)spu, n);
    sceSdVoiceTransStatus(VOICE_CH, 1);
    SignalSema(lock);
    rpc_out[0] = 0;
    break;
  }
  }
  return rpc_out;
}

static void rpc_thread(void *arg) {
  static SifRpcDataQueue_t queue;
  static SifRpcServerData_t server;
  (void)arg;
  sceSifSetRpcQueue(&queue, GetThreadId());
  sceSifRegisterRpc(&server, MXSND_SID, rpc, rpc_in, 0, 0, &queue);
  sceSifRpcLoop(&queue);
}

static int start_thread(void (*fn)(void *), int prio) {
  iop_thread_t t = {TH_C, 0, fn, 0x800, prio};
  int id = CreateThread(&t);
  if (id > 0)
    StartThread(id, 0);
  return id;
}

int _start(int argc, char **argv) {
  (void)argc, (void)argv;
  iop_sema_t s = {0, 0, 0, 1};
  block_sema = CreateSema(&s);
  s.initial = 1;
  lock = CreateSema(&s);
  if (block_sema <= 0 || lock <= 0)
    return MODULE_NO_RESIDENT_END;

  /* audsrv's AutoDMA, if it runs (mxlink loads it at boot): stopped */
  sceSdSetTransCallback(BLOCK_CH, 0);
  sceSdBlockTrans(BLOCK_CH, SD_TRANS_STOP, 0, 0, 0);
  if (sceSdInit(0) < 0)
    return MODULE_NO_RESIDENT_END;
  silence();
  /* the voices dry to the output, effects off; core 0's output into core 1
     (external input), core 1's sound data input (the stream) */
  for (int core = 0; core < 2; core++) {
    sceSdSetSwitch(core | SD_SWITCH_VMIXL, 0xFFFFFF);
    sceSdSetSwitch(core | SD_SWITCH_VMIXR, 0xFFFFFF);
    sceSdSetParam(core | SD_PARAM_EVOLL, 0);
    sceSdSetParam(core | SD_PARAM_EVOLR, 0);
    sceSdSetParam(core | SD_PARAM_MVOLL, 0x3FFF);
    sceSdSetParam(core | SD_PARAM_MVOLR, 0x3FFF);
  }
  reverb(0, 0, 0, 0); /* off; MMIX: core 0 MSNDL, MSNDR, core 1 MSND, MIN, SIN: dry */
  sceSdSetParam(SD_CORE_0 | SD_PARAM_AVOLL, 0);
  sceSdSetParam(SD_CORE_0 | SD_PARAM_AVOLR, 0);
  sceSdSetParam(SD_CORE_0 | SD_PARAM_BVOLL, 0);
  sceSdSetParam(SD_CORE_0 | SD_PARAM_BVOLR, 0);
  sceSdSetParam(SD_CORE_1 | SD_PARAM_AVOLL, 0x7FFF);
  sceSdSetParam(SD_CORE_1 | SD_PARAM_AVOLR, 0x7FFF);
  sceSdSetParam(SD_CORE_1 | SD_PARAM_BVOLL, 0x3FFF);
  sceSdSetParam(SD_CORE_1 | SD_PARAM_BVOLR, 0x3FFF);

  memset(block_buf, 0, sizeof block_buf);
  sceSdSetTransCallback(BLOCK_CH, block_done);
  sceSdBlockTrans(BLOCK_CH, SD_TRANS_LOOP, block_buf, sizeof block_buf, 0);

  sceSifInitRpc(0);
  if (start_thread(play, 0x27) <= 0 || start_thread(rpc_thread, 0x28) <= 0)
    return MODULE_NO_RESIDENT_END;
  return MODULE_RESIDENT_END;
}
