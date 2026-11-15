#ifndef NC_MACOS_SYS_PRCTL_H
#define NC_MACOS_SYS_PRCTL_H

#define PR_SET_TIMERSLACK 29
#define PR_SET_NAME 15

static inline int prctl(int option, ...)
{
    (void)option;
    return 0;
}

#endif
