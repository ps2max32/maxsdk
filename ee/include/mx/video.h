#ifndef MX_VIDEO_H
#define MX_VIDEO_H

/* Video modes: the CRTC timing (SetGsCrt, SMODE2) and the active area of the
   screen in the units of DISPLAY: DX/DW in VCK (sub-pixels), DY/DH in raster
   lines (in frame lines when interlaced: both fields). What the read circuits
   show inside it is <mx/display.h>; nothing here fixes a buffer size.

   The NTSC and PAL areas are sceGsSetDefDispEnv's (checked on a TV). The
   others are gsKit's start values and have not been checked on a screen
   (docs/gif-gs.md); a mode is a plain struct, so copy one and adjust it. */

#include <mx/crtc.h>
#include <mx/types.h>

struct mx_video_mode {
  u8 code;      /* SetGsCrt mode: 0x02 NTSC, 0x03 PAL, 0x1A.. VESA, 0x50 480p, 0x51 1080i,
                   0x52 720p, 0x53 576p; 0 for one the BIOS does not have */
  u8 interlace; /* 1: interlaced */
  u8 ffmd;      /* interlaced: 0 FIELD (a field reads every other buffer line), 1 FRAME
                   (every line, both fields the same image) */
  u8 hz;        /* fields (progressive: frames) a second */
  u16 vck_w;    /* active width in VCK */
  u16 lines;    /* active height in DISPLAY lines */
  u16 dx, dy;   /* top left of the active area: DX (VCK), DY (lines) */
  /* the timing (<mx/crtc.h>); 0: the BIOS's mode code (mx_crtc_for_code) */
  const struct mx_crtc *crtc;
};

extern const struct mx_video_mode mx_video_ntsc;      /* 480i: 2560 VCK x 448 lines */
extern const struct mx_video_mode mx_video_pal;       /* 576i: 2560 x 512 */
extern const struct mx_video_mode mx_video_ntsc_240p; /* NTSC progressive: 2560 x 224 */
extern const struct mx_video_mode mx_video_pal_288p;  /* PAL progressive: 2560 x 256 */
extern const struct mx_video_mode mx_video_480p;      /* 1440 x 480 */
extern const struct mx_video_mode mx_video_576p;      /* 1440 x 576 */
extern const struct mx_video_mode mx_video_720p;      /* 1280 x 720 */
extern const struct mx_video_mode mx_video_1080i;     /* 1920 x 1080 */
extern const struct mx_video_mode mx_video_vga_640;   /* VESA 640 x 480, 60 Hz: 1280 x 480 */
extern const struct mx_video_mode mx_video_vga_800;   /* VESA 800 x 600, 60 Hz: 1600 x 600 */
extern const struct mx_video_mode mx_video_vga_1024;  /* VESA 1024 x 768, 75 Hz: 1024 x 768 */
extern const struct mx_video_mode mx_video_vga_1280;  /* VESA 1280 x 1024, 60 Hz: 1280 x 1024 */
/* not in the BIOS: mx-kernel 22 or later (<mx/crtc.h>), not yet seen on a screen */
extern const struct mx_video_mode mx_video_720p50;    /* 1280 x 720 */
extern const struct mx_video_mode mx_video_1080i50;   /* 1920 x 1080 */
extern const struct mx_video_mode mx_video_1080p30;   /* 1920 x 1080 */

/* The console's own TV mode: mx_video_pal on a European console (the
   region letter of ROMVER in the BIOS ROM is 'E', as gsKit's
   gsKit_detect_signal reads it), else mx_video_ntsc. */
const struct mx_video_mode *mx_video_default(void);

/* The mode set by mx_video_set (a copy). */
extern struct mx_video_mode mx_video;

/* DISPLAY lines (DH units) a buffer line takes at MAGV 0: 2 in interlaced
   FRAME mode (each field shows it), else 1. */
MX_INLINE u32 mx_video_line_lines(const struct mx_video_mode *m) {
  return m->interlace && m->ffmd ? 2 : 1;
}

/* Set the mode: GS reset (drawing registers too, not GS memory), the CRTC
   timing (mx-kernel version 22 writes it from the description, <mx/crtc.h>;
   before, the BIOS's SetGsCrt), SMODE2. The read circuits are not written: apply a display for the mode
   (<mx/display.h>) before the picture is meaningful.
   The GS must be idle (no frame pipeline running). mx_gs_init_video() also
   initializes the DMAC; use that first. Returns 0, or -1 if the CRTC gives
   no two vblanks within half a second: the console does not have the mode (the frame
   pipeline would wait for ever); set another one. */
int mx_video_set(const struct mx_video_mode *m);
/* The field being shown, from the vblank that starts it: 0 even, 1 odd.
   CSR.FIELD flips about 220 us after the vblank interrupt; it is read
   accordingly, waiting up to 150 us when read around the flip. */
u32 mx_video_field(void);
/* The board's offsets of the active area for a mode (the BIOS's
   GetGsDxDyOffset, as gsKit uses for modes other than NTSC/PAL). Only BIOSes
   newer than 2001-06-08 have it; the caller must know. An MXOS program asks
   the kernel (version 23; 0, 0 from an older one). */
void mx_video_board_offset(int code, int *dx, int *dy);

#endif
