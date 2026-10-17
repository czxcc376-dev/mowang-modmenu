// Guarded evalString.
//
// libcocos' evalString() dereferences the se::ScriptEngine singleton without a
// null check, so calling it before the engine exists kills the process
// (SIGSEGV at evalString+56, fault addr 0xa4b8).  Layout offsets differ between
// builds, so instead of guessing whether the engine is up we install a SIGSEGV
// handler around the call and recover with siglongjmp.  That makes the very
// first injection attempt safe: if the engine is not ready we simply try again on
// the next frame and never take the game down.
#include "jsguard.h"
#include "log.h"

#include <csetjmp>
#include <csignal>
#include <cstring>
#include <pthread.h>

namespace {

struct Guard {
    sigjmp_buf jb;
    volatile sig_atomic_t active = 0;
    int savedSig[2] = {0, 0};
};

// One guard per thread: the swap hook and the worker both call in.
thread_local Guard t_guard;
thread_local bool t_handlerSet = false;

struct sigaction t_oldSegv;
struct sigaction t_oldBus;

void onFault(int sig, siginfo_t *info, void *ctx) {
    if (t_guard.active) {
        t_guard.active = 0;
        siglongjmp(t_guard.jb, 1);
    }
    // not ours: restore the previous disposition and re-raise
    if (sig == SIGSEGV && (t_oldSegv.sa_flags & SA_SIGINFO)) {
        t_oldSegv.sa_sigaction(sig, info, ctx);
    } else if (sig == SIGSEGV && t_oldSegv.sa_handler != SIG_DFL) {
        t_oldSegv.sa_handler(sig);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void installHandlers() {
    if (t_handlerSet) return;
    t_handlerSet = true;

    struct sigaction sa{};
    sa.sa_sigaction = onFault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &t_oldSegv);
    sigaction(SIGBUS, &sa, &t_oldBus);
}

} // namespace

bool JSGuard_Call(void (*fn)(void *), void *arg) {
    installHandlers();

    if (sigsetjmp(t_guard.jb, 1) != 0) {
        // came back from a fault inside fn()
        LOGE("JS injection faulted; engine not ready yet");
        return false;
    }

    t_guard.active = 1;
    fn(arg);
    t_guard.active = 0;
    return true;
}
