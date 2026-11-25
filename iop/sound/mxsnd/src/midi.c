/* The synthesiser and the sequencer (midi.h). A note is a voice per zone that
   matches its key and velocity; the SPU2 does its envelope. The sequencer
   takes Apogee's EMIDI controllers as Build games' songs have them: 110 and
   111 (a track for this card or not), 112 and 113 (the track's program and
   volume in place of the plain ones), 116 and 117 (a track's loop), 118 and
   119 (the song's). Its contexts (114, 115) are left out. */

#include "midi.h"
#include "midi_fix.h"

/* ---- the synthesiser ---------------------------------------------------------- */

static const struct midi_bank_head *head;
static const struct midi_bank_sample *bsmp;
static const struct midi_bank_preset *bpre;
static const struct midi_bank_zone *bzone;
static unsigned int spu_base;
static int master = 256;

static struct {
  unsigned char program, vol, expr, pan, sustain;
  unsigned short rpn;
  short bend;  /* -8192 .. 8191 */
  short range; /* the bend's, cents */
} chan[16];

enum { FREE, HELD, RELEASED };
static struct voice {
  unsigned char state, ch, key, sustained;
  unsigned char fresh; /* keyed on in this step: its off waits for the next one */
  unsigned char off;   /* the off that waits */
  unsigned char loop;
  const struct midi_bank_zone *z;
  unsigned int gain, base, order;
} voices[MIDI_VOICES];
static unsigned int order;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void levels(const struct voice *n, unsigned int *pitch, unsigned int *l, unsigned int *r) {
  const struct midi_bank_zone *z = n->z;
  *pitch = midi_pitch(n->base,
                      midi_cents(n->key, z->root, z->scale, z->tune, chan[n->ch].bend, chan[n->ch].range));
  midi_volumes(n->gain, chan[n->ch].vol, chan[n->ch].expr, master, z->pan, chan[n->ch].pan, l, r);
}

static void voice_stop(int v) {
  struct voice *n = &voices[v];
  if (n->state != HELD)
    return;
  if (n->fresh) { /* on and off in one step would not sound */
    n->off = 1;
    return;
  }
  midi_voice_off(v);
  n->state = RELEASED, n->sustained = 0, n->off = 0, n->order = ++order;
}

static void all_off(void) {
  for (int v = 0; v < MIDI_VOICES; v++)
    if (voices[v].state == HELD) {
      midi_voice_off(v);
      voices[v].state = RELEASED, voices[v].off = 0, voices[v].order = ++order;
    }
}

static void chan_update(int ch) {
  for (int v = 0; v < MIDI_VOICES; v++)
    if (voices[v].state != FREE && voices[v].ch == ch) { /* a release follows too */
      unsigned int pitch, l, r;
      levels(&voices[v], &pitch, &l, &r);
      midi_voice_set(v, pitch, l, r);
    }
}

/* a voice: free, or past its end, the one released longest ago, the oldest */
static int voice_get(void) {
  int rel = -1, old = -1;
  for (int v = 0; v < MIDI_VOICES; v++) {
    struct voice *n = &voices[v];
    if (n->state != FREE && !n->fresh && !n->loop && midi_voice_ended(v))
      n->state = FREE;
    if (n->state == FREE)
      return v;
    int *best = n->state == RELEASED ? &rel : &old;
    if (*best < 0 || (int)(n->order - voices[*best].order) < 0)
      *best = v;
  }
  return rel >= 0 ? rel : old;
}

static const struct midi_bank_preset *preset(int bank, int program) {
  const struct midi_bank_preset *any = 0;
  for (unsigned int i = 0; i < head->npresets; i++)
    if (bpre[i].bank == bank) {
      if (bpre[i].program == program)
        return &bpre[i];
      if (!any || bpre[i].program == 0)
        any = &bpre[i];
    }
  return any ? any : head->npresets ? &bpre[0] : 0;
}

static void note_off(int ch, int key) {
  for (int v = 0; v < MIDI_VOICES; v++)
    if (voices[v].state == HELD && !voices[v].sustained && voices[v].ch == ch && voices[v].key == key) {
      if (chan[ch].sustain)
        voices[v].sustained = 1;
      else
        voice_stop(v);
    }
}

static void note_on(int ch, int key, int vel) {
  /* a key struck again: its voices go, also those the pedal holds */
  for (int v = 0; v < MIDI_VOICES; v++)
    if (voices[v].state == HELD && voices[v].ch == ch && voices[v].key == key)
      voice_stop(v);
  const struct midi_bank_preset *p = preset(ch == 9 ? 128 : 0, chan[ch].program);
  if (!p)
    return;
  unsigned int first = ++order;
  for (int i = 0; i < p->nzones; i++) {
    const struct midi_bank_zone *z = &bzone[p->zone + i];
    if (key < z->keylo || key > z->keyhi || vel < z->vello || vel > z->velhi)
      continue;
    if (z->excl)
      for (int v = 0; v < MIDI_VOICES; v++)
        if (voices[v].state == HELD && voices[v].ch == ch && voices[v].z->excl == z->excl &&
            voices[v].order != first)
          voice_stop(v);
    int v = voice_get();
    struct voice *n = &voices[v];
    const struct midi_bank_sample *s = &bsmp[z->sample];
    n->state = HELD, n->fresh = 1, n->off = 0, n->sustained = 0;
    n->ch = (unsigned char)ch, n->key = (unsigned char)key, n->z = z, n->loop = s->loop != 0;
    n->order = first, n->base = midi_base(s->rate), n->gain = midi_note_gain(vel, z->atten);
    unsigned int pitch, l, r;
    levels(n, &pitch, &l, &r);
    midi_voice_on(v, spu_base + s->offset, pitch, l, r, z->adsr1, z->adsr2);
  }
}

static void chan_reset(int ch) {
  chan[ch].vol = 100, chan[ch].expr = 127, chan[ch].pan = 64, chan[ch].sustain = 0;
  chan[ch].rpn = 0x3FFF, chan[ch].bend = 0, chan[ch].range = 200;
}

static void synth_reset(void) {
  all_off();
  for (int ch = 0; ch < 16; ch++) {
    chan_reset(ch);
    chan[ch].program = 0;
  }
}

static void control(int ch, int c, int d) {
  switch (c) {
  case 6: /* data entry: the bend's range in semitones */
    if (chan[ch].rpn == 0)
      chan[ch].range = (short)(chan[ch].range % 100 + clampi(d, 0, 24) * 100);
    break;
  case 38:
    if (chan[ch].rpn == 0)
      chan[ch].range = (short)(chan[ch].range / 100 * 100 + clampi(d, 0, 99));
    break;
  case 7:
    chan[ch].vol = (unsigned char)d;
    chan_update(ch);
    break;
  case 10:
    chan[ch].pan = (unsigned char)d;
    chan_update(ch);
    break;
  case 11:
    chan[ch].expr = (unsigned char)d;
    chan_update(ch);
    break;
  case 64:
    chan[ch].sustain = d >= 64;
    if (d < 64)
      for (int v = 0; v < MIDI_VOICES; v++)
        if (voices[v].state == HELD && voices[v].ch == ch && voices[v].sustained)
          voice_stop(v);
    break;
  case 98: /* another parameter, not the bend's range */
  case 99:
    chan[ch].rpn = 0x3FFF;
    break;
  case 100:
    chan[ch].rpn = (unsigned short)((chan[ch].rpn & 0x3F80) | d);
    break;
  case 101:
    chan[ch].rpn = (unsigned short)((chan[ch].rpn & 0x7F) | d << 7);
    break;
  case 120: /* all sound off */
  case 123: /* all notes off */
    for (int v = 0; v < MIDI_VOICES; v++)
      if (voices[v].state == HELD && voices[v].ch == ch) {
        if (c == 123 && chan[ch].sustain)
          voices[v].sustained = 1;
        else
          voice_stop(v);
      }
    break;
  case 121:
    chan_reset(ch);
    chan_update(ch);
    break;
  }
}

#ifdef MIDI_TRACE
void midi_trace(int status, int d1, int d2);
#endif
static void message(int status, int d1, int d2) {
#ifdef MIDI_TRACE
  midi_trace(status, d1, d2);
#endif
  int ch = status & 15;
  switch (status & 0xF0) {
  case 0x80:
    note_off(ch, d1);
    break;
  case 0x90:
    if (d2)
      note_on(ch, d1, d2);
    else
      note_off(ch, d1);
    break;
  case 0xB0:
    control(ch, d1, d2);
    break;
  case 0xC0:
    chan[ch].program = (unsigned char)d1;
    break;
  case 0xE0:
    chan[ch].bend = (short)((d1 | d2 << 7) - 8192);
    chan_update(ch);
    break;
  }
}

/* ---- the sequencer ------------------------------------------------------------ */

#define EVENTS_MAX 8192 /* in one tick: more is a loop without time in it */

static struct track {
  const unsigned char *start, *end, *pos;
  unsigned int delay;
  unsigned char active, running;
  unsigned char include, program, volume; /* EMIDI's */
  /* the loop: where it starts, how often yet (-1: for ever, 0: none) */
  const unsigned char *lpos;
  unsigned int ldelay;
  short lcount;
  unsigned char lrunning, lactive;
} track[MIDI_TRACKS];
static int ntracks;
static unsigned int division, tempo; /* ticks a quarter note, microseconds of one */
static unsigned int acc;
static int playing, paused, looping;

static unsigned int be(const unsigned char *p, int n) {
  unsigned int v = 0;
  while (n--)
    v = v << 8 | *p++;
  return v;
}
/* the next byte, 0 past the end (a track's end is found by its position) */
static int next(struct track *t) { return t->pos < t->end ? *t->pos++ : 0; }
static unsigned int vlq(struct track *t) {
  unsigned int v = 0;
  for (int i = 0; i < 4; i++) {
    int b = next(t);
    v = v << 7 | (unsigned int)(b & 127);
    if (b < 128)
      break;
  }
  return v;
}
static void skip(struct track *t, unsigned int n) {
  t->pos += n < (unsigned int)(t->end - t->pos) ? n : (unsigned int)(t->end - t->pos);
}

/* an event: its status (0xFF: the meta event d1, its bytes at *data, d2 of
   them; 0xF0: others, skipped); -1 at the end of the track's bytes */
static int event(struct track *t, int *d1, int *d2, const unsigned char **data) {
  if (t->pos >= t->end)
    return -1;
  int s = next(t);
  *d1 = *d2 = 0;
  if (s == 0xFF) {
    *d1 = next(t);
    *d2 = (int)vlq(t);
    *data = t->pos;
    skip(t, (unsigned int)*d2);
    *d2 = (int)(t->pos - *data);
    return s;
  }
  if (s >= 0xF0) {
    skip(t, vlq(t));
    return 0xF0;
  }
  if (s & 0x80)
    t->running = (unsigned char)s;
  else {
    s = t->running;
    t->pos--;
    if (!s) { /* no status yet: not a track */
      t->pos = t->end;
      return -1;
    }
  }
  *d1 = next(t) & 127;
  if ((s & 0xE0) != 0xC0)
    *d2 = next(t) & 127;
  return s;
}

static void tracks_reset(void) {
  for (int i = 0; i < ntracks; i++) {
    struct track *t = &track[i];
    t->pos = t->start, t->running = 0;
    t->delay = vlq(t);
    t->active = t->include;
    t->lpos = 0, t->lcount = 0;
  }
}

/* what its EMIDI controllers say of a track, anywhere in it */
static void track_scan(struct track *t) {
  int found = 0, d1, d2, s;
  const unsigned char *data;
  t->include = 1, t->program = t->volume = 0;
  t->pos = t->start, t->running = 0;
  vlq(t);
  while ((s = event(t, &d1, &d2, &data)) >= 0 && !(s == 0xFF && d1 == 0x2F)) {
    if ((s & 0xF0) == 0xB0) {
      int ours = d2 == 127 || d2 == 0; /* all cards, General MIDI */
      if (d1 == 110 && ours)
        found = 1, t->include = 1;
      else if (d1 == 110 && !found)
        found = 1, t->include = 0;
      else if (d1 == 111 && ours)
        t->include = 0;
      else if (d1 == 112)
        t->program = 1;
      else if (d1 == 113)
        t->volume = 1;
    }
    vlq(t);
  }
}

static void controller(struct track *t, int ch, int c, int d) {
  struct track *a = t, *e = t + 1;
  if (c == 118 || c == 119)
    a = track, e = track + ntracks;
  switch (c) {
  case 7:
    if (!t->volume)
      message(0xB0 | ch, 7, d);
    break;
  case 113:
    message(0xB0 | ch, 7, d);
    break;
  case 112:
    message(0xC0 | ch, d, 0);
    break;
  case 110:
  case 111:
  case 114:
  case 115:
    break;
  case 116:
  case 118:
    for (; a < e; a++) {
      a->lcount = (short)(d ? d : -1);
      a->lpos = a->pos, a->lrunning = a->running, a->ldelay = a->delay, a->lactive = a->active;
    }
    break;
  case 117:
  case 119:
    if (d != 127 || !t->lpos || !t->lcount)
      break;
    for (; a < e; a++) {
      if (!a->lpos)
        continue;
      if (a->lcount > 0)
        a->lcount--;
      a->pos = a->lpos, a->running = a->lrunning, a->delay = a->ldelay, a->active = a->lactive;
    }
    break;
  default:
    message(0xB0 | ch, c, d);
  }
}

/* the song's end in it: from the start again, in the same tick */
static void tick(int again) {
  int events = 0, active = 0;
  for (int i = 0; i < ntracks; i++) {
    struct track *t = &track[i];
    while (t->active && !t->delay) {
      int d1, d2;
      const unsigned char *data;
      int s = event(t, &d1, &d2, &data);
      if (s < 0 || (s == 0xFF && d1 == 0x2F)) {
        t->active = 0;
        break;
      }
      if (++events > EVENTS_MAX) {
        midi_stop();
        return;
      }
      if (s == 0xFF) {
        if (d1 == 0x51 && d2 == 3) {
          tempo = be(data, 3);
          if (tempo < 20000) /* 3000 a minute: not music, and a step's ticks have an end */
            tempo = 20000;
        }
      } else if ((s & 0xF0) == 0xB0)
        controller(t, s & 15, d1, d2);
      else if ((s & 0xF0) == 0xC0) {
        if (!t->program)
          message(s, d1, 0);
      } else if (s != 0xF0)
        message(s, d1, d2);
      t->delay = vlq(t);
    }
    if (t->active)
      t->delay--;
  }
  for (int i = 0; i < ntracks; i++)
    active += track[i].active;
  if (!active) {
    if (looping)
      tracks_reset();
    if (looping && !again)
      tick(1);
    else if (!looping)
      midi_stop();
  }
}

/* ---- the interface ------------------------------------------------------------- */

int midi_bank(const void *tables, unsigned int bytes, unsigned int spu) {
  midi_stop();
  head = 0;
  midi_forget();
  if (!tables)
    return 0;
  const struct midi_bank_head *h = tables;
  if (bytes < sizeof *h || h->magic != MIDI_BANK_MAGIC || h->version != 1 || h->data > bytes ||
      h->nsamples > 65536 || h->npresets > 65536 || h->nzones > 65536 ||
      sizeof *h + h->nsamples * sizeof *bsmp + h->npresets * sizeof *bpre + h->nzones * sizeof *bzone > h->data)
    return -1;
  bsmp = (const void *)(h + 1);
  bpre = (const void *)(bsmp + h->nsamples);
  bzone = (const void *)(bpre + h->npresets);
  for (unsigned int i = 0; i < h->npresets; i++)
    if (bpre[i].zone + bpre[i].nzones > h->nzones)
      return -1;
  for (unsigned int i = 0; i < h->nzones; i++)
    if (bzone[i].sample >= h->nsamples)
      return -1;
  for (unsigned int i = 0; i < h->nsamples; i++)
    if (bsmp[i].rate > 192000 || bsmp[i].offset > h->adpcm || bsmp[i].size > h->adpcm - bsmp[i].offset)
      return -1;
  spu_base = spu;
  head = h;
  synth_reset();
  return 0;
}

int midi_song(const void *smf, unsigned int bytes) {
  const unsigned char *p = smf, *e = p + bytes;
  midi_stop();
  ntracks = 0;
  if (!smf)
    return 0;
  if (bytes < 14 || be(p, 4) != 0x4D546864 || be(p + 4, 4) < 6 || be(p + 4, 4) > bytes - 8 || be(p + 8, 2) > 1)
    return -1;
  unsigned int n = be(p + 10, 2);
  division = be(p + 12, 2);
  if (!division || division & 0x8000)
    division = 96;
  if (!n || n > MIDI_TRACKS)
    return -1;
  p += 8 + be(p + 4, 4);
  for (unsigned int i = 0; i < n; i++) {
    if (e - p < 8 || be(p, 4) != 0x4D54726B || be(p + 4, 4) > (unsigned int)(e - p) - 8)
      return -1;
    track[i].start = p + 8;
    p += 8 + be(p + 4, 4);
    track[i].end = p;
    track_scan(&track[i]);
  }
  ntracks = (int)n;
  return 0;
}

void midi_play(int loop) {
  midi_stop();
  if (!head || !ntracks)
    return;
  synth_reset();
  tracks_reset();
  tempo = 500000, acc = 0, looping = loop, paused = 0, playing = 1;
}

void midi_stop(void) {
  all_off();
  playing = paused = 0;
}

void midi_pause(int on) {
  if (on)
    all_off();
  paused = on;
}

void midi_forget(void) {
  for (int v = 0; v < MIDI_VOICES; v++)
    voices[v].state = FREE, voices[v].off = voices[v].fresh = 0;
}

void midi_volume(int volume) {
  master = clampi(volume, 0, 256);
  for (int ch = 0; ch < 16; ch++)
    chan_update(ch);
}

int midi_playing(void) { return playing; }

void midi_step(void) {
  for (int v = 0; v < MIDI_VOICES; v++) {
    voices[v].fresh = 0;
    if (voices[v].off)
      voice_stop(v);
  }
  if (!playing || paused)
    return;
  /* 512 frames are 32000 / 3 us, a tick tempo / division us */
  acc += 32000u * division;
  while (playing && acc >= 3 * tempo) {
    acc -= 3 * tempo;
    tick(0);
  }
}
