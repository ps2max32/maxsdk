/* mxio: the file service of the IOP for the EE, in place of rom0:FILEIO.

   Every call names the position in the file, so there is no seek, and data goes
   between the device and EE memory without the EE waiting on each piece. host:
   takes the fast path (mxfast.h) when a host has said HELLO and mxnet is
   loaded, any other path a driver of ioman.

   How to give a write depends on the device, so OPEN tells: data goes in the RPC
   buffer in pieces of chunk bytes, stride bytes apart, the first one at
   MXIO_HEAD, behind the arguments. For the fast path that leaves room for the frame's
   headers; a driver takes it in one piece.

   Arguments are 16-byte aligned words, and the answer is MXIO_ANSWER words. EE
   addresses are 16-byte aligned, and DMA rounds up: a read may write up to 15
   bytes behind its end. */
#ifndef MXIO_H
#define MXIO_H

#define MXIO_SID 0x4D58494F /* "MXIO" */
#define MXIO_HEAD 64u /* the arguments, in front of the first piece */
#define MXIO_MAX_WRITE 32768u
#define MXIO_BUFFER (MXIO_HEAD + 24u * 1472u)
#define MXIO_ANSWER 4
#define MXIO_FILES 32

enum {
  MXIO_OPEN,  /* flags (ioman's), path; answer: fd or < 0, stride, chunk */
  MXIO_CLOSE, /* fd */
  MXIO_READ,  /* fd, position, size, EE address; answer: bytes or < 0 */
  MXIO_WRITE, /* fd, position, size; data as OPEN said; answer: bytes or < 0 */
  MXIO_SIZE,  /* fd; answer: the file's size or < 0 */
  MXIO_LOG,   /* text, to the host's console; < 0 without a host */
  MXIO_STATS  /* answer: frames, doubles, drops, asked again (the fast path) */
};

#endif
