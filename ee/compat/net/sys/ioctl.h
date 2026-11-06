#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#define FIONBIO 0x8004667e
#define FIONREAD 0x4004667f

#ifdef __cplusplus
extern "C"
#endif
int ioctl(int fd, unsigned long request, ...);

#endif
