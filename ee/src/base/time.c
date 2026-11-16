/* <mx/time.h>: the kernel's time (version 29) and dates from it. Built once
   for libmx and libmxk: the kernel is asked at run time. */
#include <mx/kernel.h>
#include <mx/setting.h>
#include <mx/time.h>

#define HAS29(f)                                                                             \
  (mx__kernel && mx__kernel->version >= 29 &&                                                \
   mx__kernel->size >= __builtin_offsetof(struct mxk_services, f) + sizeof(void *))

s64 mx_time(void) { return HAS29(time_now) ? mx__kernel->time_now() : 0; }
int mx_time_set(s64 utc) { return HAS29(time_set) ? mx__kernel->time_set(utc) : -1; }

void mx_gmtime(s64 t, struct mx_tm *tm) {
  s64 days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
  u32 sec = (u32)(t - days * 86400);
  /* H. Hinnant's civil_from_days */
  s64 z = days + 719468;
  s64 era = (z >= 0 ? z : z - 146096) / 146097;
  u32 doe = (u32)(z - era * 146097);
  u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  u32 mp = (5 * doy + 2) / 153;
  u32 d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
  tm->year = (u16)((s64)yoe + era * 400 + (m <= 2));
  tm->month = (u8)m, tm->day = (u8)d;
  tm->hour = (u8)(sec / 3600), tm->minute = (u8)(sec / 60 % 60), tm->second = (u8)(sec % 60);
  tm->weekday = (u8)((days % 7 + 11) % 7); /* 1970-01-01 was a Thursday */
}

int mx_localtime(s64 t, struct mx_tm *tm) {
  char v[16];
  int off = 0, neg = 0;
  if (mx_setting_get("time.zone", v, sizeof v) > 0) {
    const char *p = v;
    if (*p == '+' || *p == '-')
      neg = *p++ == '-';
    for (; *p >= '0' && *p <= '9'; p++)
      off = off * 10 + (*p - '0');
    if (neg)
      off = -off;
  }
  if (mx_setting_get("time.dst", v, sizeof v) > 0 && (v[0] == 'o' && v[1] == 'n'))
    off += 60;
  mx_gmtime(t + (s64)off * 60, tm);
  return off;
}
