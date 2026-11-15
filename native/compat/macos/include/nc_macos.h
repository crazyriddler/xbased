#ifndef NC_MACOS_H
#define NC_MACOS_H

#ifdef __APPLE__
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int nc_gettid(void)
{
    uint64_t t = 0;
    pthread_threadid_np(NULL, &t);
    return (int)t;
}
#define gettid nc_gettid

#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME CLOCK_MONOTONIC
#endif
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#endif

#define NC_THREAD_CLOCK 0x40000000u
int nc_thread_cputime(unsigned port, struct timespec *ts);
static inline int nc_pthread_getcpuclockid(pthread_t t, clockid_t *id)
{
    *id = (clockid_t)(NC_THREAD_CLOCK | (unsigned)pthread_mach_thread_np(t));
    return 0;
}
#define pthread_getcpuclockid(t, id) nc_pthread_getcpuclockid((t), (id))
static inline int nc_clock_gettime(clockid_t id, struct timespec *ts)
{
    if ((unsigned)id & NC_THREAD_CLOCK) {
        return nc_thread_cputime((unsigned)id & ~NC_THREAD_CLOCK, ts);
    }
    return clock_gettime(id, ts);
}
#define clock_gettime(id, ts) nc_clock_gettime((id), (ts))

#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
static inline int nc_clock_nanosleep(clockid_t id, int flags, const struct timespec *req, struct timespec *rem)
{
    struct timespec rel = *req;
    if (flags & TIMER_ABSTIME) {
        struct timespec now;
        clock_gettime(id, &now);
        const int64_t ns = ((int64_t)req->tv_sec - (int64_t)now.tv_sec) * 1000000000LL +
                           ((int64_t)req->tv_nsec - (int64_t)now.tv_nsec);
        if (ns <= 0) {
            return 0;
        }
        rel.tv_sec = (time_t)(ns / 1000000000LL);
        rel.tv_nsec = (long)(ns % 1000000000LL);
        rem = NULL;
    }
    return nanosleep(&rel, rem) == 0 ? 0 : errno;
}
#define clock_nanosleep(c, f, r, m) nc_clock_nanosleep((c), (f), (r), (m))
#endif

#ifndef st_mtim
#define st_mtim st_mtimespec
#define st_atim st_atimespec
#define st_ctim st_ctimespec
#endif

#define pthread_condattr_setclock(a, c) ((void)(a), (void)(c), 0)
static inline int nc_cond_timedwait_mono(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *deadline)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t ns = ((int64_t)deadline->tv_sec - (int64_t)now.tv_sec) * 1000000000LL +
                 ((int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec);
    if (ns < 0) {
        ns = 0;
    }
    const struct timespec rel = {(time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL)};
    return pthread_cond_timedwait_relative_np(c, m, &rel);
}
#define pthread_cond_timedwait(c, m, t) nc_cond_timedwait_mono((c), (m), (t))

static inline int nc_pthread_setname(pthread_t t, const char *name)
{
    if (!pthread_equal(t, pthread_self())) {
        return 0;
    }
    char n[64];
    size_t i = 0;
    for (; name[i] && i < sizeof(n) - 1; i++) {
        n[i] = name[i];
    }
    n[i] = 0;
    return pthread_setname_np(n);
}
#define pthread_setname_np(t, n) nc_pthread_setname((t), (n))

#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x20000000
#define SOCK_CLOEXEC 0x10000000
static inline int nc_socket(int domain, int type, int proto)
{
    const int fd = socket(domain, type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC), proto);
    if (fd >= 0 && (type & SOCK_NONBLOCK)) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    if (fd >= 0 && (type & SOCK_CLOEXEC)) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    return fd;
}
#define socket(d, t, p) nc_socket((d), (t), (p))
#endif

const char *nc_vk_library_path(void);
int nc_page_mapped(const void *p);
size_t nc_exe_path(char *out, size_t n);
void nc_futex_wait(volatile void *addr, uint32_t expected, unsigned ms);
void nc_futex_wake_all(volatile void *addr);

#ifdef __cplusplus
}
#endif
#endif

#endif
