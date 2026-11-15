#include "nc_winapi.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ucontext.h>

#define MAX_HANDLERS 8

static PVECTORED_EXCEPTION_HANDLER g_h[MAX_HANDLERS];
static int g_n;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct sigaction g_old_segv, g_old_bus;
static int g_installed;

_Static_assert(sizeof(((mcontext_t)0)->__fs) >= 8 + 512, "the float state holds an FXSAVE image");

static void to_context(const ucontext_t *uc, CONTEXT *c)
{
    const _STRUCT_X86_THREAD_STATE64 *s = &uc->uc_mcontext->__ss;
    c->Rax = s->__rax;
    c->Rcx = s->__rcx;
    c->Rdx = s->__rdx;
    c->Rbx = s->__rbx;
    c->Rsp = s->__rsp;
    c->Rbp = s->__rbp;
    c->Rsi = s->__rsi;
    c->Rdi = s->__rdi;
    c->R8 = s->__r8;
    c->R9 = s->__r9;
    c->R10 = s->__r10;
    c->R11 = s->__r11;
    c->R12 = s->__r12;
    c->R13 = s->__r13;
    c->R14 = s->__r14;
    c->R15 = s->__r15;
    c->Rip = s->__rip;
    c->EFlags = (DWORD)s->__rflags;
    memcpy(&c->FltSave, &uc->uc_mcontext->__fs.__fpu_fcw, 512);
    c->MxCsr = c->FltSave.MxCsr;
}

static void from_context(const CONTEXT *c, ucontext_t *uc)
{
    _STRUCT_X86_THREAD_STATE64 *s = &uc->uc_mcontext->__ss;
    s->__rax = c->Rax;
    s->__rcx = c->Rcx;
    s->__rdx = c->Rdx;
    s->__rbx = c->Rbx;
    s->__rsp = c->Rsp;
    s->__rbp = c->Rbp;
    s->__rsi = c->Rsi;
    s->__rdi = c->Rdi;
    s->__r8 = c->R8;
    s->__r9 = c->R9;
    s->__r10 = c->R10;
    s->__r11 = c->R11;
    s->__r12 = c->R12;
    s->__r13 = c->R13;
    s->__r14 = c->R14;
    s->__r15 = c->R15;
    s->__rip = c->Rip;
    s->__rflags = c->EFlags;
    memcpy(&uc->uc_mcontext->__fs.__fpu_fcw, &c->FltSave, 512);
}

static void chain(const struct sigaction *old, int sig, siginfo_t *si, void *uc)
{
    if (old->sa_flags & SA_SIGINFO) {
        if (old->sa_sigaction) {
            old->sa_sigaction(sig, si, uc);
            return;
        }
    } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        old->sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void on_signal(int sig, siginfo_t *si, void *ucp)
{
    ucontext_t *uc = (ucontext_t *)ucp;
    CONTEXT c;
    to_context(uc, &c);
    EXCEPTION_RECORD er;
    memset(&er, 0, sizeof(er));
    er.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    er.ExceptionAddress = (PVOID)c.Rip;
    er.NumberParameters = 2;
    er.ExceptionInformation[0] = (uc->uc_mcontext->__es.__err & 2) ? 1 : 0;
    er.ExceptionInformation[1] = (ULONG_PTR)si->si_addr;
    EXCEPTION_POINTERS ep = {&er, &c};
    for (int i = 0; i < g_n; i++) {
        if (g_h[i] && g_h[i](&ep) == EXCEPTION_CONTINUE_EXECUTION) {
            from_context(&c, uc);
            return;
        }
    }
    chain(sig == SIGBUS ? &g_old_bus : &g_old_segv, sig, si, ucp);
}

PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h)
{
    pthread_mutex_lock(&g_mu);
    if (g_n < MAX_HANDLERS) {
        if (first) {
            memmove(&g_h[1], &g_h[0], sizeof(g_h[0]) * (size_t)g_n);
            g_h[0] = h;
        } else {
            g_h[g_n] = h;
        }
        g_n++;
    }
    if (!g_installed) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_signal;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &g_old_segv);
        sigaction(SIGBUS, &sa, &g_old_bus);
        g_installed = 1;
    }
    pthread_mutex_unlock(&g_mu);
    return (PVOID)h;
}

ULONG RemoveVectoredExceptionHandler(PVOID h)
{
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_n; i++) {
        if ((PVOID)g_h[i] == h) {
            memmove(&g_h[i], &g_h[i + 1], sizeof(g_h[0]) * (size_t)(g_n - i - 1));
            g_n--;
            break;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return 1;
}
