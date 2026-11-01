/* mxload: loads a maxsdk program and starts it, as ps2link does, but by
   itself: the EE reads the ELF through the file server (mx_fio_read) and
   puts each segment in place, where ps2link has the IOP do it (SifLoadElf).

     mxload.elf host:app.elf [args...]

   With no arguments it reads them from host:mxload.txt (one line), for
   PCSX2, which starts an ELF without any.

   mxload is linked high (MXLOAD_BASE, --defsym=__mx_base in the Makefile)
   and loads programs below itself, from 0x100000. Segments in VM (a program
   linked with ld/maxsdk-vm.ld) are left to the program's start-up. It then removes its
   interrupt handlers and jumps to the program's __start in its own thread,
   with a0 = an argument block like ps2link's (<mx/loader.h>), MX_LOADER_MAGIC
   in place of the pid and argv[0] the program's path. The program's start-up
   (src/base/start.c) copies the block before its heap reaches it.

   Progress on the debug screen (<mx/debug.h>): slot 0 and the colour the
   step (1 arguments, 2 ELF header, 3 segments, 4 start), slot 1 the
   segment, slot 2 its address, slot 3 its size. On an error the screen is
   red with BAD000nn in slot 0 and mxload returns to ps2link. */

#include <mx/bios.h>
#include <mx/debug.h>
#include <mx/irq.h>
#include <mx/loader.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/string.h>

struct ehdr {
  u8 ident[16];
  u16 type, machine;
  u32 version, entry, phoff, shoff, flags;
  u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct phdr {
  u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

#define PT_LOAD 1
#define MAX_PHDRS 16
#define LOW 0x00100000u

extern char __text_start[];

static struct mx_loader_args largs;
static char argline[256];

static int fail(u32 code, const char *what, const char *path) {
  mx_printf("mxload: %s: %s\n", path ? path : "", what);
  mx_dbg_color(0x0000FF);
  mx_dbg_word(0, 0xBAD00000u | code);
  return (int)code;
}

/* argv for the program: ours from argv[1] on, or host:mxload.txt's words */
static int set_args(int argc, char **argv) {
  struct mx_args *a = &largs.args;
  char *p = a->payload, *end = a->payload + sizeof a->payload;
  a->argc = 0;
  if (argc >= 2) {
    for (int i = 1; i < argc && a->argc < MX_MAX_ARGS; i++) {
      u32 n = strlen(argv[i]) + 1;
      if (p + n > end)
        return -1;
      memcpy(p, argv[i], n);
      a->argv[a->argc++] = p;
      p += n;
    }
    return 0;
  }
  int fd = mx_fio_open("host:mxload.txt", MX_FIO_RDONLY);
  if (fd < 0)
    return -1;
  int n = mx_fio_read(fd, argline, sizeof argline - 1);
  mx_fio_close(fd);
  if (n <= 0)
    return -1;
  argline[n] = 0;
  for (char *s = argline; *s && a->argc < MX_MAX_ARGS;) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
      s++;
    if (!*s)
      break;
    a->argv[a->argc++] = p;
    while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') {
      if (p + 1 >= end)
        return -1;
      *p++ = *s++;
    }
    *p++ = 0;
  }
  return a->argc ? 0 : -1;
}

int main(int argc, char **argv) {
  mx_dbg_screen();
  mx_dbg_step(1);
  if (set_args(argc, argv) < 0)
    return fail(1, "usage: mxload.elf host:app.elf [args...] (or them in host:mxload.txt)", 0);
  const char *path = largs.args.argv[0];

  mx_dbg_step(2);
  static struct ehdr eh;
  static struct phdr ph[MAX_PHDRS];
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return fail(2, "cannot open", path);
  if (mx_fio_read(fd, &eh, sizeof eh) != sizeof eh || eh.ident[0] != 0x7f || eh.ident[1] != 'E' ||
      eh.ident[2] != 'L' || eh.ident[3] != 'F' || eh.machine != 8 /* MIPS */)
    return fail(3, "not a MIPS ELF", path);
  if (eh.phentsize != sizeof(struct phdr) || eh.phnum == 0 || eh.phnum > MAX_PHDRS)
    return fail(4, "unexpected program headers", path);
  u32 phbytes = eh.phnum * sizeof(struct phdr);
  if (mx_fio_lseek(fd, (s32)eh.phoff, MX_FIO_SEEK_SET) != (int)eh.phoff ||
      mx_fio_read(fd, ph, phbytes) != (int)phbytes)
    return fail(5, "cannot read the program headers", path);

  mx_dbg_step(3);
  u32 top = (u32)(uintptr_t)__text_start;
  for (u32 i = 0; i < eh.phnum; i++) {
    const struct phdr *p = &ph[i];
    if (p->type != PT_LOAD || p->memsz == 0)
      continue;
    /* VM: the program reads it itself once VM is on (src/vm/image.c) */
    if (p->vaddr >= MX_VM_BASE && p->vaddr < MX_VM_LIMIT)
      continue;
    mx_dbg_word(1, i);
    mx_dbg_word(2, p->vaddr);
    mx_dbg_word(3, p->memsz);
    if (p->filesz > p->memsz || p->vaddr < LOW || p->vaddr + p->memsz > top ||
        p->vaddr + p->memsz < p->vaddr) {
      mx_printf("mxload: segment %u at %08x, %u bytes: not in %08x-%08x\n", i, p->vaddr,
                p->memsz, LOW, top);
      return fail(6, "a segment is not where mxload can load it", path);
    }
    u8 *d = (u8 *)(uintptr_t)p->vaddr;
    if (p->filesz && (mx_fio_lseek(fd, (s32)p->offset, MX_FIO_SEEK_SET) != (int)p->offset ||
                      mx_fio_read(fd, d, p->filesz) != (int)p->filesz))
      return fail(7, "cannot read a segment", path);
    memset(d + p->filesz, 0, p->memsz - p->filesz);
  }
  mx_fio_close(fd);

  mx_dbg_step(4);
  mx_printf("mxload: %s: starting at %08x\n", path, eh.entry);
  mx_debug_flush();
  mx_bios_flush_cache(0); /* write the D-cache back */
  mx_bios_flush_cache(2); /* and drop the I-cache */
  mx_irq_exit();
  mx_sif_exit();
  largs.pid = MX_LOADER_MAGIC;
  ((void (*)(struct mx_loader_args *))(uintptr_t)eh.entry)(&largs);
  return 0;
}
