#ifndef MX_PIC_H
#define MX_PIC_H

/* Still pictures from files: JPEG and PNG decoded on the EE to pixels the GS
   takes (CT32: R in the low byte, alpha 0x80 for opaque), for textures,
   backgrounds, icons. docs/video-plan.md.

     JPEG  baseline and extended sequential (Huffman, 8 bits): grey, or
           Y Cb Cr with the chroma at full, half (4:2:2, 4:4:0) or quarter
           (4:2:0) size; restart intervals. Not progressive, not CMYK. A
           file must end with its end-of-image marker.
     PNG   grey, RGB, palette, each with or without alpha (tRNS too), 1 to 16
           bits a sample (16 are cut to 8). Not interlaced.

   A picture of I-picture MPEG (one frame of video) is <mx/movie.h>'s: the
   IPU decodes it, many times faster, into macroblocks.

   The pixels are pinned memory (<mx/mem.h>): mx_gs_upload or mx_tex_upload
   send them as they are. Decoding is the EE's work alone and takes its
   time: 640 x 384 as a JPEG 0.08 s, as a PNG 0.3 s, read from host:
   included (docs/video-plan.md). */

#include <mx/types.h>

struct mx_pic {
  u16 width, height;
  u8 alpha;    /* some pixel is not opaque */
  u32 *pixels; /* width * height, line after line from the top */
};

enum {
  MX_PIC_OK = 0,
  MX_PIC_FORMAT = -1, /* neither JPEG nor PNG, or a kind of them not decoded here */
  MX_PIC_DATA = -2,   /* cut off or damaged */
  MX_PIC_MEMORY = -3,
  MX_PIC_FILE = -4,   /* mx_pic_load: cannot open or read it */
};

/* The size of the picture in data without decoding it. */
int mx_pic_size(const void *data, u32 bytes, u16 *width, u16 *height);
/* Decode the picture in data. MX_PIC_OK: p is filled, its pixels to be
   given back with mx_pic_free. */
int mx_pic_decode(const void *data, u32 bytes, struct mx_pic *p);
/* The same from a file. */
int mx_pic_load(const char *path, struct mx_pic *p);
void mx_pic_free(struct mx_pic *p);

/* The decoders by format, into memory of the caller's: width * height
   pixels at out (mx_pic_size says how many). */
int mx_jpeg_decode(const void *data, u32 bytes, u32 *out, u8 *alpha);
int mx_png_decode(const void *data, u32 bytes, u32 *out, u8 *alpha);
int mx_jpeg_size(const void *data, u32 bytes, u16 *width, u16 *height);
int mx_png_size(const void *data, u32 bytes, u16 *width, u16 *height);

#endif
