/* Programs linked into VM (ld/maxsdk-vm.ld): the segments at MX_VM_BASE
   and up, read from the program's ELF (argv[0], the path mxload gives)
   once VM is on, before the constructors. Each page is read like any data
   (mx_fio_read into VM: the pages are made resident and dirty), so a page
   evicted later goes to the swap file. */

#include <mx/bios.h>
#include <mx/debug.h>
#include <mx/sif.h>
#include <mx/vm.h>

extern char __mx_vm_start[] __attribute__((weak));
extern char __mx_vm_end[] __attribute__((weak));

void mx__vm_code(void);

int mx__vm_image(void) { return (uintptr_t)__mx_vm_end > (uintptr_t)__mx_vm_start; }

void *mx__vm_image_end(void) { return mx__vm_image() ? __mx_vm_end : mx_vm_base(); }

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
#define PF_X 1
#define MAX_PHDRS 16
#define CHUNK 65536u

static int bad(const char *path, const char *what) {
  mx_printf("maxsdk: %s: %s\n", path, what);
  return -1;
}

int mx__vm_load_image(const char *path) {
  u32 end = (u32)(uintptr_t)__mx_vm_end;
  if (end - MX_VM_BASE > mx_vm_size())
    return bad(path, "the program's VM part is larger than VM (mx_vm_config: a swap file?)");
  static struct ehdr eh;
  static struct phdr ph[MAX_PHDRS];
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return bad(path, "cannot open the program's ELF");
  int r = -1;
  if (mx_fio_read(fd, &eh, sizeof eh) != sizeof eh || eh.phentsize != sizeof(struct phdr) ||
      eh.phnum > MAX_PHDRS) {
    bad(path, "not the program's ELF");
    goto out;
  }
  u32 phbytes = eh.phnum * sizeof(struct phdr);
  if (mx_fio_lseek(fd, (s32)eh.phoff, MX_FIO_SEEK_SET) != (int)eh.phoff ||
      mx_fio_read(fd, ph, phbytes) != (int)phbytes) {
    bad(path, "cannot read the program headers");
    goto out;
  }
  for (u32 i = 0; i < eh.phnum; i++) {
    const struct phdr *p = &ph[i];
    if (p->type != PT_LOAD || p->vaddr < MX_VM_BASE || p->vaddr >= MX_VM_LIMIT)
      continue;
    if (p->vaddr + p->memsz > end) {
      bad(path, "a VM segment outside __mx_vm_start - __mx_vm_end");
      goto out;
    }
    if (p->flags & PF_X)
      mx__vm_code();
    /* the rest (bss) is new pages: zero */
    if (mx_fio_lseek(fd, (s32)p->offset, MX_FIO_SEEK_SET) != (int)p->offset) {
      bad(path, "cannot seek to a VM segment");
      goto out;
    }
    u8 *d = (u8 *)(uintptr_t)p->vaddr;
    for (u32 done = 0; done < p->filesz;) {
      /* to the end of the chunk-aligned piece, so a read pins few pages */
      u32 n = CHUNK - ((p->vaddr + done) & (CHUNK - 1));
      if (n > p->filesz - done)
        n = p->filesz - done;
      if (mx_fio_read(fd, d + done, n) != (int)n) {
        bad(path, "cannot read a VM segment");
        goto out;
      }
      done += n;
    }
  }
  r = 0;
out:
  mx_fio_close(fd);
  mx_bios_flush_cache(0);
  mx_bios_flush_cache(2);
  return r;
}
