/* A movie from a file (<mx/movie.h>): a thread reads it, the caller's
   thread takes the program stream apart, decodes on the IPU and feeds the
   sound. */

#include <mx/cpu.h>
#include <mx/mem.h>
#include <mx/movie.h>
#include <mx/mp2.h>
#include <mx/mpegps.h>
#include <mx/sif.h>
#include <mx/snd.h>
#include <mx/string.h>
#include <mx/thread.h>

#define READ (64u << 10)       /* a read of the file */
#define CHUNKS 4               /* reads the thread may be ahead */
#define RAW_MAX (READ + 8192)  /* a read and the packet cut off by the one before */
#define PCM_FRAMES 144000u     /* 3 s of sound waiting */
#define MPA_MAX 8192u          /* MPEG audio not decoded yet: a frame is 1728 bytes at most */
#define HZ 294912000ull        /* the EE's clock, mx_count */

struct mx_movie {
  struct mx_movie_info info;
  struct mx_mpeg mpeg;
  int fd;
  u8 is_ps, at_end, at_picture, stop;

  /* the video stream, read by the EE (headers) and the DMAC */
  u8 *es;
  u32 es_max, ahead, have, pos;
  u8 *pix;
  u8 *frames; /* a stream with P and B pictures: mx_mpeg_refs' memory */
  u8 flushed;
  u8 loop, paused, over;
  u32 played; /* pictures given out since the start or the last rewind */
  u32 decoded;
  u64 dec_cycles;

  /* the reading thread: chunk[] filled in turn while one is free */
  u8 *chunk[CHUNKS];
  int chunk_len[CHUNKS];
  u32 next;
  int sem_ready, sem_free, thread;
  void *stack;
  u8 *raw; /* a program stream's bytes not yet taken apart */
  u32 raw_have;

  /* the sound: samples waiting (48 kHz, 16 bits, stereo), fed to mxsnd's
     stream through a linear interpolation whose step follows the picture */
  s16 *pcm;
  u32 pcm_rd, pcm_wr; /* frames */
  u32 rate;           /* its samples a second (0: no sound met yet) */
  struct mx_mp2 mp2;  /* MPEG audio: the decoder, and the stream's bytes before it */
  u8 mpa[MPA_MAX];
  u32 mpa_have;
  u8 mpa_seen;        /* the file's sound is MPEG audio (else LPCM) */
  u8 want_sound, started;
  u32 step, phase;   /* 16.16 source frames an output frame */
  u32 src;           /* source frames gone into the stream */
  u32 silence;       /* frames of silence to play first */
  u32 offset;        /* the sound's first frame belongs this many frames after picture 0 */
  u32 v_pts, a_pts;
  u8 v_pts_set, a_pts_set;
  u32 shown;         /* pictures shown */

  /* mx_movie_update's clock */
  u8 running;
  u32 last;   /* mx_count at the last call */
  u64 cycles; /* since the first */
};

static struct mx_movie *ipu_user; /* whose tables the IPU holds */

/* ---- sound ------------------------------------------------------------------ */

/* Room for n more frames of samples waiting: where to write them, 0 if
   there is none (the pictures stand still and the sound piles up). */
static s16 *pcm_room(struct mx_movie *m, u32 n) {
  if (m->pcm_wr + n > PCM_FRAMES) {
    memmove(m->pcm, m->pcm + m->pcm_rd * 2, (m->pcm_wr - m->pcm_rd) * 4);
    m->pcm_wr -= m->pcm_rd;
    m->pcm_rd = 0;
  }
  return m->pcm_wr + n > PCM_FRAMES ? 0 : m->pcm + m->pcm_wr * 2;
}

/* LPCM: 16 bits, the high byte first */
static void put_lpcm(struct mx_movie *m, const u8 *be, u32 bytes) {
  u32 n = bytes / 4;
  s16 *d = m->want_sound ? pcm_room(m, n) : 0;
  if (!d)
    return;
  for (u32 i = 0; i < n * 2; i++)
    d[i] = (s16)(be[i * 2] << 8 | be[i * 2 + 1]);
  m->pcm_wr += n;
  m->rate = 48000;
}

/* MPEG audio layer II: the packets' bytes gathered, whole frames decoded */
static void put_mpa(struct mx_movie *m, const u8 *data, u32 bytes) {
  if (!m->want_sound)
    return;
  while (bytes) {
    u32 n = bytes < MPA_MAX - m->mpa_have ? bytes : MPA_MAX - m->mpa_have, at = 0;
    memcpy(m->mpa + m->mpa_have, data, n);
    m->mpa_have += n, data += n, bytes -= n;
    for (;;) {
      s16 *d = pcm_room(m, MX_MP2_SAMPLES);
      u32 got = 0, rate = 0, used = d ? mx_mp2_decode(&m->mp2, m->mpa + at, m->mpa_have - at, d, &got, &rate) : 0;
      if (!used)
        break;
      at += used;
      if (got && (!m->rate || rate == m->rate)) {
        m->pcm_wr += got;
        m->rate = rate;
      }
    }
    if (!at && m->mpa_have == MPA_MAX)
      at = MPA_MAX; /* no room for samples: this sound is lost */
    memmove(m->mpa, m->mpa + at, m->mpa_have - at);
    m->mpa_have -= at;
  }
}

static void pump(struct mx_movie *m) {
  s16 out[256 * 2];
  if (!m->started || !m->rate)
    return;
  m->info.sound_left_ms = (m->pcm_wr - m->pcm_rd + mx_snd_stream_queued() / 4) * 1000 / m->rate;
  u32 room = mx_snd_stream_room() / 4;
  if (room && m->src && mx_snd_stream_queued() == 0)
    m->info.sound_dry++;
  while (room) {
    u32 n = 0, max = room < 256 ? room : 256;
    for (; n < max && m->silence; n++, m->silence--)
      out[n * 2] = out[n * 2 + 1] = 0;
    for (; n < max; n++) {
      u32 i = m->pcm_rd + (m->phase >> 16);
      s32 f = (s32)(m->phase & 0xFFFF);
      if (i + 1 >= m->pcm_wr)
        break;
      const s16 *s = m->pcm + i * 2;
      out[n * 2] = (s16)(s[0] + (((s[2] - s[0]) * f) >> 16));
      out[n * 2 + 1] = (s16)(s[1] + (((s[3] - s[1]) * f) >> 16));
      m->phase += m->step;
    }
    m->pcm_rd += m->phase >> 16;
    m->src += m->phase >> 16;
    m->phase &= 0xFFFF;
    if (!n)
      break;
    for (u32 done = 0; done < n * 4;) {
      u32 k = mx_snd_stream_queue((const u8 *)out + done, n * 4 - done);
      if (!k)
        break;
      done += k;
    }
    room -= n;
  }
}

/* The sound's step from how far it is behind the picture: 20 ms behind make
   it a thousandth faster, 3 at most. Far off (the stream's start, a stall):
   a jump, over samples or with silence. */
static void sync(struct mx_movie *m) {
  if (!m->started || !m->info.rate_num || m->silence)
    return;
  s32 seen = (s32)((u64)m->shown * m->rate * m->info.rate_den / m->info.rate_num) - (s32)m->offset;
  s32 far = (s32)m->rate * 60 / 1000; /* 60 ms */
  s32 heard = (s32)m->src - (s32)(mx_snd_stream_queued() / 4);
  s32 behind = seen - heard;
  m->info.sound_behind_ms = behind * 1000 / (s32)m->rate;
  if (behind > far && (u32)behind < m->pcm_wr - m->pcm_rd) {
    m->pcm_rd += (u32)behind;
    m->src += (u32)behind;
    m->info.sound_jumps++;
    return;
  }
  if (behind < -far) {
    m->silence += (u32)-behind; /* the picture catches up while it plays */
    m->info.sound_jumps++;
    return;
  }
  s32 d = behind * 65536 / ((s32)m->rate * 20);
  d = d > 196 ? 196 : d < -196 ? -196 : d;
  m->step = (u32)(65536 + d);
}

/* ---- the file ---------------------------------------------------------------- */

static void reader(void *arg) {
  struct mx_movie *m = arg;
  for (u32 w = 0;; w = (w + 1) % CHUNKS) {
    mx_sema_wait(m->sem_free);
    if (m->stop)
      return;
    int n = mx_fio_read(m->fd, m->chunk[w], READ);
    m->chunk_len[w] = n;
    mx_sema_signal(m->sem_ready);
    if (n <= 0)
      return;
  }
}

/* The reader's next piece into es (and pcm). wait: sleep until it has one;
   else only if it has. Returns 0 without one: none ready, no room in es, or
   the file's end was reached before. */
static int fill(struct mx_movie *m, int wait) {
  if (m->at_end)
    return 0;
  if (m->have + READ > m->es_max) {
    /* what lies before pos goes: the rest moves down by whole quadwords, so
       pos keeps its place in one */
    u32 base = m->pos & ~15u;
    if (!base)
      return 0;
    memmove(m->es, m->es + base, m->have - base);
    m->have -= base;
    m->pos -= base;
    mx_dcache_writeback(m->es, m->have);
    if (m->have + READ > m->es_max)
      return 0; /* a picture larger than the buffer */
  }
  if (wait)
    mx_sema_wait(m->sem_ready);
  else if (!mx_sema_try(m->sem_ready))
    return 0;
  u32 from = m->have;
  const u8 *c = m->chunk[m->next];
  int n = m->chunk_len[m->next];
  if (n > 0 && !m->is_ps) {
    memcpy(m->es + m->have, c, (u32)n);
    m->have += (u32)n;
  } else if (n > 0) {
    struct mx_ps_packet p;
    u32 at = 0;
    int r;
    memcpy(m->raw + m->raw_have, c, (u32)n);
    m->raw_have += (u32)n;
    while ((r = mx_ps_next(m->raw, m->raw_have, &at, &p)) != MX_PS_MORE && r != MX_PS_END) {
      if (r == MX_PS_VIDEO && p.stream == 0 && m->have + p.bytes <= m->es_max) {
        if (p.has_pts && !m->v_pts_set)
          m->v_pts = p.pts, m->v_pts_set = 1;
        memcpy(m->es + m->have, p.data, p.bytes);
        m->have += p.bytes;
      } else if (r == MX_PS_LPCM && p.stream == 0 && p.bits == 16 && p.channels == 2 &&
                 p.rate == 48000 && !m->mpa_seen) {
        if (p.has_pts && !m->a_pts_set)
          m->a_pts = p.pts, m->a_pts_set = 1;
        put_lpcm(m, p.data, p.bytes);
      } else if (r == MX_PS_MPA && p.stream == 0 && (m->mpa_seen || !m->rate)) {
        if (p.has_pts && !m->a_pts_set)
          m->a_pts = p.pts, m->a_pts_set = 1;
        m->mpa_seen = 1;
        put_mpa(m, p.data, p.bytes);
      }
    }
    memmove(m->raw, m->raw + at, m->raw_have - at);
    m->raw_have -= at;
  } else {
    /* the end: an end code and what the decoder wants after it; if the file
       was cut off, its last picture is lost */
    memset(m->es + m->have, 0, 64);
    m->es[m->have + 2] = 1;
    m->es[m->have + 3] = 0xB7;
    m->have += 4 + MX_MPEG_AHEAD;
    m->at_end = 1;
  }
  mx_dcache_writeback(m->es + (from & ~63u), m->have + 64 - (from & ~63u));
  m->next = (m->next + 1) % CHUNKS;
  if (n > 0)
    mx_sema_signal(m->sem_free);
  pump(m);
  return 1;
}

/* Read on while the decoder has little before it, or the sound that goes
   with the pictures to come is not here yet (mxsnd's stream holds a tenth
   of a second: the samples must be on the EE well before they are heard). */
static void ahead(struct mx_movie *m) {
  while ((m->have - m->pos < m->ahead ||
          (m->started && m->pcm_wr - m->pcm_rd < m->rate / 4)) &&
         fill(m, 0))
    ;
}

static void reader_stop(struct mx_movie *m);
static int reader_start(struct mx_movie *m);

/* ---- the calls ---------------------------------------------------------------- */

void mx_movie_close(struct mx_movie *m) {
  if (!m)
    return;
  reader_stop(m);
  if (m->started)
    mx_snd_stream_stop();
  if (m->fd >= 0)
    mx_fio_close(m->fd);
  if (m->sem_ready >= 0)
    mx_sema_delete(m->sem_ready);
  if (m->sem_free >= 0)
    mx_sema_delete(m->sem_free);
  if (ipu_user == m)
    ipu_user = 0;
  void *pinned[] = {m->es, m->pix, m->stack, m->frames};
  void *paged[] = {m->raw, m->pcm, m->chunk[0], m};
  for (u32 i = 0; i < 4; i++)
    if (pinned[i])
      mx_dma_free(pinned[i]);
  for (u32 i = 0; i < 4; i++)
    if (paged[i])
      mx_heap_free(mx_heap_paged, paged[i]);
}

struct mx_movie *mx_movie_open(const char *path, u32 flags) {
  struct mx_movie *m = mx_heap_alloc(mx_heap_paged, sizeof *m, 64);
  if (!m)
    return 0;
  memset(m, 0, sizeof *m);
  m->sem_ready = m->sem_free = m->thread = -1;
  m->step = 65536;
  m->info.rgb16 = (flags & MX_MOVIE_RGB16) != 0;
  m->loop = (flags & MX_MOVIE_LOOP) != 0;
  m->want_sound = !(flags & MX_MOVIE_NO_SOUND);
  m->fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (m->fd < 0)
    goto fail;
  /* a program stream starts with a pack header */
  u8 head[4] = {0};
  mx_fio_read(m->fd, head, 4);
  mx_fio_lseek(m->fd, 0, MX_FIO_SEEK_SET);
  m->is_ps = head[0] == 0 && head[1] == 0 && head[2] == 1 && head[3] == 0xBA;

  /* the larger buffer until the picture's size is known */
  m->es_max = 1536u << 10;
  m->es = mx_dma_alloc(m->es_max + 64);
  m->stack = mx_dma_alloc(16384);
  m->raw = mx_heap_alloc(mx_heap_paged, RAW_MAX, 64);
  m->pcm = mx_heap_alloc(mx_heap_paged, PCM_FRAMES * 4, 64);
  m->chunk[0] = mx_heap_alloc(mx_heap_paged, CHUNKS * READ, 64);
  if (!m->es || !m->stack || !m->raw || !m->pcm || !m->chunk[0])
    goto fail;
  for (u32 i = 1; i < CHUNKS; i++)
    m->chunk[i] = m->chunk[0] + i * READ;
  if (reader_start(m) < 0)
    goto fail;

  mx_mpeg_init(&m->mpeg, m->info.rgb16 ? MX_IPU_RGB16 | MX_IPU_DITHER : 0);
  int r;
  while ((r = mx_mpeg_headers(&m->mpeg, m->es, m->have, &m->pos)) == MX_MPEG_MORE)
    if (!fill(m, 1))
      break;
  if (r != MX_MPEG_PICTURE)
    goto fail;
  m->at_picture = 1;
  m->info.width = m->mpeg.width, m->info.height = m->mpeg.height;
  m->info.mbw = m->mpeg.mbw, m->info.mbh = m->mpeg.mbh;
  m->info.rate_num = m->mpeg.rate_num, m->info.rate_den = m->mpeg.rate_den;
  u32 mbs = (u32)m->mpeg.mbw * m->mpeg.mbh;
  m->ahead = mbs > 1620 ? 448u << 10 : 192u << 10;
  m->pix = mx_dma_alloc(mbs * (m->info.rgb16 ? 512 : 1024));
  if (!m->pix)
    goto fail;
  /* other pictures than I: look at the second picture's type (it is within
     the first megabyte), and take the memory for the references */
  int next;
  while (!(next = mx_mpeg_next_type(m->es, m->have, m->pos + 4)) && m->have < (1024u << 10))
    if (!fill(m, 1))
      break;
  if (next == MX_MPEG_P || next == MX_MPEG_B) {
    m->frames = mx_dma_alloc(MX_MPEG_REF_BYTES(m->mpeg.mbw, m->mpeg.mbh));
    if (!m->frames)
      goto fail;
    mx_mpeg_refs(&m->mpeg, m->frames, 0);
    m->info.predicted = 1;
  }
  /* sound, if the file's start had samples */
  if (m->want_sound && m->rate && mx_snd_init() == 0 && mx_snd_stream_format(m->rate, 16, 2) == 0)
    m->info.sound = 1;
  else
    m->want_sound = 0;
  return m;
fail:
  mx_movie_close(m);
  return 0;
}

/* The reader stopped: it is in a read, or waits for a free chunk. */
static void reader_stop(struct mx_movie *m) {
  if (m->thread < 0)
    return;
  m->stop = 1;
  mx_sema_signal(m->sem_free);
  mx_thread_join(m->thread);
  m->thread = -1;
  m->stop = 0;
}

static int reader_start(struct mx_movie *m) {
  if (m->sem_ready >= 0)
    mx_sema_delete(m->sem_ready);
  if (m->sem_free >= 0)
    mx_sema_delete(m->sem_free);
  m->sem_ready = mx_sema_create(0, CHUNKS);
  m->sem_free = mx_sema_create(CHUNKS, CHUNKS);
  if (m->sem_ready < 0 || m->sem_free < 0)
    return -1;
  m->next = 0;
  /* above the caller's priority: a read is asked for as soon as the one
     before is done; the thread sleeps while the IOP works */
  m->thread = mx_thread_start(reader, m, m->stack, 16384, mx_thread_prio(mx_thread_self()) - 1);
  return m->thread < 0 ? -1 : 0;
}

int mx_movie_rewind(struct mx_movie *m) {
  reader_stop(m);
  mx_fio_lseek(m->fd, 0, MX_FIO_SEEK_SET);
  m->at_end = m->at_picture = m->flushed = m->over = 0;
  m->have = m->pos = m->raw_have = 0;
  if (m->frames)
    mx_mpeg_refs(&m->mpeg, m->frames, 0);
  /* the sound starts again with the first picture shown; what mxsnd still
     holds plays out */
  m->started = 0;
  m->pcm_rd = m->pcm_wr = m->phase = m->src = m->silence = m->offset = 0;
  m->mpa_have = 0;
  mx_mp2_init(&m->mp2);
  m->step = 65536;
  m->v_pts_set = m->a_pts_set = 0;
  m->shown = m->played = 0;
  m->running = 0;
  m->cycles = 0;
  return reader_start(m);
}

void mx_movie_pause(struct mx_movie *m, int on) {
  m->paused = on != 0;
  m->last = mx_count();
}

const struct mx_movie_info *mx_movie_info(const struct mx_movie *m) { return &m->info; }
void *mx_movie_picture(const struct mx_movie *m) { return m->pix; }

u32 mx_movie_chain(const struct mx_movie *m, u64 *tags, u32 dbp, u32 dbw, u32 psm, u32 x, u32 y,
                   u32 end_id) {
  return mx_mpeg_chain(tags, &m->mpeg, m->pix, dbp, dbw, psm, x, y, end_id);
}

void mx_movie_raster(const struct mx_movie *m, void *out, u32 stride) {
  mx_mpeg_raster(&m->mpeg, m->pix, out, stride);
}

void mx_movie_poll(struct mx_movie *m) {
  pump(m);
  ahead(m);
}

int mx_movie_decode(struct mx_movie *m) {
  if (ipu_user != m) { /* another movie's tables are in the IPU */
    m->mpeg.iq_load[0] = m->mpeg.iq_load[1] = 1;
    ipu_user = m;
  }
  for (;;) {
    ahead(m);
    if (!m->at_picture) {
      int r = mx_mpeg_headers(&m->mpeg, m->es, m->have, &m->pos);
      if (r == MX_MPEG_MORE && fill(m, 1))
        continue;
      if (r != MX_MPEG_PICTURE) {
        /* the end: the reference picture still to be shown, then from the
           start again or over */
        if (!m->flushed && mx_mpeg_flush(&m->mpeg, m->pix) == MX_MPEG_PICTURE) {
          m->flushed = 1;
          m->info.pictures++;
          m->played++;
          return 1;
        }
        if (!m->loop || !m->played || mx_movie_rewind(m) < 0) {
          m->over = 1;
          return 0;
        }
        continue;
      }
      m->at_picture = 1;
    }
    u32 t0 = mx_count();
    int r = mx_mpeg_picture(&m->mpeg, m->es, m->have, &m->pos, m->pix);
    if (r == MX_MPEG_MORE) {
      if (!fill(m, 1)) { /* cut off inside a picture */
        m->at_picture = 0;
        m->have = m->pos; /* nothing more to decode: the end's turn */
        if (m->have + 4 + MX_MPEG_AHEAD <= m->es_max) {
          memset(m->es + m->have, 0, 4 + MX_MPEG_AHEAD);
          m->es[m->have + 2] = 1, m->es[m->have + 3] = 0xB7;
          mx_dcache_writeback(m->es + (m->have & ~63u), 128);
          m->have += 4 + MX_MPEG_AHEAD;
        }
      }
      continue;
    }
    m->at_picture = 0;
    if (r == MX_MPEG_PICTURE || r == MX_MPEG_NONE) {
      m->dec_cycles += mx_count() - t0;
      m->info.errors += m->mpeg.errors;
      m->info.decode_us = (u32)(m->dec_cycles * 1000000 / HZ / ++m->decoded);
    }
    if (r != MX_MPEG_PICTURE)
      continue; /* passed over, or decoded with nothing to show yet */
    m->info.pictures++;
    m->played++;
    pump(m);
    return 1;
  }
}

void mx_movie_shown(struct mx_movie *m) {
  if (!m->started && m->info.sound) {
    /* the sound starts with the first picture: by their time stamps, the
       one that starts later waits (silence), or the sound's start is dropped */
    s32 off = m->a_pts_set && m->v_pts_set ? (s32)(m->a_pts - m->v_pts) * ((s32)m->rate / 100) / 900 : 0;
    if (off > 0 && off < (s32)m->rate)
      m->silence = m->offset = (u32)off;
    else if (off < 0 && (u32)-off < m->pcm_wr - m->pcm_rd)
      m->pcm_rd += (u32)-off;
    m->started = 1;
    m->shown = 0;
  } else {
    m->shown++;
  }
  pump(m);
  if (m->shown % 12 == 0)
    sync(m);
}

int mx_movie_update(struct mx_movie *m) {
  u32 now = mx_count();
  if (m->paused)
    return 0;
  if (m->over) {
    /* the last picture is out: over once its sound is too */
    pump(m);
    if (m->started && m->info.sound_left_ms > 0 && m->cycles < 2 * HZ) {
      m->cycles += now - m->last;
      m->last = now;
      return 0;
    }
    return -1;
  }
  if (!m->running) {
    m->running = 1;
    m->last = now;
  }
  m->cycles += now - m->last;
  m->last = now;
  u64 due = m->info.rate_num ? (u64)m->played * m->info.rate_den * HZ / m->info.rate_num : 0;
  if (m->cycles < due) {
    mx_movie_poll(m);
    return 0;
  }
  if (m->cycles - due > HZ / 2) /* the program stood still: go on from here */
    m->cycles = due;
  if (!mx_movie_decode(m)) {
    m->cycles = 0; /* now the time the sound gets to end */
    return m->started ? 0 : -1;
  }
  mx_movie_shown(m);
  return 1;
}
