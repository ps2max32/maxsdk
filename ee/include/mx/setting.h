#ifndef MX_SETTING_H
#define MX_SETTING_H

/* The system's settings (kernel version 27; docs/mx-os.md, Settings): one
   store, mc0:/MXOS/SETTINGS.DAT, which the kernel reads at its boot and keeps,
   and writes a few seconds after the last change. Keys are short names with
   dots ("video.mode", "net.ip", "swap.path", "pad.swap_xo"); values are text.
   Any program reads them; only the shell changes them. A change is
   MX_EVENT_SETTING (<mx/proc.h>) to every process that asked for events.
   libmxk only: on libmx (and a kernel before 27) get and set return -1, list and seq 0. */

#include <mx/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MX_SETTING_KEY 32   /* bytes with the 0 */
#define MX_SETTING_VALUE 64

/* key's value into value (at most size bytes with the 0): its length, or -1
   when it is not set */
int mx_setting_get(const char *key, char *value, u32 size);
/* key set to value ("" or 0: removed). 0, or -1 (not the shell, a key or
   value too long, the store full). From INIT.ELF (kernel version 46, the boot
   script, docs/mx-os.md) for this boot only: not written to SETTINGS.DAT,
   read before the store; the shell's own change of the key drops it. */
int mx_setting_set(const char *key, const char *value);

struct mx_setting {
  char key[MX_SETTING_KEY];
  char value[MX_SETTING_VALUE]; /* "" when it was removed */
  u32 seq;                      /* the store's counter at its last change */
};
/* The settings changed after seq (0: all), at most max in s: how many */
int mx_setting_list(u32 after_seq, struct mx_setting *s, int max);
/* The store's counter now: changes with every change */
u32 mx_setting_seq(void);

#ifdef __cplusplus
}
#endif

#endif
