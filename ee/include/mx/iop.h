#ifndef MX_IOP_H
#define MX_IOP_H

/* IOP memory, modules and the patches of the ROM modules (ps2sdk's sbv), all
   from the EE: IOP RAM is read over SIF (RPC RDATA) and written by SIF DMA.
   maxsdk's IOP modules are in iop/. Not for interrupt handlers, nor for
   two threads at once. */

#include <mx/types.h>

/* Reset the IOP (ps2sdk's SifIopReset + SifIopSync): it boots again with
   the ROM's modules, or with those of an IOPRP image when arg names one
   ("rom0:UDNL <path>"; 0 or "" for none), and the SIF is set up again.
   Every module loaded before is gone, and every binding (mx_sif_bound);
   maxsdk's own clients bind again, the pad and audsrv do not (mx_pad_init,
   mx_snd_init again). Thread code, before any file is open. Returns 0, < 0
   when the IOP does not come back within 10 s. */
int mx_iop_reset(const char *arg);

/* Reboot the IOP with the IOPRP image of size bytes at img (EE memory), as
   ps2sdk's SifIopRebootBuffer: its modules take the place of the ROM's of
   the same name (e.g. a SECRMAN), the ROM's IOPBTCONF stays (an image with
   an IOPBTCONF of its own is not supported). A plain reset first, then as
   mx_iop_reset. Not under mx-kernel (libmxk): returns -1 there. */
int mx_iop_reboot_image(const void *img, u32 size);

/* Copy size bytes of IOP RAM at iop (any alignment) to dst. */
int mx_iop_read(u32 iop, void *dst, u32 size);
/* Copy size bytes from src to IOP RAM at iop: both 16-byte aligned, size a
   multiple of 16 (SIF DMA). */
int mx_iop_write(u32 iop, const void *src, u32 size);
/* Write one word (4-byte aligned) of IOP RAM: the 16 bytes around it are read
   and written back. */
int mx_iop_write_word(u32 iop, u32 value);

/* The IOP heap (RPC 0x80000003, served by FILEIO). 0 / < 0 on failure. */
u32 mx_iop_alloc(u32 size);
int mx_iop_free(u32 iop);

/* LOADCORE's record of a loaded module (ModuleInfo). */
struct mx_iop_module {
  u32 next; /* the next record, 0 at the end */
  u32 name; /* IOP address of the name */
  u16 version, flags, id, unused;
  u32 entry, gp, text_start, text_size, data_size, bss_size, unused1, unused2;
};

/* The module named name (exactly): returns its id and its record in *m, 0 if
   it is not loaded. */
int mx_iop_find_module(const char *name, struct mx_iop_module *m);
/* The highest id of the loaded modules (ids only grow: a higher one than
   before means a module was loaded since), < 0 on an error. */
int mx_iop_last_module(void);
/* The export library named name (e.g. "modload"): its first max function
   addresses go in exports (if exports); returns how many it has, 0 if none. */
int mx_iop_find_exports(const char *name, u32 *exports, int max);

/* The patches. Each returns 0 when it patched, 1 when the patch is there
   already (ps2link applies the LMB and prefix check ones itself, after its
   IOP reset) or the module does not need it, < 0 on an error. */

/* rom0:FILEIO 1.1: getstat and dread call sceSifSetDma with interrupts on
   (a race with the other SIF DMA of the IOP, e.g. PADMAN's), and remove goes
   on into mkdir. mx_fio_dopen applies it the first time. */
int mx_iop_patch_fileio(void);
/* rom0:LOADFILE has no LoadModuleBuffer (function 6): add it. */
int mx_iop_patch_lmb(void);
/* MODLOAD loads unprotected modules and ELFs only from some devices: from
   all of them. */
int mx_iop_patch_prefix_check(void);

/* Load and start the IRX module of size bytes at buf (EE memory; it is copied
   into the IOP heap, the copy freed after). Applies mx_iop_patch_lmb. Returns
   the module id (< 0 on failure); its start result goes in *modres (if
   modres). */
int mx_iop_load_module_buffer(const void *buf, u32 size, const char *args, u32 args_len,
                              int *modres);

/* An IOP channel (kernel version 17; libmxk only, libmx returns -1): what an
   IOP module sends with sceSifSendCmd(cid, ...), cid 0-31, comes as a
   message (<mx/msg.h>, from process 0, no reply wanted) to port, a port of
   this process: the packet's payload, without its 16-byte header (up to 96
   bytes). One process per cid; port 0 gives it up; the process's end too.
   0, or -1 (taken, not its port, cid out of range). */
int mx_iop_channel(u32 cid, int port);

#endif
