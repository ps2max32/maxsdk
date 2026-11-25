/* A General MIDI synthesiser and a sequencer of standard MIDI files, on voices
   that the includer (mxsnd.c; the host test) gives. No allocation, no FPU, no
   system calls; not thread-safe. */
#ifndef MXSND_MIDI_H
#define MXSND_MIDI_H

#define MIDI_VOICES 24
#define MIDI_TRACKS 64

/* the bank's layout is <mx/midi.h>'s (tools/sf2bank) */
#define MIDI_BANK_MAGIC 0x4D47584Du
struct midi_bank_head {
  unsigned int magic, version;
  unsigned int nsamples, npresets, nzones, adpcm, data, pad;
};
struct midi_bank_sample {
  unsigned int offset, size, rate, loop;
};
struct midi_bank_preset {
  unsigned short bank, program, zone, nzones;
};
struct midi_bank_zone {
  unsigned char keylo, keyhi, vello, velhi;
  unsigned short sample;
  unsigned char root, excl;
  short tune, scale, pan;
  unsigned short atten, adsr1, adsr2;
};

/* The bank's tables (its first head.data bytes, bytes of them here, 4-byte
   aligned), whose ADPCM is at spu: 0, < 0 when they are not a bank's. They
   stay in use until midi_bank(0, 0, 0). Stops the song. */
int midi_bank(const void *tables, unsigned int bytes, unsigned int spu);
/* A standard MIDI file (format 0 or 1, at most MIDI_TRACKS tracks), in use until
   midi_song(0, 0): 0, < 0 when it is not one. Stops the one before. */
int midi_song(const void *smf, unsigned int bytes);
void midi_play(int loop); /* from its start */
void midi_stop(void);
void midi_pause(int on); /* on: its notes off; a played song goes on after */
/* the voices were silenced by another: none is held or released any more */
void midi_forget(void);
void midi_volume(int volume); /* 0 .. 256 */
int midi_playing(void);  /* 1 also while paused */
/* 512 frames at 48 kHz went by: the events due. */
void midi_step(void);

/* the includer's */
void midi_voice_on(int v, unsigned int spu, unsigned int pitch, unsigned int l, unsigned int r, unsigned int adsr1,
                   unsigned int adsr2);
void midi_voice_set(int v, unsigned int pitch, unsigned int l, unsigned int r);
void midi_voice_off(int v);   /* its release */
int midi_voice_ended(int v);  /* past its end block */
#endif
