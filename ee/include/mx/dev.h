#ifndef MX_DEV_H
#define MX_DEV_H

/* Storage devices and MXOS's swap file (kernel version 26, mx OS plan step
   4e; docs/mx-os.md, Devices). The kernel loads the storage drivers (USB,
   MX4SIO, the HDD) at its boot and keeps them while programs run: a program
   never loads a storage IRX. Devices come and go at any time (a USB stick,
   a memory card); the kernel sees it, tells every process that asked for
   events (MX_EVENT_DEVICE, <mx/proc.h>) and adds a notification
   (<mx/notify.h>). libmxk only: on libmx the calls return -1 or 0. */

#include <mx/types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  MX_DEV_MC = 1,     /* a memory card: mc0:, mc1: */
  MX_DEV_USB = 2,    /* a USB drive: usb0:, usb1: */
  MX_DEV_MX4SIO = 3, /* an SD card through MX4SIO: mx4sio0: */
  MX_DEV_HDD = 4,    /* the internal HDD: ata0: */
  MX_DEV_HOST = 5,   /* the PC through the host link: host: */
};
enum {
  MX_DEV_FS_NONE = 0, /* not known, or not mounted */
  MX_DEV_FS_MCFS = 1,
  MX_DEV_FS_FAT = 2, /* FAT12/16/32 */
  MX_DEV_FS_EXFAT = 3,
};
enum {
  MX_DEV_SWAP = 1,     /* MXOS's swap file is on it: do not remove it */
  MX_DEV_BUSY = 2,     /* files are open on it */
  MX_DEV_READONLY = 4, /* it cannot be written */
};

struct mx_dev_info {
  char name[12];   /* "usb0:", "mc1:", ... */
  u8 kind;         /* MX_DEV_* */
  u8 present;      /* 1: there now; 0: went (kept in the list until the next change) */
  u8 fs;           /* MX_DEV_FS_* */
  u8 flags;        /* MX_DEV_SWAP, MX_DEV_BUSY, MX_DEV_READONLY */
  u64 size;        /* bytes, 0 when not known */
  u64 free;        /* bytes free, 0 when not known (FAT and exFAT: not yet) */
  char label[24];  /* the volume label, "" when it has none or it is not known */
};

/* The devices, at most max of them in d: how many there are */
int mx_dev_list(struct mx_dev_info *d, int max);
/* A counter that changes with every change of the list: compare it after
   being away (a game in front) instead of counting events */
u32 mx_dev_gen(void);
/* Ready to remove: 0 when nothing is open on it and it does not hold the
   swap file (it can be pulled out now), -1 when it does (a notification
   says what) */
int mx_dev_eject(const char *name);

/* MXOS's swap file. mx_swap_set chooses where it is (a path on a device
   that is there, e.g. "usb0:/PS2MAX/swap.bin", and its size in MiB; 0: 64)
   or that there is none (path "" or 0: MXOS then warns that programs run
   out of memory soon). It returns at once: the kernel writes the setting
   (mc0:/MXOS/SWAP.CFG), makes the file if needed and attaches it in the
   background, and reports through notifications (created, attached,
   failed). 0, or -1 (a program not the shell, a bad path). */
int mx_swap_set(const char *path, u32 mib);

enum {
  MX_SWAP_NONE = 0,     /* none set */
  MX_SWAP_WAITING = 1,  /* set, its device is not there */
  MX_SWAP_CREATING = 2, /* being made */
  MX_SWAP_ATTACHED = 3, /* in use */
  MX_SWAP_FAILED = 4,   /* could not be made or opened */
};
struct mx_swap_info {
  char path[64]; /* the setting, "" for none */
  u32 mib;       /* its size */
  u32 state;     /* MX_SWAP_* */
  char device[12]; /* its device ("usb0:"), "" for none */
};
int mx_swap_info(struct mx_swap_info *s);

#ifdef __cplusplus
}
#endif

#endif
