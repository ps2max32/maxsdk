/* <mx/net.h>'s mx_net_status: the kernel's (version 30). Built once for
   libmx and libmxk: the kernel is asked at run time. */
#include <mx/kernel.h>
#include <mx/net.h>

int mx_net_status(struct mx_net_status *s) {
  if (!mx__kernel || mx__kernel->version < 30 ||
      mx__kernel->size < __builtin_offsetof(struct mxk_services, net_status) + sizeof(void *))
    return -1;
  return mx__kernel->net_status(s);
}
