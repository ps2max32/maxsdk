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
  MXIO_STATS, /* answer: frames, doubles, drops, asked again (the fast path) */
  MXIO_CLOSE_ALL, /* what a program left open */
  MXIO_WATCH,     /* EE address of a struct mxio_devs (64-byte aligned); answer:
                     MXIO_DEVS_VERSION. mxio then writes the devices there by DMA
                     at every change (kernel version 26) */
  MXIO_CLOCK,     /* answer: cdvdman's sceCdReadClock's result (1 read), then its
                     8 bytes (stat, second, minute, hour, pad, day, month, year:
                     BCD, Japan's time) (kernel version 29) */
  MXIO_CLOCK_SET  /* the same 8 bytes; answer: sceCdWriteClock's result (1 written) */
};

/* The storage devices (MXIO_WATCH): mxio sees USB drives, MX4SIO and the HDD
   come and go through bdm's callback, and the memory cards by looking every
   second. gen changes when a device came or went; flags (files open on it)
   change without it. A device that went stays in the list, not present,
   until the next change. Kinds and fs as <mx/dev.h>'s MX_DEV_*. */
#define MXIO_DEVS_VERSION 1
#define MXIO_DEVS 8
#define MXIO_DEV_BUSY 2 /* <mx/dev.h>'s MX_DEV_BUSY: files open on it */
struct mxio_dev {
  char name[12]; /* "usb0:", "mc1:" */
  unsigned char kind, present, fs, flags;
  unsigned int size_lo, size_hi; /* bytes, 0 when not known */
  unsigned int files;            /* files open on it */
  char label[24];
  unsigned int free_lo, free_hi; /* bytes free, 0 when not known */
  unsigned int pad;
};
struct mxio_devs {
  unsigned int gen, n, version, pad;
  struct mxio_dev d[MXIO_DEVS];
};

#endif
