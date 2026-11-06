/* libmx: no virtual memory. Everything is core, so what the VM offers has
   nothing to do, and programs and libgl need no #ifdef (docs/kernel.md). */

#include <mx/debug.h>
#include <mx/mx.h>
#include <mx/string.h>
#include <mx/vm.h>

#include "vm_int.h"

int mx_vm_init(const struct mx_vm_config *cfg) {
  (void)cfg;
  return -1;
}
int mx_vm_active(void) { return 0; }
u32 mx_vm_size(void) { return 0; }
const void *mx_vm_map_file(const char *path, u32 *size) {
  (void)path, (void)size;
  return 0;
}
void mx_vm_unmap_file(const void *p) { (void)p; }
int mx_vm_pin(const void *p, u32 bytes, u32 flags) {
  (void)p, (void)bytes, (void)flags;
  return 0;
}
void mx_vm_unpin(const void *p, u32 bytes) { (void)p, (void)bytes; }
void mx_vm_overwrite_done(const void *p, u32 bytes, u32 done) {
  (void)p, (void)bytes, (void)done;
}
void mx_vm_unpin_after(const void *p, u32 bytes, mx_event e) { (void)p, (void)bytes, (void)e; }
u32 mx_vm_unpin_done(void) { return 0; }
struct mx_vm_piece mx_vm_first_piece(const void *p, u32 bytes) {
  return (struct mx_vm_piece){mx_paddr_of(p), bytes};
}
u32 mx_vm_pieces(const void *p, u32 bytes, struct mx_vm_piece *pc, u32 max) {
  (void)max;
  pc[0] = mx_vm_first_piece(p, bytes);
  return 1;
}
void mx_vm_stats(struct mx_vm_stats *s) { memset(s, 0, sizeof *s); }
void mx_vm_trace_mark(u32 tag) { (void)tag; }
void mx_vm_trace_restart(void) {}
int mx_vm_trace_save(const char *path) {
  (void)path;
  return -1;
}

/* mx_is_vm is never true here, so these are not reached */
void (*mx__core_fault_hook)(const void *p, const char *what);
void mx__core_fault(const void *p, const char *what) { (void)p, (void)what; }
mx_paddr mx__vm_paddr(const void *p, u32 bytes) {
  (void)bytes;
  return mx_paddr_of(p);
}
void mx__paddr_fault(const void *p, u32 bytes) {
  mx_printf("VM: %u bytes at %p: no physical address\n", bytes, p);
  mx_exit(255);
}

/* the program's start and end, heaps of pages (mem.c, heap.c) */
int mx__vm_image(void) { return 0; }
void *mx__vm_image_end(void) { return mx_vm_base(); }
int mx__vm_load_image(const char *path) {
  (void)path;
  return -1;
}
void mx__vm_shutdown(void) {}
void mx__vm_swap_close(void) {}
void mx__vm_report(void) {}
void *mx__vm_frames_take(u32 bytes) {
  (void)bytes;
  return 0;
}
void mx__vm_frames_give(void *p, u32 bytes) { (void)p, (void)bytes; }
u32 mx__vm_va_take(u32 bytes) {
  (void)bytes;
  return 0;
}
void mx__vm_prefetch(const void *p, u32 bytes) { (void)p, (void)bytes; }
int mx__vm_resident(const void *p, u32 bytes) {
  (void)p, (void)bytes;
  return 1;
}
u32 mx__vm_resident_bytes(const void *p, u32 bytes) {
  (void)p;
  return bytes;
}
void mx__vm_discard(const void *p, u32 bytes) { (void)p, (void)bytes; }
void mx__vm_cold(const void *p, u32 bytes) { (void)p, (void)bytes; }
