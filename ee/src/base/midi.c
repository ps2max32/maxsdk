/* MIDI music (mx/midi.h): the calls. mxsnd plays it on the IOP
   (iop/sound/mxsnd/src/midi.c); here are the bank's samples in SPU RAM, the
   hand-over of the tables and songs, and both again after another program. */

#include <mx/midi.h>
#include <mx/snd.h>
#include <mxsnd.h>

void mx__snd_music(int on);
int mx__snd_music_data(int what, const void *data, u32 bytes, u32 spu);
int mx__snd_music_mine(void);
int mx__snd_music_cmd(int op, u32 value, u32 *at);
int mx__snd_music_status(u32 at, int *done);

/* the bank's head (tools/sf2bank.c, little endian): the tables follow it, up to
   data, then adpcm bytes of SPU ADPCM */
struct head {
  u32 magic, version, nsamples, npresets, nzones, adpcm, data, pad;
};
#define MAGIC 0x4D47584Du /* "MXGM" */

static const struct head *bank;
static struct mx_snd_sample whole;
static const void *song; /* while it plays */
static u32 song_size, at;
static int have_song, loops, paused, volume = 256, wanted;

int mx_midi_load(const void *data, u32 size) {
  const struct head *h = data;
  mx_midi_free();
  if (size < sizeof *h || h->magic != MAGIC || h->version != 1 || h->data > size || h->adpcm > size - h->data ||
      (h->data & 15) || mx_snd_load(&whole, (const u8 *)data + h->data, h->adpcm, 48000) < 0)
    return -1;
  if (mx__snd_music_data(MXSND_M_BANK, data, h->data, whole.spu) < 0) {
    mx_snd_free(&whole);
    return -1;
  }
  bank = h;
  mx__snd_music(1);
  mx_midi_volume(volume);
  return 0;
}

void mx_midi_free(void) {
  if (!bank)
    return;
  wanted = have_song = 0, song = 0;
  mx__snd_music_data(MXSND_M_SONG, 0, 0, 0);
  mx__snd_music_data(MXSND_M_BANK, 0, 0, 0); /* its voices are silent when this is back */
  bank = 0;
  mx_snd_free(&whole);
  mx__snd_music(0);
}

int mx_midi_song(const void *smf, u32 size) {
  wanted = have_song = 0;
  if (!bank || !size || mx__snd_music_data(MXSND_M_SONG, smf, size, 0) < 0)
    return -1;
  song = smf, song_size = size, have_song = 1;
  return 0;
}

int mx_midi_play(int loop) {
  if (!have_song || mx__snd_music_cmd(MXSND_MO_PLAY, loop != 0, &at) < 0)
    return -1;
  wanted = 1, loops = loop, paused = 0;
  return 0;
}

void mx_midi_stop(void) {
  wanted = 0;
  if (bank)
    mx__snd_music_cmd(MXSND_MO_STOP, 0, &at);
}

void mx_midi_pause(int on) {
  paused = on != 0;
  if (bank)
    mx__snd_music_cmd(MXSND_MO_PAUSE, (u32)paused, &at);
}

void mx_midi_volume(int v) {
  volume = v < 0 ? 0 : v > 256 ? 256 : v;
  if (bank)
    mx__snd_music_cmd(MXSND_MO_VOLUME, (u32)volume, &at);
}

int mx_midi_playing(void) {
  int done, s;
  if (wanted && (s = mx__snd_music_status(at, &done)) >= 0 && done && !s)
    wanted = 0; /* it ended */
  return wanted;
}

/* snd.c: the sound is this program's again. The samples and, when another
   program had music, the tables and the song again (from its start) */
void mx__snd_front(void) {
  if (!bank)
    return;
  if (mx_snd_lost(&whole) && mx_snd_reload(&whole, (const u8 *)bank + bank->data) < 0)
    return;
  if (mx__snd_music_mine() || (!have_song && !wanted)) {
    mx__snd_music_cmd(MXSND_MO_FRONT, 0, &at);
    return;
  }
  have_song = have_song && wanted;
  if (mx__snd_music_data(MXSND_M_BANK, bank, bank->data, whole.spu) < 0) {
    wanted = have_song = 0;
    return;
  }
  mx_midi_volume(volume);
  if (have_song && mx__snd_music_data(MXSND_M_SONG, song, song_size, 0) == 0) {
    int p = paused;
    mx_midi_play(loops);
    if (p)
      mx_midi_pause(1);
  } else
    wanted = have_song = 0;
}
