#ifndef MX_MIDI_H
#define MX_MIDI_H

/* MIDI music, played by mxsnd on the IOP: a standard MIDI file and a General
   MIDI bank of samples are handed over, the IOP reads the song and the SPU2
   plays the notes, so nothing is due from the EE while it plays. Notes start
   on steps of 10.67 ms (512 frames at 48 kHz).

   While a bank is loaded the music has voices 24 to 47; mx/snd.h's are 0 to 23
   then (a voice of 24 or more playing at the load is stopped).

   The bank is tools/sf2bank's file, made from a SoundFont 2 file:
     sf2bank [-r maxrate] in.sf2 out.bnk
   (e.g. TimGM6mb, GPL-2: 1.7 MB of SPU RAM with -r 32000). What the SPU2
   cannot do is not in it: filters, LFOs, modulators.

   Songs: format 0 and 1, at most 64 tracks. Notes, program change, pitch bend
   and its range, controllers 7 volume, 10 pan, 11 expression, 64 sustain, 120,
   121, 123; Apogee's EMIDI 110 to 113 and 116 to 119 (tracks by card, programs,
   volumes, loops). Channel 10 (9 from 0) is the drums: bank 128.

   Under the kernel (mx/snd.h): the song pauses while another program is in
   front, and goes on at this program's next call of mx/snd.h or here after it
   is back; a game calls mx_snd_commit once a frame at least. */

#include <mx/types.h>

/* Load bank (size bytes, the file): its samples into SPU RAM, its tables into
   IOP memory. bank stays in use (read again when another program took the SPU
   RAM) until mx_midi_free. mx_snd_init first. 0, < 0: not a bank, or no room. */
int mx_midi_load(const void *bank, u32 size);
/* Stop the song and free it and the bank. */
void mx_midi_free(void);

/* Hand over a song (copied to IOP memory; the one before is stopped and
   freed). 0, < 0: not one, or no room. */
int mx_midi_song(const void *smf, u32 size);
/* Play it from its start; loop: from the start again at its end. */
int mx_midi_play(int loop);
void mx_midi_stop(void);
void mx_midi_pause(int on);
/* 0 .. 256 (the default). */
void mx_midi_volume(int volume);
/* 1: a song plays (or is paused). */
int mx_midi_playing(void);

#endif
