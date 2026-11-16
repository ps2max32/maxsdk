/* <mx/screen.h>'s mx_home_picture: the kernel's (version 32). Built once for
   libmx and libmxk: the kernel is asked at run time. */
#include <mx/kernel.h>
#include <mx/screen.h>

static int have(void) {
  return mx__kernel && mx__kernel->version >= 32 &&
         mx__kernel->size >= __builtin_offsetof(struct mxk_services, home_picture_read) + sizeof(void *);
}

int mx_home_picture(struct mx_picture *p) { return have() ? mx__kernel->home_picture(p) : -1; }

int mx_home_picture_read(void *buf, u32 bytes) {
  return have() ? mx__kernel->home_picture_read(buf, bytes) : -1;
}
