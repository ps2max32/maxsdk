/* mxsnd: the sound service of the IOP (iop/sound/mxsnd) for the EE, on libsd,
   in place of audsrv: the 48 voices of the SPU2's two cores and a PCM stream
   on core 1's sound data input (AutoDMA, 48 kHz 16-bit stereo).

   The EE drives it without a call a frame. The module has an area of IOP
   memory (struct mxsnd_iop, its address in INIT's answer) that the EE writes
   by SIF DMA: the stream's bytes and the commands into their rings, then the
   head (the rings' write counts). DMA keeps the order, so what the head
   counts is there. The module works every 10.7 ms (512 frames at 48 kHz,
   half of the AutoDMA buffer): the commands up to the head, then the next
   512 frames of the stream, converted from its format; then it writes its
   status by SIF DMA into an area of EE memory that INIT gave (the older of
   two records: the newer, the higher seq, is the status).

   RPCs are for set-up only: INIT, UPLOAD (ADPCM into SPU RAM, through the
   area's upload buffer). Arguments and answers are words. */
#ifndef MXSND_H
#define MXSND_H

#define MXSND_SID 0x4D58534E /* "MXSN" */
#define MXSND_VERSION 1
#define MXSND_VOICES 48 /* 0 .. 23 core 0, 24 .. 47 core 1 */

#define MXSND_STREAM_BYTES 32768 /* a power of two */
#define MXSND_CMDS 512           /* a power of two */
#define MXSND_UPLOAD_BYTES 32768
/* SPU RAM for samples: the cores' fixed areas and libsd's silent block below
   (0x5000), the reverb's work areas above: each core's ends at its EEA (libsd:
   core 0 0x1DFFFF, core 1 0x1FFFFF) and takes up to 0x18040 bytes (echo) */
#define MXSND_SPU_START 0x5040u
#define MXSND_SPU_END 0x1C7000u

enum {
  MXSND_INIT,   /* in: status area (EE physical address, 64-byte aligned; 0: stop
                   writing it, for a program's end); answer: struct mxsnd_init.
                   Silences the voices and the stream */
  MXSND_UPLOAD, /* in: SPU address, bytes (the area's upload buffer, 64-byte
                   multiple); answer: 0, < 0 */
  MXSND_PEEK,   /* in: SPU address, bytes (64-byte multiples): SPU RAM into the
                   upload buffer (tests: the voices' mix in the output area);
                   answer: 0, < 0 */
  MXSND_STOP,   /* in: EE physical start, end (a program's memory, for its end):
                   when the status area is in [start, end), its writes stop and
                   the voices and the stream are silenced; answer: 1, else 0 (it
                   belongs to another program) */
};
/* the output area: each core's dry mix of its voices, 512 samples (two halves
   of 256, written in turn), left then right */
#define MXSND_SPU_MIX0 0x2000u
#define MXSND_SPU_MIX1 0x3000u

struct mxsnd_init {
  unsigned int version; /* MXSND_VERSION */
  unsigned int area;    /* IOP address of struct mxsnd_iop */
  unsigned int stream_r, cmd_r; /* the counts read so far: the EE goes on from there */
};

/* a command, 16 bytes */
struct mxsnd_cmd {
  unsigned char op, voice;
  unsigned short pitch;      /* 4096: 48 kHz */
  unsigned short voll, volr; /* 0 .. 0x3FFF */
  unsigned int addr;         /* SPU address of the sample's first block */
  unsigned short adsr1, adsr2;
};
enum {
  MXSND_C_PLAY = 1,   /* voice: addr, pitch, voll, volr, adsr1, adsr2; key on */
  MXSND_C_SET,        /* voice: pitch, voll, volr */
  MXSND_C_STOP,       /* voice: key off (its release) */
  MXSND_C_SILENCE,    /* all voices off at once */
  MXSND_C_FORMAT,     /* the stream: addr rate (Hz), voll bits (8: unsigned, 16), volr channels */
  MXSND_C_STREAM_VOL, /* voll: the stream's volume, 0 .. 0x3FFF */
  MXSND_C_STREAM_DROP, /* the stream's bytes up to the count addr are dropped */
  MXSND_C_REVERB,     /* both cores' effect: addr mode (SD_EFFECT_MODE_*, 0 off), voll
                         depth (0 .. 0x7FFF), volr delay | feedback << 8 (0 .. 127, echo
                         and delay), voice 1: the stream into it too; its work area
                         cleared */
  MXSND_C_WET         /* voice: voll 1 into the reverb (the default), 0 dry only */
};

/* the head: counts, free running (positions are counts mod the ring's size) */
struct mxsnd_head {
  unsigned int stream_w; /* bytes */
  unsigned int cmd_w;    /* commands */
  unsigned int pad[2];
};

struct mxsnd_iop {
  struct mxsnd_head head; /* written last of a transfer */
  unsigned char pad[48];
  struct mxsnd_cmd cmd[MXSND_CMDS];
  unsigned char stream[MXSND_STREAM_BYTES];
  unsigned char upload[MXSND_UPLOAD_BYTES];
};

/* the status, 32 bytes; the EE's area has two */
struct mxsnd_status {
  unsigned int seq;      /* written last of the two when higher; 0: never written */
  unsigned int stream_r; /* the stream's bytes taken */
  unsigned int cmd_r;    /* commands done */
  unsigned int endx0;    /* voices 0 .. 23 past their end block (since their key on) */
  unsigned int endx1;    /* voices 24 .. 47 */
  unsigned int frames;   /* 48 kHz frames played */
  unsigned int starved;  /* frames the stream had no bytes for while it played */
  unsigned int pad;
};
_Static_assert(sizeof(struct mxsnd_status) == 32, "struct mxsnd_status");
_Static_assert(sizeof(struct mxsnd_cmd) == 16, "struct mxsnd_cmd");

struct mxsnd_area {
  struct mxsnd_status s[2];
};

#endif
