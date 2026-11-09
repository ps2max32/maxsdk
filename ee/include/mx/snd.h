#ifndef MX_SND_H
#define MX_SND_H

/* Sound through maxsdk's IOP module mxsnd.irx (iop/sound/mxsnd) on ps2sdk's
   libsd.irx; libmx carries both and loads them unless the IOP has them. The
   output (the SPU2's core 1) mixes:
     a PCM stream: the EE queues samples in any format mxsnd converts to
       48 kHz (8 or 16 bits, mono or stereo); about 100 ms of it are kept
       queued at most, so queue some every frame;
     samples: SPU ADPCM (mx_snd_encode) loaded into SPU RAM once, played on
       the 48 voices of the two cores (0 .. 47), by voice or as channels (a
       channel takes a voice by priority and loudness).
   The EE writes the stream and the commands into mxsnd's memory by SIF DMA
   and mxsnd works them every 10.7 ms: the stream's calls and mx_snd_commit
   cost no wait on the IOP; mx_snd_init and mx_snd_load wait for it. Not for
   interrupt handlers, nor for two threads at once. */

#include <mx/types.h>

#define MX_SND_RATE 48000 /* the SPU2's output rate */
#define MX_SND_VOL_MAX 0x3FFF
#define MX_SND_VOICES 48

/* Load the modules (unless the IOP has them), start mxsnd: the voices and the
   stream silent. Returns < 0 if the IOP does not answer or a module does
   not load. */
int mx_snd_init(void);

/* Send the channels' and voices' changes made since (one transfer); the
   stream's calls send them too. Call it once a frame. */
void mx_snd_commit(void);

/* ---- the PCM stream ---------------------------------------------------------- */

/* The format of the samples queued from now on (rate in Hz, up to 48000,
   linearly interpolated to 48 kHz; bits 8 (unsigned) or 16; channels 1 or
   2; 16-bit stereo is interleaved). The default is 48000 Hz 16-bit stereo.
   Returns 0, < 0 for a format mxsnd does not take. */
int mx_snd_stream_format(u32 rate, int bits, int channels);
/* Bytes that may be queued now. */
u32 mx_snd_stream_room(void);
/* Bytes queued and not played yet. */
u32 mx_snd_stream_queued(void);
/* Queue up to bytes of data (any alignment): returns the bytes taken, which
   is less when the room is less. The stream sounds as it comes. */
u32 mx_snd_stream_queue(const void *data, u32 bytes);
/* Drop what is queued (silence until more comes). */
void mx_snd_stream_stop(void);
/* The stream's volume, 0 .. MX_SND_VOL_MAX (the default). */
void mx_snd_stream_volume(int volume);

/* ---- samples ----------------------------------------------------------------- */

/* Bytes of ADPCM for n samples. */
#define MX_SND_ADPCM_SIZE(n) ((((n) + 27) / 28) * 16)
/* Encode n samples (16-bit mono) to SPU ADPCM in out (MX_SND_ADPCM_SIZE(n)
   bytes; the last block is padded with silence). loop: the voice plays it
   again from the start at the end; else it stops there. Returns the bytes
   written. */
u32 mx_snd_encode(const s16 *pcm, u32 n, u8 *out, int loop);
/* The same, looping from sample loop_start (rounded down to a block of 28)
   to the end. */
u32 mx_snd_encode_loop(const s16 *pcm, u32 n, u8 *out, u32 loop_start);

/* A sample in SPU RAM (mx_snd_load fills it in). */
struct mx_snd_sample {
  u32 rate;
  u32 size;
  u32 spu;  /* its address in SPU RAM, 0: not loaded */
  u32 loop; /* its ADPCM loops (mx_snd_encode's loop) */
};

/* Load size bytes of ADPCM (from mx_snd_encode, any alignment), played at
   rate Hz (up to four times MX_SND_RATE), into SPU RAM (about 1.8 MiB for
   samples). Returns 0, < 0 on failure (IOP or SPU RAM full). */
int mx_snd_load(struct mx_snd_sample *s, const void *adpcm, u32 size, u32 rate);
/* Stop its voices and free its SPU RAM. */
int mx_snd_free(struct mx_snd_sample *s);

/* Voices, each call sent at once: play s at its rate and full volume on
   voice (0 .. 47), or on a voice that is free when voice < 0. Returns the
   voice, < 0 when every voice is busy. */
int mx_snd_play(const struct mx_snd_sample *s, int voice);
/* 1 while voice plays s: a loop-less sample until its end (as mxsnd last
   saw it, up to 10.7 ms late), a looping one until it is stopped. */
int mx_snd_playing(int voice, const struct mx_snd_sample *s);
/* A voice's volume, 0 .. MX_SND_VOL_MAX each side. */
void mx_snd_voice_volume(int voice, int left, int right);

/* Channels: s played at rate Hz with volumes left, right (0 ..
   MX_SND_VOL_MAX) on a free voice, else on the one of the lowest priority
   (0 .. 255), the quietest of those, when this one's priority is higher, or
   as high and it is louder. Returns the channel (> 0), 0 when it is not
   played. The changes go with the next mx_snd_commit. */
int mx_snd_ch_play(const struct mx_snd_sample *s, u32 rate, int left, int right, int priority);
/* The channel's rate and volumes; < 0 when it has ended or lost its voice. */
int mx_snd_ch_set(int ch, u32 rate, int left, int right);
void mx_snd_ch_stop(int ch);
/* 1 while the channel sounds (as mx_snd_playing). */
int mx_snd_ch_playing(int ch);
/* The channel into the reverb (on: the default at each play) or dry only. */
void mx_snd_ch_reverb(int ch, int on);

/* ---- reverb -------------------------------------------------------------------- */

/* The SPU2's reverb (both cores the same), its work areas at the top of SPU
   RAM: a mode, how much of it is heard (depth, 0 .. 0x7FFF), delay and
   feedback (0 .. 127, the echo and delay modes only), the stream into it
   too or not (the voices always, but those mx_snd_ch_reverb took out). Its
   work areas are cleared at each call. Off by default. */
enum {
  MX_SND_REVERB_OFF, MX_SND_REVERB_ROOM, MX_SND_REVERB_STUDIO_A, MX_SND_REVERB_STUDIO_B,
  MX_SND_REVERB_STUDIO_C, MX_SND_REVERB_HALL, MX_SND_REVERB_SPACE, MX_SND_REVERB_ECHO,
  MX_SND_REVERB_DELAY, MX_SND_REVERB_PIPE
};
void mx_snd_reverb(int mode, int depth, int delay, int feedback, int stream);

#endif
