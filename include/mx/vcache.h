#ifndef MX_VCACHE_H
#define MX_VCACHE_H

/* GS memory as a cache: textures (each MIPMAP level and the CLUT placed on
   its own) and other buffers (a CLUT shared by textures), by a key of the
   application's, in the pages that render targets leave (<mx/vram.h>:
   taken as needed, owner MX_VRAM_CACHE, given back when empty).

   Places: a level of a page or more gets a run of whole pages of its own
   (no rounding to a power of two); a smaller one a run of blocks inside a
   page shared with other small pieces, from a multiple of the power of two
   that holds it (as mx_tex_init places them: the piece's blocks in the
   page's block order then lie in that run, whatever its format).

   A lookup finds the key (a hit: the places are as they were, the data is
   there), or places it (a miss: the caller uploads every level, and the
   CLUT, before the first draw that reads it: mx_tex_upload, mx_gs_upload).
   It counts as a use in the current frame (mx_vcache_frame).

   Room is made by evicting whole entries, never a pinned one, nor one the
   GS may still read (mode):
     MX_VCACHE_STREAM  uploads go into the frame's stream, in order with the
                       draws, and each frame's stream is sent once: any
                       entry may be overwritten, even one this frame drew
                       with earlier (its draws come before the upload).
                       Places from a lookup are valid until the next lookup
                       that misses: look up before each use.
     MX_VCACHE_REPLAY  in the stream, but a frame's stream may be played
                       more than once (<mx/hires.h>'s passes): no entry this
                       frame uses is evicted.
     MX_VCACHE_ASYNC   uploads may overtake the draws (PATH3, once
                       records): only entries of frames the GS is done with
                       (mx_vcache_frame's done) are evicted.
   What goes depends on the policy:
     MX_VCACHE_AUTO  as MX_VCACHE_COST, until making room would evict an
                     entry this frame used (STREAM: the frame's textures do
                     not all fit); the rest of that frame is placed as
                     MX_VCACHE_RING (cheap, no search), the next frame
                     starts as MX_VCACHE_COST again. The default.
     MX_VCACHE_COST  the run (of pages for a large piece, of blocks in a
                     page for a small one) whose entries cost least to
                     lose. An entry's cost per block falls with the frames
                     since its last use (halved a frame) and rises with how
                     many recent frames used it; free blocks cost nothing.
                     Entries this frame used go last, the most recently used
                     of them first (STREAM: when a frame's textures do not
                     all fit, the part used earlier stays from frame to
                     frame).
     MX_VCACHE_RING  in ring order, as libgl did: from where the last large
                     piece ended on, round to the first page; small pieces
                     after the last in the ring's page of small pieces. What
                     is in the way goes (what must stay is passed).
   Free blocks are used first in every policy.

   Not thread safe: one thread looks up. */

#include <mx/tex.h>
#include <mx/types.h>

#define MX_VCACHE_ENTRIES 1024

enum { MX_VCACHE_STREAM, MX_VCACHE_REPLAY, MX_VCACHE_ASYNC };
enum { MX_VCACHE_AUTO, MX_VCACHE_COST, MX_VCACHE_RING };
/* lookup results */
enum { MX_VCACHE_FULL = -1, MX_VCACHE_HIT = 0, MX_VCACHE_MISS = 1 };

/* Drop every entry (the GS no longer reads them), give their pages back,
   and set the most pages the cache takes (0: as many as are free), the
   mode and the policy. After mx_gs_init* (which free every page, the
   cache's too). */
void mx_vcache_init(u32 max_pages, int mode, int policy);
/* Drop every entry and give the pages back (the GS no longer reads them). */
void mx_vcache_reset(void);
/* At the start of each frame: now the number of the frame being built,
   done the frames the GS is done with (frame f is done when done > f):
   mx_vcache_frame(mx_frame_number(), mx_frame_done()) with <mx/frame.h>. */
void mx_vcache_frame(u32 now, u32 done);

/* A (1 << tw) x (1 << th) texture of psm with levels levels, and for
   T8/T4/T8H/T4HL/T4HH its CLUT of cpsm (CSM1: 16 x 16, T4/T4HL/T4HH 8 x
   2), into *t: tbp/tbw of each level, cbp/cpsm (csm 0), blocks (all its
   pieces). The key's shape must not change while the GS may read it (a
   miss on a key of another shape drops the old entry; -1 if it can't). */
int mx_vcache_tex(u64 key, u32 psm, u32 tw, u32 th, u32 levels, u32 cpsm, struct mx_tex *t);
/* A buffer of w x h pixels of psm (one piece, buffer width
   mx_tex_tbw(w, psm)), its 64-word block into *bp. */
int mx_vcache_buf(u64 key, u32 w, u32 h, u32 psm, u32 *bp);
/* Where key is, without a use: 1 (and *t: a buffer's block in tbp[0]), or 0. */
int mx_vcache_peek(u64 key, struct mx_tex *t);
/* A pinned entry is never evicted (pins count; MX_VCACHE_FULL comes
   sooner). Returns 0, or -1 if key is not there. */
int mx_vcache_pin(u64 key);
void mx_vcache_unpin(u64 key);
/* Drop key now (its room is free for the next lookups): 0, or -1 if it is
   not there, pinned, or one the mode keeps (in use). */
int mx_vcache_drop(u64 key);

struct mx_vcache_stats {
  u32 hits, misses, full; /* lookups */
  u32 evicted;            /* entries evicted to make room */
  u32 evicted_now;        /* of them, used in the frame that evicted them (STREAM) */
  u32 ring_frames;        /* frames AUTO placed in ring order from a point on */
  u32 ring_places;        /* pieces placed in ring order */
  u32 upload_blocks;      /* blocks placed by misses (to upload) */
  u32 entries, pages;     /* now */
  u32 small_pages;        /* of the pages, shared by small pieces */
  u32 used_blocks;        /* blocks the entries take */
};
void mx_vcache_stats(struct mx_vcache_stats *s);

#endif
