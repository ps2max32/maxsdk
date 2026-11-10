/* Controllers through rom0:PADMAN (the protocol of the libpad EE client for
   the PADMAN in ROM). Each opened port has a 256-byte area in EE memory into
   which the IOP writes two records alternately; the one with the larger
   frame number is the newest. */

#include <mx/cpu.h>
#include <mx/kernel.h>
#include <mx/mx.h>
#include <mx/pad.h>
#include <mx/sif.h>
#include <mx/timer.h>

#define PAD_SID 0x8000010F
#define PAD_SID2 0x8000011F
#define PAD_CMD_OPEN 0x80000100
#define PAD_CMD_SET_MAIN_MODE 0x80000105 /* port, slot, mode (1: analog), lock (3) */
/* the motors (ps2sdk's libpad, the ROM's PADMAN): port, slot, 6 bytes */
#define PAD_CMD_SET_ACT_DIRECT 0x80000106 /* the motors' values */
#define PAD_CMD_SET_ACT_ALIGN 0x80000107  /* which byte drives which motor */
#define PAD_REQ_COMPLETE 0

#define PAD_STATE_STABLE 6
#define PAD_STATE_FINDCTP1 2
#define PAD_STATE_FINDPAD 1 /* and 0, DISCONN: no pad on the port */

struct pad_record {
  u32 frame;
  u8 state, req_state, ok, unk7;
  u8 data[32]; /* data[1]: mode, data[2..3]: the buttons, 0 = held,
                  data[4..7] the sticks in analog mode */
  u32 length;
  u8 rest[20];
};

static struct mx_sif_client pad, pad2;
static struct pad_record area[2][4] MX_DMA_LINES; /* 2 records + spare per port */
static s32 cmd[32] MX_DMA_LINES;
static s32 res[32] MX_DMA_LINES;
static int ready;
static u32 prev[2];
static u32 analog_polls[2];
/* the motors: aligned since the port opened (or the pad came), the values
   wanted, and whether they still have to go (mx__pad_act) */
static u8 act_aligned[2], act_small[2], act_big[2], act_pending[2];

/* The kernel, when a program that waited for another goes on (step 2c): its
   ports were closed at the other's end, so its next mx_pad_init opens them
   again. */
void mx__pad_forget(void) { ready = 0; }

int mx__pad_init(void) {
  if (ready)
    return 0;
  if (!mx_sif_ready() && mx_sif_init() < 0)
    return -1;
  /* a driver loaded by the loader (or by an earlier run) answers at once */
  if (mx_sif_bind_tries(&pad, PAD_SID, 2) < 0 || !pad.server) {
    if (mx_iop_load_module("rom0:SIO2MAN") < 0 || mx_iop_load_module("rom0:PADMAN") < 0)
      return -1;
    if (mx_sif_bind(&pad, PAD_SID) < 0)
      return -1;
  }
  if (mx_sif_bind(&pad2, PAD_SID2) < 0)
    return -1;
  for (int port = 0; port < 2; port++) {
    volatile struct pad_record *r = MX_UNCACHED(area[port]);
    for (int i = 0; i < 2; i++) {
      for (int k = 0; k < 32; k++)
        r[i].data[k] = 0xFF;
      r[i].frame = 0;
      r[i].length = 0;
    }
    mx_dcache_invalidate(area[port], sizeof area[port]);
    act_aligned[port] = 0;
    cmd[0] = PAD_CMD_OPEN;
    cmd[1] = port;
    cmd[2] = 0; /* slot */
    cmd[4] = (s32)(uintptr_t)area[port];
    if (mx_sif_call(&pad, 1, cmd, sizeof cmd, (void *)MX_UNCACHED(res), sizeof res) < 0)
      return -1;
  }
  ready = 1;
  return 0;
}

static volatile struct pad_record *newest(int port) {
  volatile struct pad_record *r = MX_UNCACHED(area[port]);
  return r[0].frame < r[1].frame ? &r[1] : &r[0];
}

/* A pad with the configuration mode only (DualShock, DualShock 2: the
   driver's STABLE). Not FINDCTP1, a pad without it (digital PS1 pads, guns):
   on a slim PS2 (SCPH-70004) port 0 answers so with no pad plugged in
   (digital, model 0; probably its IR receiver for the DVD remote), and took
   port 0 from the USB and Bluetooth pads. */
static int connected(volatile struct pad_record *n) {
  return n->state == PAD_STATE_STABLE && n->length >= 4 && n->data[0] == 0;
}

/* The motors: PADMAN takes them only from a pad that is stable, in
   DualShock (analog) mode and done with its last request; the alignment
   (byte 0 the small motor, byte 1 the big one) once, then the values. A
   digital pad is put in DualShock mode for them. */
static void act_call(u32 command, int port, const u8 d[6]) {
  cmd[0] = (s32)command;
  cmd[1] = port;
  cmd[2] = 0; /* slot */
  u8 *b = (u8 *)&cmd[3];
  for (int i = 0; i < 6; i++)
    b[i] = d[i];
  mx_sif_call(&pad, 1, cmd, sizeof cmd, (void *)MX_UNCACHED(res), sizeof res);
}

static u32 act_mode_at[2];

static void act_service(int port, volatile struct pad_record *n) {
  if (!act_pending[port] || n->state != PAD_STATE_STABLE || n->req_state != PAD_REQ_COMPLETE)
    return;
  if ((n->data[1] >> 4) != 7) {
    /* the motors only in DualShock mode, as libpad's examples set it first:
       asked for here when the program never read the sticks (want_analog) */
    if (mx_count() - act_mode_at[port] > MX_EE_HZ) {
      act_mode_at[port] = mx_count();
      cmd[0] = PAD_CMD_SET_MAIN_MODE;
      cmd[1] = port;
      cmd[2] = 0; /* slot */
      cmd[3] = 1; /* analog */
      cmd[4] = 3; /* locked */
      mx_sif_call(&pad, 1, cmd, sizeof cmd, (void *)MX_UNCACHED(res), sizeof res);
    }
    return;
  }
  if (!act_aligned[port]) {
    static const u8 align[6] = {0, 1, 0xff, 0xff, 0xff, 0xff};
    act_call(PAD_CMD_SET_ACT_ALIGN, port, align);
    act_aligned[port] = 1;
    return; /* the values once the pad took it */
  }
  u8 v[6] = {act_small[port], act_big[port], 0, 0, 0, 0};
  act_call(PAD_CMD_SET_ACT_DIRECT, port, v);
  act_pending[port] = 0;
}

int mx__pad_act(int port, u32 small, u32 big) {
  if (!ready || (unsigned)port > 1)
    return -1;
  volatile struct pad_record *n = newest(port);
  /* kept whatever the pad does now, and sent when it can take them: busy
     with a command (PADMAN's EXECCMD after each one) or found again (as
     after the switch to DualShock mode, which a stop must not miss) */
  act_small[port] = small ? 1 : 0, act_big[port] = (u8)(big > 255 ? 255 : big);
  act_pending[port] = 1;
  act_service(port, n);
  return n->state <= PAD_STATE_FINDPAD ? -1 : 0;
}

/* A DualShock starts digital: once it is stable, ask for analog mode,
   locked (the ANALOG button does not switch it back); again every 128 polls
   while it is digital (a pad without analog mode says no). */
static void want_analog(int port, volatile struct pad_record *n) {
  if (n->state != PAD_STATE_STABLE || (n->data[1] >> 4) == 7 || analog_polls[port]++ % 128)
    return;
  cmd[0] = PAD_CMD_SET_MAIN_MODE;
  cmd[1] = port;
  cmd[2] = 0; /* slot */
  cmd[3] = 1;
  cmd[4] = 3;
  mx_sif_call(&pad, 1, cmd, sizeof cmd, (void *)MX_UNCACHED(res), sizeof res);
}

int mx__pad_read(int port, struct mx_pad_state *s, int sticks) {
  s->buttons = 0;
  s->analog = 0;
  for (int i = 0; i < 4; i++)
    s->sticks[i] = 128;
  if (!ready || (unsigned)port > 1)
    return 0;
  volatile struct pad_record *n = newest(port);
  if (!connected(n)) {
    if (n->state <= PAD_STATE_FINDPAD)
      act_aligned[port] = 0; /* gone: a pad that comes is aligned again */
    else
      act_service(port, n); /* busy: what waits goes when it is done */
    return 0;
  }
  act_service(port, n);
  s->buttons = ~(u32)(n->data[2] | n->data[3] << 8) & 0xFFFF;
  /* no home button on this pad: the combination is it */
  if ((s->buttons & MX_PAD_QUIT) == MX_PAD_QUIT)
    s->buttons = (s->buttons & ~MX_PAD_QUIT) | MX_PAD_HOME;
  if (!sticks)
    return 1;
  want_analog(port, n);
  /* data[1]: the type in the high nibble (7: analog) and the halfwords of
     data after it in the low one; the sticks are data[4..7] */
  if ((n->data[1] >> 4) != 7 || (n->data[1] & 15) < 3 || n->length < 8)
    return 1;
  s->analog = 1;
  for (int i = 0; i < 4; i++)
    s->sticks[i] = n->data[4 + i];
  return 1;
}

/* the kernel's pad (table version 6), or this file's */
static int kpad(void) { return mx__kernel && mx__kernel->version >= 6; }

static int pad_get(int port, struct mx_pad_state *s, int sticks) {
  return kpad() ? mx__kernel->pad_read(port, s, sticks) : mx__pad_read(port, s, sticks);
}

int mx_pad_init(void) { return kpad() ? mx__kernel->pad_init() : mx__pad_init(); }

int mx_pad_rumble(int port, u32 small, u32 big) {
  if (!mx__kernel || mx__kernel->version < 11)
    return -1;
  return mx__kernel->pad_rumble(port, small, big);
}

int mx_pad_connected(int port) {
  struct mx_pad_state s;
  return pad_get(port, &s, 0);
}

u32 mx_pad_buttons(int port) {
  struct mx_pad_state s;
  pad_get(port, &s, 0);
  /* the way out of any program: back to the loader (the combination itself
     from a kernel before the home button) */
  if ((s.buttons & MX_PAD_HOME) || (s.buttons & MX_PAD_QUIT) == MX_PAD_QUIT)
    mx_exit(0);
  return s.buttons;
}

int mx_pad_sticks(int port, u8 st[4]) {
  struct mx_pad_state s;
  pad_get(port, &s, 1);
  for (int i = 0; i < 4; i++)
    st[i] = s.sticks[i];
  return s.analog;
}

u32 mx_pad_pressed(int port) {
  if ((unsigned)port > 1)
    return 0;
  u32 b = mx_pad_buttons(port), p = b & ~prev[port];
  prev[port] = b;
  return p;
}
