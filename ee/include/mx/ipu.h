#ifndef MX_IPU_H
#define MX_IPU_H

/* The IPU (EE User's Manual, ch. 8): the EE's decoder of MPEG macroblocks.
   It reads a bit stream from its input FIFO (DMA channel toIPU), and writes
   pixels to its output FIFO (fromIPU): variable-length decoding, inverse
   quantization, IDCT, and the conversion to RGB. It has no motion
   compensation: a picture of intra macroblocks (an I picture) is decoded
   by it alone (IDEC), any other by BDEC a macroblock, with the prediction
   added on the EE. What is measured about it: docs/ipu.md.

   This is the unit only; MPEG video on it is <mx/mpeg.h>.

   One command at a time, from one thread. A command is waited for asleep:
   the IPU's interrupt and the ends of its two DMA channels wake the caller,
   so the channels' interrupts are the IPU's while it is in use.

   Memory: what the IPU reads must be written back first
   (mx_dcache_writeback); what it writes is in memory, not in the cache
   (mx_dcache_invalidate before reading it; nothing to do before sending it
   on by DMA). Both in one physical piece, 16-byte aligned: core memory, or
   VM pinned with MX_VM_PIN_DMA (<mx/vm.h>).

   Formats (a macroblock is 16 x 16 pixels):
     RGB32  64 quadwords: 256 pixels A8 B8 G8 R8 (R in the low byte, as the
            GS's CT32), row by row
     RGB16  32 quadwords: 256 pixels A1 B5 G5 R5 (the GS's CT16)
     RAW8   24 quadwords: Y (16 x 16 bytes), Cb (8 x 8), Cr (8 x 8)
     RAW16  48 quadwords: the same in 16-bit signed samples
     INDX4   8 quadwords: 256 indices into the 16 colours of mx_ipu_setvq */

#include <mx/bits.h>
#include <mx/cpu.h>
#include <mx/hw/formats.h>
#include <mx/hw/regs.h>
#include <mx/types.h>

enum {
  MX_IPU_MB_RGB32 = 64, /* quadwords a macroblock */
  MX_IPU_MB_RGB16 = 32,
  MX_IPU_MB_RAW8 = 24,
  MX_IPU_MB_RAW16 = 48,
  MX_IPU_MB_INDX4 = 8,
};

/* How a command ended. */
enum {
  MX_IPU_OK = 0,
  MX_IPU_STARVED = -1, /* it waits for more of the bit stream: mx_ipu_more, mx_ipu_wait */
  MX_IPU_FULL = -2,    /* more output than the buffer given: the IPU was reset */
};

/* Reset the IPU, take its interrupt and those of its DMA channels. Returns
   0, or -1 without a semaphore. */
int mx_ipu_init(void);
/* Reset the IPU and give the interrupts back. */
void mx_ipu_exit(void);
/* Stop both channels, end the command under way, empty the FIFOs. The
   tables (mx_ipu_setiq, mx_ipu_setvq), the thresholds and mx_ipu_ctrl's
   bits are to be set again. */
void mx_ipu_reset(void);

/* How the bit stream is coded, from the picture's headers: IPU_CTRL's IDP,
   AS, IVF, QST, MP1 and PCT, e.g. MX_SET(IPU_CTRL_IDP, 1) | MX_BIT(IPU_CTRL_AS). */
void mx_ipu_ctrl(u32 bits);
/* IPU_CTRL: MX_GET(IPU_CTRL_SCD, ..), _ECD (set by the last command: it met
   a start code / what is no code), _CBP (BDEC of a non-intra macroblock). */
MX_INLINE u32 mx_ipu_status(void) { return MX_R32(MX_IPU_CTRL); }

/* ---- the bit stream ---------------------------------------------------- */

/* Start a bit stream: the input FIFO emptied, then qwc quadwords from bs
   (at most 65535), decoding from bit bp (0 .. 127) of the first. Bits are
   taken from each byte's highest first, bytes in memory order. */
void mx_ipu_input(const void *bs, u32 qwc, u32 bp);
/* Go on with qwc more quadwords, which follow the ones before in the
   stream (anywhere in memory). Waits for the transfer before to end. */
void mx_ipu_more(const void *bs, u32 qwc);
/* Stop the bit stream's transfer and empty the input FIFO. */
void mx_ipu_input_end(void);
/* The bits decoded since mx_ipu_input, counted from the start of its first
   quadword (so bp of them were skipped). After a command has ended (not
   between mx_ipu_input and the first). */
u32 mx_ipu_pos(void);
/* The next 32 bits of the stream, the first in the highest bit; nothing is
   taken. After IDEC, BDEC, VDEC or FDEC. */
u32 mx_ipu_top(void);

/* FDEC: skip fb bits (0 .. 32), then as mx_ipu_top. */
u32 mx_ipu_fdec(u32 fb);
/* VDEC: skip fb bits, decode one code of table tbl; the code's bits are
   taken. Returns the manual's value (8.5.3): the code's length in the upper
   half, what it means in the lower; 0 for no code of the table. */
enum { MX_IPU_VLC_MBAI = 0, MX_IPU_VLC_MBT = 1, MX_IPU_VLC_MOTION = 2, MX_IPU_VLC_DMV = 3 };
u32 mx_ipu_vdec(u32 tbl, u32 fb);

/* ---- tables ------------------------------------------------------------ */

/* SETIQ: the next 64 bytes of the bit stream after fb bits are the intra
   (nonintra 0) or non-intra quantizer matrix, in the stream's order
   (zigzag). */
void mx_ipu_setiq(int nonintra, u32 fb);
/* The same from memory (m: 64 bytes, zigzag order). It takes the input
   FIFO: a bit stream under way is to be started again (mx_ipu_input at
   mx_ipu_pos). */
void mx_ipu_setiq_mem(int nonintra, const u8 *m);
/* SETVQ: the 16 colours (RGB16) that MX_IPU_PACK_INDX4 picks from. Takes the
   input FIFO. */
void mx_ipu_setvq(const u16 *clut);
/* SETTH: the alpha of a decoded pixel. Where R, G and B are all below th0
   the pixel is 0, colour and alpha; where all are below th1, alpha 0x40;
   else 0x80. In RGB16 the bit is set for 0x40 only (manual 8.6.1). 0, 0:
   every alpha 0x80. */
void mx_ipu_setth(u32 th0, u32 th1);

/* ---- decoding ---------------------------------------------------------- */

#define MX_IPU_RGB16 (1u << 27)  /* IDEC, CSC: output RGB16 (else RGB32) */
#define MX_IPU_DITHER (1u << 26) /* IDEC, CSC, PACK: ordered dither for RGB16 */
#define MX_IPU_IDEC_SGN (1u << 25) /* IDEC: 128 off each colour (wraps) */
#define MX_IPU_IDEC_DTD (1u << 24) /* IDEC: the macroblocks have dct_type */

/* IDEC: skip fb bits, then decode intra macroblocks to RGB until a start
   code (a slice of an I picture, from its first macroblock_type on; the
   macroblocks after the first must follow each other). qsc: the slice's
   quantiser_scale_code. out takes at most mbs macroblocks (RGB32: up to
   1023 at once). Returns the macroblocks written, or MX_IPU_STARVED /
   MX_IPU_FULL; mx_ipu_status says whether a start code (SCD) or an error
   (ECD) ended it, and the stream is at that code. */
int mx_ipu_idec(u32 fb, u32 qsc, u32 flags, void *out, u32 mbs);
/* 1 where IDEC takes the field as the scale, not its code: PCSX2
   (docs/pcsx2.md), found out by mx_ipu_init. mx_ipu_idec then gives it the
   code's scale (for the q_scale_type of mx_ipu_ctrl), 31 at most: a slice
   with a larger scale is wrong there up to the first macroblock that brings
   its own code. */
int mx_ipu_qsc_is_scale(void);

#define MX_IPU_BDEC_INTRA (1u << 27) /* an intra macroblock: all six blocks */
#define MX_IPU_BDEC_DCR (1u << 26)   /* reset the DC prediction first */
#define MX_IPU_BDEC_FIELD (1u << 25) /* field DCT */

/* BDEC: skip fb bits, then decode one macroblock to RAW16 (out: 48
   quadwords): the samples of an intra one, the difference to the prediction
   of another (from its coded_block_pattern on; blocks not coded are 0).
   Returns 1 (a macroblock written), 0 with ECD set, or MX_IPU_STARVED. */
int mx_ipu_bdec(u32 fb, u32 qsc, u32 flags, void *out);

/* CSC: mbs macroblocks RAW8 at raw8 to RGB32 or RGB16 (flags: MX_IPU_RGB16,
   MX_IPU_DITHER) at out; at most 1023 at once as RGB32, 2047 as RGB16.
   Takes the input FIFO. Returns the macroblocks written. */
int mx_ipu_csc(const void *raw8, u32 mbs, u32 flags, void *out);

#define MX_IPU_PACK_RGB16 (1u << 27) /* else INDX4 */

/* PACK: mbs macroblocks RGB32 at rgb32 to RGB16 (MX_IPU_PACK_RGB16, with
   MX_IPU_DITHER if wanted) or INDX4 at out; at most 1023 at once. Takes
   the input FIFO. Returns the macroblocks written. */
int mx_ipu_pack(const void *rgb32, u32 mbs, u32 flags, void *out);

/* Wait for a command that returned MX_IPU_STARVED, after mx_ipu_more. For
   IDEC and BDEC as their return value, else 0 or MX_IPU_STARVED. */
int mx_ipu_wait(void);

#endif
