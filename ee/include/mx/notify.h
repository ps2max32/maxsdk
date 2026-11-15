#ifndef MX_NOTIFY_H
#define MX_NOTIFY_H

/* Notifications (kernel version 26; docs/mx-os.md): the kernel keeps the
   last 32 in a ring, so that none depends on the shell running or being in
   front. The kernel adds its own (a device came or went, the swap file, a
   program that crashed); any program may add one. The shell shows them
   (toasts, a history) and is told of each new one or change by
   MX_EVENT_NOTIFY (<mx/proc.h>). libmxk only: on libmx the calls return -1
   or 0. */

#include <mx/types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  MX_NOTIFY_INFO = 0,
  MX_NOTIFY_DEVICE = 1, /* a device came or went */
  MX_NOTIFY_SWAP = 2,   /* the swap file: created, attached, removed, none */
  MX_NOTIFY_CRASH = 3,  /* a program crashed */
  MX_NOTIFY_PROGRAM = 4, /* from a program (mx_notify) */
  MX_NOTIFY_PAD = 5,     /* a controller came or went, its battery is low */
};
enum {
  MX_NOTIFY_ACTION_NONE = 0,
  MX_NOTIFY_ACTION_STORAGE = 1, /* the shell's Storage page */
  MX_NOTIFY_ACTION_SWAP = 2,    /* the shell's swap setting */
  MX_NOTIFY_ACTION_PADS = 3,    /* the shell's Controllers page */
};
#define MX_NOTIFY_TITLE 40  /* bytes with the 0: ASCII and Latin-1 */
#define MX_NOTIFY_TEXT 120

struct mx_notify {
  u32 id;      /* 1, 2, ... in order; the same id again when it changed */
  u32 seq;     /* the ring's counter when it was added or last changed */
  u32 time;    /* seconds since the kernel's boot */
  u8 kind;     /* MX_NOTIFY_* */
  u8 urgent;   /* 1: shown until it goes to 0 (e.g. the swap device may not be removed) */
  u8 action;   /* MX_NOTIFY_ACTION_* */
  u8 pad;
  char source[16]; /* the program's name, "" for the kernel */
  char title[MX_NOTIFY_TITLE];
  char text[MX_NOTIFY_TEXT];
};

/* Adds one: its id, or -1 */
int mx_notify(u32 kind, int urgent, const char *title, const char *text);
/* The notifications added or changed after seq (0: all in the ring), the
   oldest first, at most max of them in n: how many */
int mx_notify_read(u32 after_seq, struct mx_notify *n, int max);
/* The ring's counter now: changes with every new or changed notification */
u32 mx_notify_seq(void);

#ifdef __cplusplus
}
#endif

#endif
