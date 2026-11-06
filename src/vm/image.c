/* Programs linked into VM (ld/maxsdk-vm.ld): the segments at MX_VM_BASE
   and up come from the program's ELF (argv[0], the path mxlink gives),
   which stays open: once VM is on, before the constructors, their pages
   are registered with the pager (mx__vm_file_pages), and each is read when
   it is first touched. A page not written since is dropped when evicted
   and read again; a written one goes to the swap file. The bss after a
   segment's bytes is new pages (zero). */

#include <mx/debug.h>
#include <mx/sif.h>
#include <mx/vm.h>

extern char __mx_vm_start[] __attribute__((weak));
extern char __mx_vm_end[] __attribute__((weak));

void mx__vm_code(void);
int mx__vm_file_pages(int fd, u32 va, u32 filesz, u32 offset);

/* The program's VM part: its own, or in the kernel the one it says it has. */
static char *image_lo, *image_hi;
void mx__vm_image_range(void *lo, void *hi) { image_lo = lo, image_hi = hi; }

static char *image_end(void) { return image_hi ? image_hi : __mx_vm_end; }

int mx__vm_image(void) {
  return image_hi ? image_hi > image_lo : (uintptr_t)__mx_vm_end > (uintptr_t)__mx_vm_start;
}

void *mx__vm_image_end(void) { return mx__vm_image() ? image_end() : mx_vm_base(); }

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

static int bad(const char *path, const char *what) {
  mx_printf("maxsdk: %s: %s\n", path, what);
  return -1;
}

int mx__vm_load_image(const char *path) {
  u32 end = (u32)(uintptr_t)image_end();
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
    if (p->filesz && mx__vm_file_pages(fd, p->vaddr, p->filesz, p->offset) < 0) {
      bad(path, "a VM segment the pager does not take (not on a page, or more than 4)");
      goto out;
    }
  }
  return 0; /* fd stays open: the pager reads from it */
out:
  mx_fio_close(fd);
  return r;
}
