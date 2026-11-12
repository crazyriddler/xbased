#include <sys/eventfd.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int nc_thread_cputime(unsigned port, struct timespec *ts)
{
    thread_basic_info_data_t info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info((thread_act_t)port, THREAD_BASIC_INFO, (thread_info_t)&info, &n) != KERN_SUCCESS) {
        errno = EINVAL;
        return -1;
    }
    const uint64_t us = ((uint64_t)info.user_time.seconds + (uint64_t)info.system_time.seconds) * 1000000u +
                        (uint64_t)info.user_time.microseconds + (uint64_t)info.system_time.microseconds;
    ts->tv_sec = (time_t)(us / 1000000u);
    ts->tv_nsec = (long)(us % 1000000u) * 1000;
    return 0;
}

int nc_page_mapped(const void *p)
{
    mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)p;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &n, &obj) !=
        KERN_SUCCESS) {
        return 0;
    }
    return a <= (mach_vm_address_t)(uintptr_t)p;
}

size_t nc_exe_path(char *out, size_t n)
{
    char raw[PATH_MAX];
    uint32_t len = sizeof(raw);
    if (!n || _NSGetExecutablePath(raw, &len) != 0) {
        if (n) {
            out[0] = 0;
        }
        return 0;
    }
    char full[PATH_MAX];
    const char *p = realpath(raw, full) ? full : raw;
    snprintf(out, n, "%s", p);
    return strlen(out);
}

static bool file_exists(const char *p)
{
    return p && p[0] && access(p, R_OK) == 0;
}

const char *nc_vk_library_path(void)
{
    static char path[PATH_MAX];
    static bool done;
    if (done) {
        return path[0] ? path : NULL;
    }
    done = true;
    setenv("MVK_CONFIG_FAST_MATH_ENABLED", "0", 0);
    setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "1", 0);
    const char *env = getenv("XBASED_VULKAN_LIBRARY");
    if (file_exists(env)) {
        snprintf(path, sizeof(path), "%s", env);
        return path;
    }
    char exe[PATH_MAX];
    if (nc_exe_path(exe, sizeof(exe))) {
        char *slash = strrchr(exe, '/');
        if (slash) {
            slash[1] = 0;
            snprintf(path, sizeof(path), "%s../Frameworks/libMoltenVK.dylib", exe);
            if (file_exists(path)) {
                char full[PATH_MAX];
                if (realpath(path, full)) {
                    snprintf(path, sizeof(path), "%s", full);
                }
                return path;
            }
            snprintf(path, sizeof(path), "%slibMoltenVK.dylib", exe);
            if (file_exists(path)) {
                return path;
            }
        }
    }
    static const char *const k_system[] = {"/usr/local/lib/libvulkan.1.dylib", "/usr/local/lib/libMoltenVK.dylib"};
    for (unsigned i = 0; i < sizeof(k_system) / sizeof(k_system[0]); i++) {
        if (file_exists(k_system[i])) {
            snprintf(path, sizeof(path), "%s", k_system[i]);
            return path;
        }
    }
    path[0] = 0;
    return NULL;
}

extern int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout_us);
extern int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);
#define UL_COMPARE_AND_WAIT 1
#define ULF_WAKE_ALL 0x00000100
#define ULF_NO_ERRNO 0x01000000

void nc_futex_wait(volatile void *addr, uint32_t expected, unsigned ms)
{
    __ulock_wait(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, (void *)addr, expected, ms ? ms * 1000u : 1u);
}

void nc_futex_wake_all(volatile void *addr)
{
    __ulock_wake(UL_COMPARE_AND_WAIT | ULF_WAKE_ALL | ULF_NO_ERRNO, (void *)addr, 0);
}

int eventfd(unsigned int initval, int flags)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof(a);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || getsockname(fd, (struct sockaddr *)&a, &al) != 0 ||
        connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    if (flags & EFD_NONBLOCK) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    if (flags & EFD_CLOEXEC) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    if (initval) {
        const uint64_t v = initval;
        (void)!send(fd, &v, sizeof(v), 0);
    }
    return fd;
}

__attribute__((constructor)) static void nc_macos_init(void)
{
    signal(SIGPIPE, SIG_IGN);
}
