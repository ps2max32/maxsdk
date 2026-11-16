#ifndef MX_TIME_H
#define MX_TIME_H

/* The date and time (kernel version 29; docs/kernel.md). The kernel reads the
   PS2's clock (battery backed, in the CD/DVD controller) at its start and
   counts on from there. The local time is UTC moved by the settings
   (<mx/setting.h>) time.zone (minutes east of UTC, "60" for CET, "-300" for
   EST; 0 when unset) and time.dst ("on": one hour more). libmxk only: on
   libmx mx_time is 0. */

#include <mx/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Seconds since 1970-01-01 00:00 UTC; 0 when the time is not known (the
   clock could not be read, PCSX2 without it) */
s64 mx_time(void);
/* The shell only: the time set to utc (also in the PS2's clock). 0, or -1
   (not the shell, a time outside 2000..2099, the clock not written) */
int mx_time_set(s64 utc);

struct mx_tm {
  u16 year;   /* 2026 */
  u8 month;   /* 1..12 */
  u8 day;     /* 1..31 */
  u8 hour;    /* 0..23 */
  u8 minute;  /* 0..59 */
  u8 second;  /* 0..59 */
  u8 weekday; /* 0 Sunday .. 6 Saturday */
};
/* t (mx_time's seconds) as a UTC date and time */
void mx_gmtime(s64 t, struct mx_tm *tm);
/* t as the local date and time (time.zone, time.dst); its offset from UTC
   in minutes */
int mx_localtime(s64 t, struct mx_tm *tm);

#ifdef __cplusplus
}
#endif

#endif
