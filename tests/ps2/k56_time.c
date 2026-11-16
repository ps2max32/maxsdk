/* The time (kernel version 29, <mx/time.h>): mx_time from the PS2's clock,
   counting on; mx_gmtime and mx_localtime; mx_time_set refused for a
   program that is not the shell.
     k56_time.elf      (status 0) */
#include <mx/thread.h>
#include <mx/time.h>
#include "t.h"

static void show(const char *what, const struct mx_tm *tm) {
  static const char *wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  mx_printf("k56: %s %04u-%02u-%02u %02u:%02u:%02u %s\n", what, tm->year, tm->month, tm->day, tm->hour,
            tm->minute, tm->second, wd[tm->weekday % 7]);
}

int main(void) {
  /* the date arithmetic on known days */
  struct mx_tm tm;
  mx_gmtime(0, &tm);
  CHECK(tm.year == 1970 && tm.month == 1 && tm.day == 1 && tm.weekday == 4);
  mx_gmtime(951782400, &tm); /* 2000-02-29 00:00, a Tuesday */
  CHECK(tm.year == 2000 && tm.month == 2 && tm.day == 29 && tm.hour == 0 && tm.weekday == 2);
  mx_gmtime(1794700800 + 3723, &tm); /* 2026-11-15 01:02:03, a Sunday */
  CHECK(tm.year == 2026 && tm.month == 11 && tm.day == 15 && tm.hour == 1 && tm.minute == 2 &&
        tm.second == 3 && tm.weekday == 0);

  s64 t = mx_time();
  mx_printf("k56: mx_time %lld\n", (long long)t);
  CHECK(t > 1735689600); /* after 2025-01-01: the clock was read */
  mx_gmtime(t, &tm);
  show("UTC  ", &tm);
  int off = mx_localtime(t, &tm);
  mx_printf("k56: local offset %d minutes\n", off);
  show("local", &tm);
  mx_sleep_us(2100000);
  s64 u = mx_time();
  mx_printf("k56: 2.1 s later: +%lld s\n", (long long)(u - t));
  CHECK(u - t >= 2 && u - t <= 3);
  CHECK_EQ(mx_time_set(t), -1); /* not the shell */
  return t_done("k56");
}
