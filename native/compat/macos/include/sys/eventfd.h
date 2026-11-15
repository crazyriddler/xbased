#ifndef NC_MACOS_SYS_EVENTFD_H
#define NC_MACOS_SYS_EVENTFD_H

#include <stdint.h>

#define EFD_NONBLOCK 0x800
#define EFD_CLOEXEC 0x80000

typedef uint64_t eventfd_t;

#ifdef __cplusplus
extern "C" {
#endif
int eventfd(unsigned int initval, int flags);
#ifdef __cplusplus
}
#endif

#endif
