#ifndef MX_CRTC_H
#define MX_CRTC_H

/* The CRTC: the timing of the video signal (pixel clock, sync, blanking), computed
   from a description instead of the BIOS's fixed mode table (SetGsCrt). Any timing
   the GS's PLL and counters can make can be described, e.g. 1080i at 50 Hz.

   A description gives the line and the frame in the GS's own units:
   - horizontal: VCK (video clock) cycles; a pixel is spml VCK.
     hs + hbp + hact + hfp is the whole line.
   - vertical: lines, except for NTSC/PAL and interlaced modes, which count half
     lines a field (NTSC 480i: 6 + 6 + 26 + 480 + 6 + 1 = 525 half lines).

   The descriptions of the BIOS's modes (mx_crtc_ntsc, ...) give the same register
   values as the BIOS's SetGsCrt (checked on the host: tests/host/test_crtc.c). */

#include <mx/types.h>

enum {
  MX_CRTC_VESA = 0, /* computer monitor (RGB, sync on green) */
  MX_CRTC_HD = 1,   /* digital TV: 480p, 720p, 1080i (component) */
  MX_CRTC_NTSC = 2, /* broadcast TV */
  MX_CRTC_PAL = 3,
};

struct mx_crtc {
  u32 vck_hz;   /* video clock; the PLL setting nearest to it is used */
  u8 kind;      /* MX_CRTC_* */
  u8 spml;      /* VCK a pixel: 1, 2 or 4 */
  u8 interlace; /* 1: interlaced */
  u8 ffmd;      /* interlaced: 0 FIELD, 1 FRAME (SMODE2.FFMD) */
  u16 hs, hbp, hact, hfp; /* horizontal sync, back porch, active, front porch (VCK) */
  u16 hseq;               /* sync pulse width during vertical sync; 0: hs */
  u16 vs, vbpe, vbp, vdp, vfpe, vfp; /* vertical sync, back porch without and with
                                       colour burst, active, front porch without and
                                       with colour burst */
};

/* The register values for a description, in the order they are written. */
struct mx_crtc_regs {
  u64 smode1; /* the final value (PRST and SINT as the BIOS leaves them) */
  u64 smode2, srfsh, synch1, synch2, syncv;
};

/* Compute the registers. Returns 0, or -1 if a value does not fit its field. */
int mx_crtc_regs(const struct mx_crtc *c, struct mx_crtc_regs *r);

/* The VCK (Hz) of SMODE1's PLL fields: 13.5 MHz * LC / (RC << T1248). */
u32 mx_crtc_vck(u64 smode1);

/* The pixel clock: VCK / spml. */
MX_INLINE u32 mx_crtc_pixel_hz(const struct mx_crtc *c) { return c->vck_hz / c->spml; }

/* The description of a BIOS mode code (SetGsCrt's 0x02, 0x03, 0x50 ...), 0 for
   one there is none of (576p: not in all BIOSes). */
const struct mx_crtc *mx_crtc_for_code(int code, int interlace);

/* The BIOS's modes (SetGsCrt codes 0x02, 0x03, 0x50 ...). */
extern const struct mx_crtc mx_crtc_ntsc;      /* 480i (SetGsCrt 0x02) */
extern const struct mx_crtc mx_crtc_ntsc_p;    /* 240p */
extern const struct mx_crtc mx_crtc_pal;       /* 576i (0x03) */
extern const struct mx_crtc mx_crtc_pal_p;     /* 288p */
extern const struct mx_crtc mx_crtc_480p;      /* 0x50 */
extern const struct mx_crtc mx_crtc_720p;      /* 0x52 */
extern const struct mx_crtc mx_crtc_1080i;     /* 0x51 */
extern const struct mx_crtc mx_crtc_vesa_1a;   /* 640x480 60 Hz */
extern const struct mx_crtc mx_crtc_vesa_1b;   /* 640x480 72 Hz */
extern const struct mx_crtc mx_crtc_vesa_1c;   /* 640x480 75 Hz */
extern const struct mx_crtc mx_crtc_vesa_2a;   /* 800x600 56 Hz */
extern const struct mx_crtc mx_crtc_vesa_2b;   /* 800x600 60 Hz */
extern const struct mx_crtc mx_crtc_vesa_2c;   /* 800x600 72 Hz */
extern const struct mx_crtc mx_crtc_vesa_2d;   /* 800x600 75 Hz */
extern const struct mx_crtc mx_crtc_vesa_3b;   /* 1024x768 60 Hz */
extern const struct mx_crtc mx_crtc_vesa_3c;   /* 1024x768 70 Hz */
extern const struct mx_crtc mx_crtc_vesa_3d;   /* 1024x768 75 Hz */
extern const struct mx_crtc mx_crtc_vesa_3e;   /* 1024x768 85 Hz */
extern const struct mx_crtc mx_crtc_vesa_4a;   /* 1280x1024 60 Hz */
extern const struct mx_crtc mx_crtc_vesa_4b;   /* 1280x1024 75 Hz */

/* Modes the BIOS does not have, made the same way (not yet seen on a screen). */
extern const struct mx_crtc mx_crtc_1080i50;   /* 1920x1080 interlaced, 50 Hz */
extern const struct mx_crtc mx_crtc_720p50;    /* 1280x720, 50 Hz */
extern const struct mx_crtc mx_crtc_1080p30;   /* 1920x1080 progressive, 30 Hz (74.25 MHz) */

#endif
