/* The fast path: files from the host to EE memory past lwIP.

   Requests and their answers are plain UDP datagrams on port 18197, through
   lwIP's sockets. Data comes back in DATA datagrams, which the stack takes
   before lwIP and sends on to the EE by SIF DMA, straight from the buffer SMAP
   wrote them to. The layout keeps every DMA aligned, and does not copy:

     frame + 0   Ethernet, IP and UDP headers: 42 bytes
     frame + 42  struct below: 22 bytes
     frame + 64  data: as aligned as the frame (SMAP's DMA needs 4 bytes)

   MXFAST_CHUNK bytes of data make a frame of 1472 bytes, 23 of SMAP's 64-byte
   DMA blocks, so nothing is left for its PIO loop. Each offset is a multiple of
   16, as SIF DMA wants for its target. The host pads the last frame to 16 bytes,
   so the target needs up to 15 bytes of room behind it.

   The host keeps no state of a transfer: a READ names a range of the file and
   where it goes in the transfer, and a lost frame is asked for by a READ of that
   range again. All numbers are little endian. */
#ifndef MXFAST_H
#define MXFAST_H

#define MXFAST_PORT 18197
#define MXFAST_MAGIC 0x464D /* "MF" */
#define MXFAST_HDR 22u
#define MXFAST_DATA_AT 64u /* in the frame */
#define MXFAST_CHUNK 1408u
#define MXFAST_MAX (256u * 1024u) /* bytes of one transfer */
#define MXFAST_FRAME (MXFAST_DATA_AT + MXFAST_CHUNK)
#define MXFAST_WRITE_FRAMES 24u /* of one write: 33792 bytes */

enum {
  MXFAST_OPEN = 1,  /* path after the header; answer: result = handle or < 0 */
  MXFAST_CLOSE = 2, /* handle */
  MXFAST_READ = 3,  /* handle, offset in the file, size, offset in the transfer */
  MXFAST_LOG = 4,   /* text after the header, for the host's console */
  MXFAST_DATA = 5,
  MXFAST_WRITE = 6,  /* data: handle, offset in the file, offset in the transfer */
  MXFAST_COMMIT = 7, /* answer: a 0, b one bit for each frame of this id received */
  MXFAST_HELLO = 8,  /* from the host, every second: here is the file server */
  MXFAST_SIZE = 9,   /* handle; answer: the file's size */
  MXFAST_ANSWER = 0x80 /* or'ed to the request's op */
};

/* The header, at an offset of 2 modulo 4 in the frame: read by bytes.
     0 magic (2)   2 op (1)   3 unused (1)   4 id (4)
     8 a (4)      12 len (2) 14 b (4)       18 c (4)
   DATA:   a offset in the transfer, len bytes of data, b the end of the transfer
           if this range reaches the end of the file, else 0xFFFFFFFF
   READ:   a handle, b offset in the file, c size, then 4 bytes: offset in the
           transfer
   CLOSE:  a handle
   OPEN:   a 0 to read, 1 to write (created if missing), 3 to write from empty
   WRITE:  a handle, len bytes of data, b offset in the file, c offset in the
           transfer. The host writes each frame when it comes, so a frame
           sent twice does no harm, and the frames of an id count in its COMMIT.
   ANSWER: a result, b as above

   Writes leave the IOP without a copy and without lwIP: the data comes as frames,
   each with MXFAST_DATA_AT bytes free in front of it.

   Library "mxfast", for mxio, called from one thread below the driver's. The
   functions after the first four of its table: */

#ifdef _IOP
struct pbuf;
/* A frame from the driver: 1 if the fast path took it */
int mxfast_rx(struct pbuf *p);
/* For the driver, before lwIP's frames: the next frame to send, and its end */
int mxfast_tx_next(void **payload);
int mxfast_tx_deq(void);
#endif

/* -6 when no host has said HELLO */
typedef int mxfast_open_fn(const char *path, int mode);
typedef int mxfast_close_fn(int handle);
/* the bytes read to ee, 16-byte aligned; at most MXFAST_MAX */
typedef int mxfast_read_fn(int handle, unsigned int pos, unsigned int size, unsigned int ee);
/* frames: MXFAST_WRITE_FRAMES of MXFAST_FRAME bytes, data at MXFAST_DATA_AT in each */
typedef int mxfast_write_fn(int handle, unsigned int pos, unsigned int size, void *frames);
typedef int mxfast_size_fn(int handle);
typedef int mxfast_log_fn(const char *text);
typedef void mxfast_stats_fn(int *out);

#endif
