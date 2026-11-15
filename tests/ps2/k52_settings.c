/* The settings store (kernel version 27, <mx/setting.h>).
     as the kernel's shell: k52_settings.elf set     (status 0: get, set, remove,
       list, the event, the limits; leaves test.k52.keep = "kept" and waits for
       the kernel to write the file)
     as the kernel's shell: k52_settings.elf check   (status 0: after a new
       boot of the kernel, test.k52.keep read back from the file; removed)
     not as the shell: k52_settings.elf noshell     (status 0: reads, but
       mx_setting_set refused)
     k52_settings.elf log: the kernel's log lines about the settings */
#include <mx/log.h>
#include <mx/msg.h>
#include <mx/proc.h>
#include <mx/setting.h>
#include <mx/string.h>
#include <mx/thread.h>
#include "t.h"

static int has(const char *s, const char *w) {
  for (; *s; s++)
    if (!strncmp(s, w, strlen(w)))
      return 1;
  return 0;
}

static int shell(void) {
  struct mx_proc_info p[8];
  int n = mx_proc_list(p, 8);
  for (int i = 0; i < n && i < 8; i++)
    if (p[i].id == mx_proc_self())
      return (p[i].flags & MX_PROC_SHELL) != 0;
  return 0;
}

static void show(u32 after) {
  struct mx_setting s[16];
  int n = mx_setting_list(after, s, 16);
  for (int i = 0; i < n; i++)
    mx_printf("k52:   %s = '%s' (seq %u)\n", s[i].key, s[i].value, s[i].seq);
}

static int event(int port, u32 *seq) {
  struct mx_event e;
  struct mx_msg m;
  if (mx_msg_receive(port, &e, sizeof e, &m) <= 0)
    return -1;
  *seq = (u32)e.status;
  return (int)e.type;
}

int main(int argc, char **argv) {
  const char *what = argc > 1 ? argv[1] : "set";
  char v[MX_SETTING_VALUE];
  u32 seq0 = mx_setting_seq();
  mx_printf("k52: %s, seq %u, shell %d\n", what, seq0, shell());
  show(0);

  if (!strcmp(what, "log")) {
    /* the kernel's pieces, joined into lines */
    static char line[512];
    u32 pos = 0, n = 0;
    char piece[65];
    int proc, got;
    while ((got = mx_log_read(&pos, piece, 64, &proc, 0)) > 0) {
      if (proc)
        continue;
      for (int i = 0; i < got; i++) {
        if (piece[i] == '\n' || n == sizeof line - 1) {
          line[n] = 0;
          if (has(line, "settings") || has(line, "shell") || has(line, "service"))
            mx_printf("k52 log: %s\n", line);
          n = 0;
        } else
          line[n++] = piece[i];
      }
    }
    return t_done("k52");
  }
  if (!strcmp(what, "noshell")) {
    CHECK(!shell());
    CHECK_EQ(mx_setting_set("test.k52", "x"), -1);
    CHECK_EQ(mx_setting_seq(), seq0);
    return t_done("k52");
  }
  CHECK(shell());

  if (!strcmp(what, "check")) {
    CHECK_EQ(mx_setting_get("test.k52.keep", v, sizeof v), 4);
    CHECK(!strcmp(v, "kept"));
    CHECK_EQ(mx_setting_get("test.k52", v, sizeof v), -1);
    CHECK_EQ(mx_setting_set("test.k52.keep", ""), 0);
    CHECK_EQ(mx_setting_get("test.k52.keep", v, sizeof v), -1);
    mx_printf("k52: waiting for the file\n");
    mx_sleep_us(5000000);
    return t_done("k52");
  }

  int port = mx_port_create("k52");
  CHECK(port > 0);
  CHECK_EQ(mx_proc_events(port), 0);

  /* set, get, the event */
  CHECK_EQ(mx_setting_get("test.k52", v, sizeof v), -1);
  CHECK_EQ(mx_setting_set("test.k52", "hello world"), 0);
  CHECK_EQ(mx_setting_seq(), seq0 + 1);
  CHECK_EQ(mx_setting_get("test.k52", v, sizeof v), 11);
  CHECK(!strcmp(v, "hello world"));
  CHECK_EQ(mx_setting_get("test.k52", v, 6), 5); /* cut to the room */
  CHECK(!strcmp(v, "hello"));
  u32 seq;
  CHECK_EQ(event(port, &seq), MX_EVENT_SETTING);
  CHECK_EQ(seq, seq0 + 1);
  /* the same value: no change */
  CHECK_EQ(mx_setting_set("test.k52", "hello world"), 0);
  CHECK_EQ(mx_setting_seq(), seq0 + 1);

  /* list after a seq: only what changed since */
  struct mx_setting s[4];
  CHECK_EQ(mx_setting_list(seq0, s, 4), 1);
  CHECK(!strcmp(s[0].key, "test.k52") && s[0].seq == seq0 + 1);

  /* removed: listed with "" (the shell's copy learns it), not read */
  CHECK_EQ(mx_setting_set("test.k52", 0), 0);
  CHECK_EQ(mx_setting_get("test.k52", v, sizeof v), -1);
  CHECK_EQ(mx_setting_list(seq0 + 1, s, 4), 1);
  CHECK(!strcmp(s[0].key, "test.k52") && !s[0].value[0]);
  CHECK_EQ(event(port, &seq), MX_EVENT_SETTING);

  /* the limits */
  char longk[MX_SETTING_KEY + 1], longv[MX_SETTING_VALUE + 1];
  memset(longk, 'k', sizeof longk - 1), longk[sizeof longk - 1] = 0;
  memset(longv, 'v', sizeof longv - 1), longv[sizeof longv - 1] = 0;
  CHECK_EQ(mx_setting_set(longk, "x"), -1);
  CHECK_EQ(mx_setting_set("test.k52", longv), -1);
  longv[MX_SETTING_VALUE - 1] = 0; /* 63: fits */
  CHECK_EQ(mx_setting_set("test.k52", longv), 0);
  CHECK_EQ(mx_setting_get("test.k52", v, sizeof v), MX_SETTING_VALUE - 1);
  CHECK_EQ(mx_setting_set("test.k52", ""), 0);
  CHECK_EQ(mx_setting_set("a=b", "x"), -1);
  CHECK_EQ(mx_setting_set("a b", "x"), -1);
  CHECK_EQ(mx_setting_set("test.k52", "two\nlines"), -1);
  CHECK_EQ(mx_setting_set("", "x"), -1);

  /* kept for "check", after the kernel wrote it (3 s after the last change) */
  CHECK_EQ(mx_setting_set("test.k52.keep", "kept"), 0);
  mx_printf("k52: now:\n");
  show(0);
  mx_printf("k52: waiting for the file\n");
  mx_sleep_us(5000000);
  return t_done("k52");
}
