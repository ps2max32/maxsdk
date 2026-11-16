#ifndef MX_SCREEN_H
#define MX_SCREEN_H

/* The screen the system is set up for, and the pictures that fill it
   (MXOS kernel version 31; docs/mx-os.md "Video").

   The shell's settings say what the TV shows (all keys optional):
     video.mode      240p 288p 480i 576i 480p 576p 720p 720p50 1080i 1080i50
                     1080p24 1080p25 1080p30: the shell's and the kernel's
                     mode (default: the console's region, 480i or 576i)
     video.out       component (default) or rgb (SCART RGB, composite,
                     S-video: 240p, 288p, 480i and 576i only, the GS sends RGB)
     video.scale     stretch (default): every program in video.mode; native:
                     a program may take any mode of video.allowed
     video.allowed   the modes confirmed on this TV, "480i 480p 1080i"
                     (the region's 480i or 576i always is)
     video.aspect    4:3 (default) or 16:9: the TV's shape in 240-576 line
                     modes (720p and 1080 modes are 16:9)
     video.overscan  on (default): 240-576 line modes keep to the area every
                     TV shows (640 pixels wide, 224, 256, 448 or 512 lines);
                     off: the whole picture (704 x 240, 288, 480 or 576), for
                     TVs that show all of it
     video.confirm   "<mode> <out>" the user kept; the kernel puts video.mode
                     and video.out back to it 15 s after a change that is not
                     confirmed (the shell writes it when the user says yes)

   A screen is a picture of w x h pixels and how the GS shows it: a mode, a
   whole number of VCK per pixel (MAGH) and of lines per buffer line
   (MAGV), centred. Only whole numbers: the GS cannot scale by 1.5, so a
   mode has a few sizes that fill it (1080i: 1920, 960, 640, 480, 384 or 320
   wide; 540, 270, 180 or 135 lines), and a picture of another size or
   shape is shown at the nearest whole factors with black borders.

   Programs set their mode through this: mx_screen_pick for the size they
   draw, then mx_screen_video for the mode to set (mx_gs_init_video) and
   their display (mx_display_init fits the picture to it). mx_gs_init does
   that for its w x h. */

#include <mx/types.h>
#include <mx/video.h>

enum {
  MX_SCREEN_480I = 1,
  MX_SCREEN_576I,
  MX_SCREEN_480P,
  MX_SCREEN_576P,
  MX_SCREEN_720P,
  MX_SCREEN_1080I,
  MX_SCREEN_240P, /* NTSC and PAL timing, every field the same lines */
  MX_SCREEN_288P,
  MX_SCREEN_720P50, /* the 50 Hz and 1080p modes: the kernel's own timing (no
                       BIOS code); not on consoles the kernel cannot set directly */
  MX_SCREEN_1080I50,
  MX_SCREEN_1080P24,
  MX_SCREEN_1080P25,
  MX_SCREEN_1080P30,
  MX_SCREEN_MODES /* one past the last */
};

struct mx_screen {
  u8 mode;       /* MX_SCREEN_* */
  u8 ffmd;       /* interlaced: 1 every buffer line in both fields (FRAME), 0 a field
                    shows every other line (FIELD; as mx_video_mode.ffmd) */
  u8 magh, magv; /* VCK a pixel (1-16); DISPLAY lines a buffer line, over
                    mx_video_line_lines (1-4) */
  u16 w, h;      /* the picture */
  u16 aspect;    /* its shape on the TV: width / height x 1000 (4:3 1333, 16:9 1778) */
  u16 cover;     /* how much of the mode's area it covers, x 1000 (1000: fills it) */
  u8 flags;      /* MX_SCREEN_F_* */
  u8 hz;         /* fields (progressive: frames) a second */
};
#define MX_SCREEN_F_SHELL 1  /* the mode is video.mode (the shell's) */
#define MX_SCREEN_F_FILL 2   /* fills the area in both directions */
#define MX_SCREEN_F_DOUBLE 4 /* two 32-bit buffers and a 16-bit Z of w x h fit GS memory */

/* The settings as read (struct, so the kernel can fill it from its store). */
struct mx_screen_conf {
  u8 mode;     /* video.mode as it is used: MX_SCREEN_*, the region's SD mode if
                  the setting is missing, unknown or not possible with out */
  u8 rgb;      /* video.out rgb */
  u8 native;   /* video.scale native */
  u8 wide;     /* video.aspect 16:9 */
  u8 full;     /* video.overscan off: the whole 704 x 480/576 */
  u8 pal;      /* the console's region is PAL (576i is its SD mode) */
  u16 allowed; /* bit per MX_SCREEN_* mode: video.allowed, the region's SD mode, mode */
};
/* conf from the settings (mx_setting_get; none: the defaults). */
void mx_screen_conf(struct mx_screen_conf *c);
/* The same through another reader of the settings (the kernel's). */
void mx_screen_conf_from(struct mx_screen_conf *c,
                         int (*get)(const char *key, char *value, u32 size));

/* The modes a program may take: video.mode, and with video.scale native
   the allowed ones too. Bit per MX_SCREEN_* mode. */
u32 mx_screen_modes(const struct mx_screen_conf *c);
/* The pictures that fill one of them (MX_SCREEN_F_FILL), the largest
   first; up to max. Returns how many there are. */
int mx_screen_fills(const struct mx_screen_conf *c, u32 mode, struct mx_screen *s, int max);

/* The best screen for a picture of w x h of shape aspect (x 1000; 0: as
   the pixels are square-ish, w / h x 1000 is not used, any shape):
   exact: w x h as given, at the whole factors that show it largest with the
   shape nearest aspect; else the size that fills a mode nearest to w x h,
   narrowed or shortened to aspect (borders). Over the modes the program may
   take; with video.scale native one that shows w x h at whole factors with
   the right shape goes before video.mode. Returns 0, or -1 (no mode fits). */
#define MX_SCREEN_EXACT 1u
int mx_screen_pick(const struct mx_screen_conf *c, u32 w, u32 h, u32 aspect, u32 flags,
                   struct mx_screen *s);
/* The best whole factors for w x h in one mode (exact, as above). 0, or -1. */
int mx_screen_fit(const struct mx_screen_conf *c, u32 mode, u32 w, u32 h, u32 aspect,
                  struct mx_screen *s);

/* The video mode to set for s: the mode with its area (overscan), FRAME or
   FIELD as s. A display of the w x h picture (mx_display_init) is then
   shown at s's factors, centred. */
void mx_screen_video(const struct mx_screen_conf *c, const struct mx_screen *s,
                     struct mx_video_mode *m);

/* "480i" ...; MX_SCREEN_* of a name, 0 if none. */
const char *mx_screen_name(u32 mode);
u32 mx_screen_mode_of(const char *name);

#endif
