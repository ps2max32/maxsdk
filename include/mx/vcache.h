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
     MX_VCACHE_AUTO  MX_VCACHE_COST. The default.
     MX_VCACHE_COST  the run (of pages for a large piece, of blocks in a
                     page for a small one) whose entries cost least to
                     lose. An entry's cost per block falls with the frames
                     since its last use (halved a frame) and rises with how
                     many recent frames used it; free blocks cost nothing.
                     STREAM, entries the last two frames used, by their
                     next use, the farthest first (they cost more than any
                     older one):
                       1 this frame's done for it: used as many times as
                         in the frame before (mx_vcache_use counts the
                         uses), the most recently used first;
                       2 the last frame's not used yet in this one, the
                         one used last first;
                       3 this frame's others, the most recently used first.
                     When a frame's textures do not all fit, the part used
                     earlier stays from frame to frame, and a texture done
                     for the frame makes room before one still to come.
                     An entry evicted and placed again in the next frame
                     keeps its counts.
     MX_VCACHE_RING  in ring order, as libgl did: from where the last large
                     piece ended on, round to the first page; small pieces
                     after the last in the ring's page of small pieces. What
                     is in the way goes (what must stay is passed).
   Free blocks are used first in every policy.

   Not thread safe: one thread looks up. */

#include <mx/tex.h>
#include <mx/types.h>

#define MX_VCACHE_ENTRIES 1024
/* cpsm of an indexed texture without a CLUT of its own (mx_vcache_tex) */
#define MX_VCACHE_NO_CLUT 0xFFu

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
/* Pages page .. page + pages - 1 hold a buffer whose top byte of each
   word nothing writes (CT24, Z24: GS manual 2.6.3; mx_hires_hbyte): T8
   textures may go there as T8H (the same texels a byte, in 32-bit page
   geometry: 4 times the blocks), their CLUT in a page of the cache. A
   texture goes there when that room costs less to make than room in
   pages of the cache; a lookup gives psm T8H then (upload and draw it
   so). 0 pages: none. After mx_vcache_init (which sets none), before
   lookups. */
void mx_vcache_hpages(u32 page, u32 pages);

/* A (1 << tw) x (1 << th) texture of psm with levels levels, and for
   T8/T4/T8H/T4HL/T4HH its CLUT of cpsm (CSM1: 16 x 16, T4/T4HL/T4HH 8 x
   2; MX_VCACHE_NO_CLUT: none, its CLUT is elsewhere, e.g. one shared by
   textures), into *t: tbp/tbw of each level, cbp/cpsm (csm 0), blocks (all
   its pieces). The key's shape must not change while the GS may read it (a
   miss on a key of another shape drops the old entry; -1 if it can't). */
int mx_vcache_tex(u64 key, u32 psm, u32 tw, u32 th, u32 levels, u32 cpsm, struct mx_tex *t);
/* A buffer of w x h pixels of psm (one piece, buffer width
   mx_tex_tbw(w, psm)), its 64-word block into *bp. */
int mx_vcache_buf(u64 key, u32 w, u32 h, u32 psm, u32 *bp);
/* Where key is, without a use: 1 (and *t: a buffer's block in tbp[0]), or 0. */
int mx_vcache_peek(u64 key, struct mx_tex *t);
/* A use of key in the current frame without a lookup (a draw with a
   texture whose places the caller kept from this frame's lookup): counts
   for the policy. 0, or -1 if key is not there. */
int mx_vcache_use(u64 key);
/* A pinned entry is never evicted (pins count; MX_VCACHE_FULL comes
   sooner). Returns 0, or -1 if key is not there. */
int mx_vcache_pin(u64 key);
void mx_vcache_unpin(u64 key);
/* Drop key now (its room is free for the next lookups): 0, or -1 if it is
   not there, pinned, or one the mode keeps (in use). */
int mx_vcache_drop(u64 key);

/* ---- textures whose upper levels may be paged out ------------------------------ */

/* A texture's levels in RAM, split (<mx/mem.h>): the tail (levels from
   tail on, and the CLUT) in a pinned heap, always resident; the head
   (levels 0 .. tail - 1) in an asset heap, pageable. The head is 15/16 of
   the bytes; the tail starts 2 levels down, fewer if its top level would
   be smaller than MX_VCACHE_TAIL_MIN texels a side. A texture too small
   to split, or without an asset heap, is all in the pinned heap (tail ==
   0). level[] are the levels, each 64-byte aligned. */
#define MX_VCACHE_TAIL_MIN 32
struct mx_heap;
struct mx_tex_mem {
  void *level[MX_TEX_LEVELS];
  void *clut;
  void *head, *tail_mem; /* the two allocations (head 0: not split) */
  u32 head_bytes, tail_bytes;
  u8 levels, tail;
  u8 from;        /* the finest level placed last (mx_vcache_tex_mem) */
  u8 lock_init;   /* the cache's set-up its head was locked in (mode changes) */
  u32 lock_frame; /* the frame its head was locked for last, + 1 */
};
/* bytes of level lv of a (1 << tw) x (1 << th) texture of psm, a multiple of 64 */
u32 mx_tex_level_bytes(u32 psm, u32 tw, u32 th, u32 lv);
/* Allocate levels levels (and clut_bytes of CLUT, 0: none); asset 0: not
   split. Returns 0, or -1 (out of memory: nothing allocated). */
int mx_tex_mem_alloc(struct mx_tex_mem *m, u32 psm, u32 tw, u32 th, u32 levels, u32 clut_bytes,
                     struct mx_heap *pinned, struct mx_heap *asset);
/* free it now (the GS reads none of it any more) */
void mx_tex_mem_free(struct mx_tex_mem *m, struct mx_heap *pinned, struct mx_heap *asset);
/* The cache's key of the texture placed from level from (0: key itself). */
MX_INLINE u64 mx_vcache_tex_key(u64 key, u32 from) { return key ^ ((u64)from << 56); }
/* Place the texture of memory m, as mx_vcache_tex, from the finest level
   it can have now: need (the finest the caller wants: 0 all of it, more for
   a lower quality) if it is in GS memory or its head can be locked without
   waiting for the swap file (wait, or no level below the tail: wait for
   it instead), else the tail
   (a texture 1 << tail times smaller each way; the pager brings the head
   in, and a later call places it whole: a miss). *from: the level placed
   as level 0 (t is that texture: tw, th, levels less from). On a miss the
   caller uploads m->level[*from + i] to t->tbp[i], and the CLUT; the head
   is locked until the frame is done (mx_frame_unlock). Each texture once a
   frame (mx_vcache_frame) at most. */
int mx_vcache_tex_mem(u64 key, struct mx_tex_mem *m, u32 psm, u32 tw, u32 th, u32 cpsm, u32 need,
                      int wait, struct mx_tex *t, u32 *from);
/* Lock its head for a transfer of this frame (changed lines of a texture
   in GS memory), waiting if it must; unlocked when the frame is done. */
void mx_tex_mem_lock(struct mx_tex_mem *m);

struct mx_vcache_stats {
  u32 hits, misses, full; /* lookups */
  u32 evicted;            /* entries evicted to make room */
  u32 evicted_now;        /* of them, used in the frame that evicted them (STREAM) */
  u32 evicted_done;       /* of them, done for that frame (as many uses as the frame before) */
  u32 ring_places;        /* pieces placed in ring order */
  u32 upload_blocks;      /* blocks placed by misses (to upload) */
  u32 entries, pages;     /* now */
  u32 small_pages;        /* of the pages, shared by small pieces */
  u32 used_blocks;        /* blocks the entries take */
  u32 proxies;            /* textures placed from their tail: the head was not resident */
  u32 upgrades;           /* of them, placed whole again since */
  u32 hbyte_blocks;       /* blocks of top bytes (mx_vcache_hpages) */
  u32 hbyte_used;         /* of them, holding T8H textures now */
  u32 hbyte_places;       /* textures placed there */
  /* where GS memory goes (now; blocks): the entries' texels (whole blocks
     of 256 bytes), the blocks between them in the page's block order (a
     piece that is not a whole number of blocks wide and high), whole pages
     of large pieces past that, free blocks in pages of small pieces; the
     top bytes' texels (64 a block) */
  u32 need_blocks, span_waste, round_waste, small_free, hbyte_need;
  /* making room: the blocks in use in the runs taken, and the blocks of
     the entries evicted for them (all of each entry: more when one lay
     only partly in the run) */
  u32 evict_run_blocks, evicted_blocks;
};
void mx_vcache_stats(struct mx_vcache_stats *s);

#endif
