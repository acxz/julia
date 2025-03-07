// This file is a part of Julia. License is MIT: https://julialang.org/license

// Note that this file is `#include`d by "signal-handling.c"

#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#if defined(_OS_DARWIN_) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
#endif

#ifdef __APPLE__
#include <AvailabilityMacros.h>
#ifdef MAC_OS_X_VERSION_10_9
#include <sys/_types/_ucontext64.h>
#else
#define __need_ucontext64_t
#include <machine/_structs.h>
#endif
#endif

// Figure out the best signals/timers to use for this platform
#ifdef __APPLE__ // Darwin's mach ports allow signal-free thread management
#define HAVE_MACH
#define HAVE_KEVENT
#elif defined(__FreeBSD__) // generic bsd
#define HAVE_ITIMER
#else // generic linux
#define HAVE_TIMER
#endif

#ifdef HAVE_KEVENT
#include <sys/event.h>
#endif

// 8M signal stack, same as default stack size and enough
// for reasonable finalizers.
// Should also be enough for parallel GC when we have it =)
#define sig_stack_size (8 * 1024 * 1024)

#include "julia_assert.h"

// helper function for returning the unw_context_t inside a ucontext_t
// (also used by stackwalk.c)
bt_context_t *jl_to_bt_context(void *sigctx)
{
#ifdef __APPLE__
    return (bt_context_t*)&((ucontext64_t*)sigctx)->uc_mcontext64->__ss;
#elif defined(_CPU_ARM_)
    // libunwind does not use `ucontext_t` on ARM.
    // `unw_context_t` is a struct of 16 `unsigned long` which should
    // have the same layout as the `arm_r0` to `arm_pc` fields in `sigcontext`
    ucontext_t *ctx = (ucontext_t*)sigctx;
    return (bt_context_t*)&ctx->uc_mcontext.arm_r0;
#else
    return (bt_context_t*)sigctx;
#endif
}

static int thread0_exit_count = 0;
static void jl_exit_thread0(int exitstate, jl_bt_element_t *bt_data, size_t bt_size);

static inline __attribute__((unused)) uintptr_t jl_get_rsp_from_ctx(const void *_ctx)
{
#if defined(_OS_LINUX_) && defined(_CPU_X86_64_)
    const ucontext_t *ctx = (const ucontext_t*)_ctx;
    return ctx->uc_mcontext.gregs[REG_RSP];
#elif defined(_OS_LINUX_) && defined(_CPU_X86_)
    const ucontext_t *ctx = (const ucontext_t*)_ctx;
    return ctx->uc_mcontext.gregs[REG_ESP];
#elif defined(_OS_LINUX_) && defined(_CPU_AARCH64_)
    const ucontext_t *ctx = (const ucontext_t*)_ctx;
    return ctx->uc_mcontext.sp;
#elif defined(_OS_LINUX_) && defined(_CPU_ARM_)
    const ucontext_t *ctx = (const ucontext_t*)_ctx;
    return ctx->uc_mcontext.arm_sp;
#elif defined(_OS_DARWIN_) && defined(_CPU_X86_64_)
    const ucontext64_t *ctx = (const ucontext64_t*)_ctx;
    return ctx->uc_mcontext64->__ss.__rsp;
#elif defined(_OS_DARWIN_) && defined(_CPU_AARCH64_)
    const ucontext64_t *ctx = (const ucontext64_t*)_ctx;
    return ctx->uc_mcontext64->__ss.__sp;
#elif defined(_OS_FREEBSD_) && defined(_CPU_X86_64_)
    const ucontext_t *ctx = (const ucontext_t*)_ctx;
    return ctx->uc_mcontext.mc_rsp;
#else
    // TODO Add support for PowerPC(64)?
    return 0;
#endif
}

static int is_addr_on_sigstack(jl_ptls_t ptls, void *ptr)
{
    // One guard page for signal_stack.
    return !((char*)ptr < (char*)ptls->signal_stack - jl_page_size ||
             (char*)ptr > (char*)ptls->signal_stack + sig_stack_size);
}

// Modify signal context `_ctx` so that `fptr` will execute when the signal
// returns. `fptr` will execute on the signal stack, and must not return.
// jl_call_in_ctx is also currently executing on that signal stack,
// so be careful not to smash it
static void jl_call_in_ctx(jl_ptls_t ptls, void (*fptr)(void), int sig, void *_ctx)
{
    // Modifying the ucontext should work but there is concern that
    // sigreturn oriented programming mitigation can work against us
    // by rejecting ucontext that is modified.
    // The current (staged) implementation in the Linux Kernel only
    // checks that the syscall is made in the signal handler and that
    // the ucontext address is valid. Hopefully the value of the ucontext
    // will not be part of the validation...
    if (!ptls || !ptls->signal_stack) {
        sigset_t sset;
        sigemptyset(&sset);
        sigaddset(&sset, sig);
        sigprocmask(SIG_UNBLOCK, &sset, NULL);
        fptr();
        return;
    }
    uintptr_t rsp = jl_get_rsp_from_ctx(_ctx);
    if (is_addr_on_sigstack(ptls, (void*)rsp)) {
        rsp = (rsp - 256) & ~(uintptr_t)15; // redzone and re-alignment
    }
    else {
        rsp = (uintptr_t)ptls->signal_stack + sig_stack_size;
    }
    assert(rsp % 16 == 0);
#if defined(_OS_LINUX_) && defined(_CPU_X86_64_)
    ucontext_t *ctx = (ucontext_t*)_ctx;
    rsp -= sizeof(void*);
    ctx->uc_mcontext.gregs[REG_RSP] = rsp;
    ctx->uc_mcontext.gregs[REG_RIP] = (uintptr_t)fptr;
#elif defined(_OS_FREEBSD_) && defined(_CPU_X86_64_)
    ucontext_t *ctx = (ucontext_t*)_ctx;
    rsp -= sizeof(void*);
    ctx->uc_mcontext.mc_rsp = rsp;
    ctx->uc_mcontext.mc_rip = (uintptr_t)fptr;
#elif defined(_OS_LINUX_) && defined(_CPU_X86_)
    ucontext_t *ctx = (ucontext_t*)_ctx;
    rsp -= sizeof(void*);
    ctx->uc_mcontext.gregs[REG_ESP] = rsp;
    ctx->uc_mcontext.gregs[REG_EIP] = (uintptr_t)fptr;
#elif defined(_OS_FREEBSD_) && defined(_CPU_X86_)
    ucontext_t *ctx = (ucontext_t*)_ctx;
    rsp -= sizeof(void*);
    ctx->uc_mcontext.mc_esp = rsp;
    ctx->uc_mcontext.mc_eip = (uintptr_t)fptr;
#elif defined(_OS_LINUX_) && defined(_CPU_AARCH64_)
    ucontext_t *ctx = (ucontext_t*)_ctx;
    ctx->uc_mcontext.sp = rsp;
    ctx->uc_mcontext.regs[29] = 0; // Clear link register (x29)
    ctx->uc_mcontext.pc = (uintptr_t)fptr;
#elif defined(_OS_LINUX_) && defined(_CPU_ARM_)
    ucontext_t *ctx = (ucontext_t*)_ctx;
    uintptr_t target = (uintptr_t)fptr;
    // Apparently some glibc's sigreturn target is running in thumb state.
    // Mimic a `bx` instruction by setting the T(5) bit of CPSR
    // depending on the target address.
    uintptr_t cpsr = ctx->uc_mcontext.arm_cpsr;
    // Thumb mode function pointer should have the lowest bit set
    if (target & 1) {
        target = target & ~((uintptr_t)1);
        cpsr = cpsr | (1 << 5);
    }
    else {
        cpsr = cpsr & ~(1 << 5);
    }
    ctx->uc_mcontext.arm_cpsr = cpsr;
    ctx->uc_mcontext.arm_sp = rsp;
    ctx->uc_mcontext.arm_lr = 0; // Clear link register
    ctx->uc_mcontext.arm_pc = target;
#elif defined(_OS_DARWIN_) && (defined(_CPU_X86_64_) || defined(_CPU_AARCH64_))
    // Only used for SIGFPE.
    // This doesn't seems to be reliable when the SIGFPE is generated
    // from a divide-by-zero exception, which is now handled by
    // `catch_exception_raise`. It works fine when a signal is received
    // due to `kill`/`raise` though.
    ucontext64_t *ctx = (ucontext64_t*)_ctx;
#if defined(_CPU_X86_64_)
    rsp -= sizeof(void*);
    ctx->uc_mcontext64->__ss.__rsp = rsp;
    ctx->uc_mcontext64->__ss.__rip = (uintptr_t)fptr;
#else
    ctx->uc_mcontext64->__ss.__sp = rsp;
    ctx->uc_mcontext64->__ss.__pc = (uintptr_t)fptr;
    ctx->uc_mcontext64->__ss.__lr = 0;
#endif
#else
#pragma message("julia: throw-in-context not supported on this platform")
    // TODO Add support for PowerPC(64)?
    sigset_t sset;
    sigemptyset(&sset);
    sigaddset(&sset, sig);
    sigprocmask(SIG_UNBLOCK, &sset, NULL);
    fptr();
#endif
}

static void jl_throw_in_ctx(jl_task_t *ct, jl_value_t *e, int sig, void *sigctx)
{
    jl_ptls_t ptls = ct->ptls;
    if (!jl_get_safe_restore()) {
        ptls->bt_size =
            rec_backtrace_ctx(ptls->bt_data, JL_MAX_BT_SIZE, jl_to_bt_context(sigctx),
                              ct->gcstack);
        ptls->sig_exception = e;
    }
    jl_call_in_ctx(ptls, &jl_sig_throw, sig, sigctx);
}

static pthread_t signals_thread;

static int is_addr_on_stack(jl_task_t *ct, void *addr)
{
    if (ct->copy_stack) {
        jl_ptls_t ptls = ct->ptls;
        return ((char*)addr > (char*)ptls->stackbase - ptls->stacksize &&
                (char*)addr < (char*)ptls->stackbase);
    }
    return ((char*)addr > (char*)ct->stkbuf &&
            (char*)addr < (char*)ct->stkbuf + ct->bufsz);
}

static void sigdie_handler(int sig, siginfo_t *info, void *context)
{
    signal(sig, SIG_DFL);
    uv_tty_reset_mode();
    if (sig == SIGILL)
        jl_show_sigill(context);
    jl_critical_error(sig, jl_to_bt_context(context), jl_get_current_task());
    if (sig != SIGSEGV &&
        sig != SIGBUS &&
        sig != SIGILL) {
        raise(sig);
    }
    // fall-through return to re-execute faulting statement (but without the error handler)
}

#if defined(_CPU_X86_64_) || defined(_CPU_X86_)
enum x86_trap_flags {
    USER_MODE = 0x4,
    WRITE_FAULT = 0x2,
    PAGE_PRESENT = 0x1
};

int exc_reg_is_write_fault(uintptr_t err) {
    return err & WRITE_FAULT;
}
#elif defined(_CPU_AARCH64_)
enum aarch64_esr_layout {
    EC_MASK = ((uint32_t)0b111111) << 26,
    EC_DATA_ABORT = ((uint32_t)0b100100) << 26,
    ISR_DA_WnR = ((uint32_t)1) << 6
};

int exc_reg_is_write_fault(uintptr_t esr) {
    return (esr & EC_MASK) == EC_DATA_ABORT && (esr & ISR_DA_WnR);
}
#endif

#if defined(HAVE_MACH)
#include "signals-mach.c"
#else


#if defined(_OS_LINUX_) && (defined(_CPU_X86_64_) || defined(_CPU_X86_))
int is_write_fault(void *context) {
    ucontext_t *ctx = (ucontext_t*)context;
    return exc_reg_is_write_fault(ctx->uc_mcontext.gregs[REG_ERR]);
}
#elif defined(_OS_LINUX_) && defined(_CPU_AARCH64_)
struct linux_aarch64_ctx_header {
	uint32_t magic;
	uint32_t size;
};
const uint32_t linux_esr_magic = 0x45535201;

int is_write_fault(void *context) {
    ucontext_t *ctx = (ucontext_t*)context;
    struct linux_aarch64_ctx_header *extra =
        (struct linux_aarch64_ctx_header *)ctx->uc_mcontext.__reserved;
    while (extra->magic != 0) {
        if (extra->magic == linux_esr_magic) {
            return exc_reg_is_write_fault(*(uint64_t*)&extra[1]);
        }
        extra = (struct linux_aarch64_ctx_header *)
            (((uint8_t*)extra) + extra->size);
    }
    return 0;
}
#elif defined(_OS_FREEBSD_) && (defined(_CPU_X86_64_) || defined(_CPU_X86_))
int is_write_fault(void *context) {
    ucontext_t *ctx = (ucontext_t*)context;
    return exc_reg_is_write_fault(ctx->uc_mcontext.mc_err);
}
#else
#pragma message("Implement this query for consistent PROT_NONE handling")
int is_write_fault(void *context) {
    return 0;
}
#endif

static int jl_is_on_sigstack(jl_ptls_t ptls, void *ptr, void *context)
{
    return (is_addr_on_sigstack(ptls, ptr) &&
            is_addr_on_sigstack(ptls, (void*)jl_get_rsp_from_ctx(context)));
}

static void segv_handler(int sig, siginfo_t *info, void *context)
{
    if (jl_get_safe_restore()) { // restarting jl_ or profile
        jl_call_in_ctx(NULL, &jl_sig_throw, sig, context);
        return;
    }
    jl_task_t *ct = jl_get_current_task();
    if (ct == NULL) {
        sigdie_handler(sig, info, context);
        return;
    }
    assert(sig == SIGSEGV || sig == SIGBUS);
    if (jl_addr_is_safepoint((uintptr_t)info->si_addr)) {
        jl_set_gc_and_wait();
        // Do not raise sigint on worker thread
        if (jl_atomic_load_relaxed(&ct->tid) != 0)
            return;
        if (ct->ptls->defer_signal) {
            jl_safepoint_defer_sigint();
        }
        else if (jl_safepoint_consume_sigint()) {
            jl_clear_force_sigint();
            jl_throw_in_ctx(ct, jl_interrupt_exception, sig, context);
        }
        return;
    }
    if (is_addr_on_stack(ct, info->si_addr)) { // stack overflow
        jl_throw_in_ctx(ct, jl_stackovf_exception, sig, context);
    }
    else if (jl_is_on_sigstack(ct->ptls, info->si_addr, context)) {
        // This mainly happens when one of the finalizers during final cleanup
        // on the signal stack has a deep/infinite recursion.
        // There isn't anything more we can do
        // (we are already corrupting that stack running this function)
        // so just call `_exit` to terminate immediately.
        jl_safe_printf("ERROR: Signal stack overflow, exit\n");
        _exit(sig + 128);
    }
    else if (sig == SIGSEGV && info->si_code == SEGV_ACCERR && is_write_fault(context)) {  // writing to read-only memory (e.g., mmap)
        jl_throw_in_ctx(ct, jl_readonlymemory_exception, sig, context);
    }
    else {
#ifdef SEGV_EXCEPTION
        jl_throw_in_ctx(ct, jl_segv_exception, sig, context);
#else
        sigdie_handler(sig, info, context);
#endif
    }
}

#if !defined(JL_DISABLE_LIBUNWIND)
static unw_context_t *signal_context;
pthread_mutex_t in_signal_lock;
static pthread_cond_t exit_signal_cond;
static pthread_cond_t signal_caught_cond;

static void jl_thread_suspend_and_get_state(int tid, unw_context_t **ctx)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    pthread_mutex_lock(&in_signal_lock);
    jl_ptls_t ptls2 = jl_all_tls_states[tid];
    jl_atomic_store_release(&ptls2->signal_request, 1);
    pthread_kill(ptls2->system_id, SIGUSR2);
    // wait for thread to acknowledge
    int err = pthread_cond_timedwait(&signal_caught_cond, &in_signal_lock, &ts);
    if (err == ETIMEDOUT) {
        sig_atomic_t request = 1;
        if (jl_atomic_cmpswap(&ptls2->signal_request, &request, 0)) {
            *ctx = NULL;
            pthread_mutex_unlock(&in_signal_lock);
            return;
        }
        // Request is either now 0 (meaning the other thread is waiting for
        //   exit_signal_cond already),
        // Or it is now -1 (meaning the other thread
        //   is waiting for in_signal_lock, and we need to release that lock
        //   here for a bit, until the other thread has a chance to get to the
        //   exit_signal_cond)
        if (request == -1) {
            err = pthread_cond_wait(&signal_caught_cond, &in_signal_lock);
            assert(!err);
        }
    }
    // Now the other thread is waiting on exit_signal_cond (verify that here by
    // checking it is 0, and add an acquire barrier for good measure)
    int request = jl_atomic_load_acquire(&ptls2->signal_request);
    assert(request == 0); (void) request;
    *ctx = signal_context;
}

static void jl_thread_resume(int tid, int sig)
{
    jl_ptls_t ptls2 = jl_all_tls_states[tid];
    jl_atomic_store_release(&ptls2->signal_request, sig == -1 ? 3 : 1);
    pthread_cond_broadcast(&exit_signal_cond);
    pthread_cond_wait(&signal_caught_cond, &in_signal_lock); // wait for thread to acknowledge
    // The other thread is waiting to leave exit_signal_cond (verify that here by
    // checking it is 0, and add an acquire barrier for good measure)
    int request = jl_atomic_load_acquire(&ptls2->signal_request);
    assert(request == 0); (void) request;
    pthread_mutex_unlock(&in_signal_lock);
}
#endif

// Throw jl_interrupt_exception if the master thread is in a signal async region
// or if SIGINT happens too often.
static void jl_try_deliver_sigint(void)
{
    jl_ptls_t ptls2 = jl_all_tls_states[0];
    jl_safepoint_enable_sigint();
    jl_wake_libuv();
    jl_atomic_store_release(&ptls2->signal_request, 2);
    // This also makes sure `sleep` is aborted.
    pthread_kill(ptls2->system_id, SIGUSR2);
}

// Write only by signal handling thread, read only by main thread
// no sync necessary.
static int thread0_exit_state = 0;
static void JL_NORETURN jl_exit_thread0_cb(void)
{
CFI_NORETURN
    // This can get stuck if it happens at an unfortunate spot
    // (unavoidable due to its async nature).
    // Try harder to exit each time if we get multiple exit requests.
    if (thread0_exit_count <= 1) {
        jl_critical_error(thread0_exit_state - 128, NULL, jl_current_task);
        jl_exit(thread0_exit_state);
    }
    else if (thread0_exit_count == 2) {
        exit(thread0_exit_state);
    }
    else {
        _exit(thread0_exit_state);
    }
}

static void jl_exit_thread0(int state, jl_bt_element_t *bt_data, size_t bt_size)
{
    jl_ptls_t ptls2 = jl_all_tls_states[0];
    if (thread0_exit_count <= 1) {
        unw_context_t *signal_context;
        jl_thread_suspend_and_get_state(0, &signal_context);
        if (signal_context != NULL) {
            thread0_exit_state = state;
            ptls2->bt_size = bt_size; // <= JL_MAX_BT_SIZE
            memcpy(ptls2->bt_data, bt_data, ptls2->bt_size * sizeof(bt_data[0]));
            jl_thread_resume(0, -1);
            return;
        }
    }
    thread0_exit_state = state;
    jl_atomic_store_release(&ptls2->signal_request, 3);
    // This also makes sure `sleep` is aborted.
    pthread_kill(ptls2->system_id, SIGUSR2);
}

// request:
// -1: beginning processing [invalid outside here]
//  0: nothing [not from here]
//  1: get state
//  2: throw sigint if `!defer_signal && io_wait` or if force throw threshold
//     is reached
//  3: exit with `thread0_exit_state`
void usr2_handler(int sig, siginfo_t *info, void *ctx)
{
    jl_task_t *ct = jl_get_current_task();
    if (ct == NULL)
        return;
    jl_ptls_t ptls = ct->ptls;
    if (ptls == NULL)
        return;
    int errno_save = errno;
    // acknowledge that we saw the signal_request
    sig_atomic_t request = jl_atomic_exchange(&ptls->signal_request, -1);
#if !defined(JL_DISABLE_LIBUNWIND)
    if (request == 1) {
        pthread_mutex_lock(&in_signal_lock);
        signal_context = jl_to_bt_context(ctx);
        // acknowledge that we set the signal_caught_cond broadcast
        request = jl_atomic_exchange(&ptls->signal_request, 0);
        assert(request == -1); (void) request;
        pthread_cond_broadcast(&signal_caught_cond);
        pthread_cond_wait(&exit_signal_cond, &in_signal_lock);
        request = jl_atomic_exchange(&ptls->signal_request, 0);
        assert(request == 1 || request == 3);
        // acknowledge that we got the resume signal
        pthread_cond_broadcast(&signal_caught_cond);
        pthread_mutex_unlock(&in_signal_lock);
    }
    else
#endif
    jl_atomic_exchange(&ptls->signal_request, 0); // returns -1
    if (request == 2) {
        int force = jl_check_force_sigint();
        if (force || (!ptls->defer_signal && ptls->io_wait)) {
            jl_safepoint_consume_sigint();
            if (force)
                jl_safe_printf("WARNING: Force throwing a SIGINT\n");
            // Force a throw
            jl_clear_force_sigint();
            jl_throw_in_ctx(ct, jl_interrupt_exception, sig, ctx);
        }
    }
    else if (request == 3) {
        jl_call_in_ctx(ct->ptls, jl_exit_thread0_cb, sig, ctx);
    }
    errno = errno_save;
}

// Because SIGUSR1 is dual-purpose, and the timer can have trailing signals after being deleted,
// a 2-second grace period is imposed to ignore any trailing timer-created signals so they don't get
// confused for user triggers
uint64_t last_timer_delete_time = 0;

int timer_graceperiod_elapsed(void)
{
    return jl_hrtime() > (last_timer_delete_time + 2e9);
}

#if defined(HAVE_TIMER)
// Linux-style
#include <time.h>
#include <string.h>  // for memset

static timer_t timerprof;
static struct itimerspec itsprof;

JL_DLLEXPORT int jl_profile_start_timer(void)
{
    struct sigevent sigprof;

    // Establish the signal event
    memset(&sigprof, 0, sizeof(struct sigevent));
    sigprof.sigev_notify = SIGEV_SIGNAL;
    sigprof.sigev_signo = SIGUSR1;
    sigprof.sigev_value.sival_ptr = &timerprof;
    // Because SIGUSR1 is multipurpose, set `running` before so that we know that the first SIGUSR1 came from the timer
    running = 1;
    if (timer_create(CLOCK_REALTIME, &sigprof, &timerprof) == -1) {
        running = 0;
        return -2;
    }

    // Start the timer
    itsprof.it_interval.tv_sec = 0;
    itsprof.it_interval.tv_nsec = 0;
    itsprof.it_value.tv_sec = nsecprof / GIGA;
    itsprof.it_value.tv_nsec = nsecprof % GIGA;
    if (timer_settime(timerprof, 0, &itsprof, NULL) == -1) {
        running = 0;
        return -3;
    }
    return 0;
}

JL_DLLEXPORT void jl_profile_stop_timer(void)
{
    if (running) {
        timer_delete(timerprof);
        last_timer_delete_time = jl_hrtime();
        running = 0;
    }
}

#elif defined(HAVE_ITIMER)
// BSD-style timers
#include <string.h>
#include <sys/time.h>
struct itimerval timerprof;

JL_DLLEXPORT int jl_profile_start_timer(void)
{
    timerprof.it_interval.tv_sec = 0;
    timerprof.it_interval.tv_usec = 0;
    timerprof.it_value.tv_sec = nsecprof / GIGA;
    timerprof.it_value.tv_usec = ((nsecprof % GIGA) + 999) / 1000;
    // Because SIGUSR1 is multipurpose, set `running` before so that we know that the first SIGUSR1 came from the timer
    running = 1;
    if (setitimer(ITIMER_PROF, &timerprof, NULL) == -1) {
        running = 0;
        return -3;
    }
    return 0;
}

JL_DLLEXPORT void jl_profile_stop_timer(void)
{
    if (running) {
        memset(&timerprof, 0, sizeof(timerprof));
        setitimer(ITIMER_PROF, &timerprof, NULL);
        last_timer_delete_time = jl_hrtime();
        running = 0;
    }
}

#else

#error no profile tools available

#endif
#endif // HAVE_MACH

static void allocate_segv_handler(void)
{
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = segv_handler;
    act.sa_flags = SA_ONSTACK | SA_SIGINFO;
    if (sigaction(SIGSEGV, &act, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
    // On AArch64, stack overflow triggers a SIGBUS
    if (sigaction(SIGBUS, &act, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
}

static void *alloc_sigstack(size_t *ssize)
{
    void *stk = jl_malloc_stack(ssize, NULL);
    if (stk == MAP_FAILED)
        jl_errorf("fatal error allocating signal stack: mmap: %s", strerror(errno));
    return stk;
}

void jl_install_thread_signal_handler(jl_ptls_t ptls)
{
    size_t ssize = sig_stack_size;
    void *signal_stack = alloc_sigstack(&ssize);
    ptls->signal_stack = signal_stack;
    stack_t ss;
    ss.ss_flags = 0;
    ss.ss_size = ssize - 16;
    ss.ss_sp = signal_stack;
    if (sigaltstack(&ss, NULL) < 0) {
        jl_errorf("fatal error: sigaltstack: %s", strerror(errno));
    }

#ifdef HAVE_MACH
    attach_exception_port(pthread_mach_thread_np(ptls->system_id), 0);
#endif
}

static void jl_sigsetset(sigset_t *sset)
{
    sigemptyset(sset);
    sigaddset(sset, SIGINT);
    sigaddset(sset, SIGTERM);
    sigaddset(sset, SIGABRT);
    sigaddset(sset, SIGQUIT);
#ifdef SIGINFO
    sigaddset(sset, SIGINFO);
#else
    sigaddset(sset, SIGUSR1);
#endif
#if defined(HAVE_TIMER)
    sigaddset(sset, SIGUSR1);
#elif defined(HAVE_ITIMER)
    sigaddset(sset, SIGPROF);
#endif
}

#ifdef HAVE_KEVENT
static void kqueue_signal(int *sigqueue, struct kevent *ev, int sig)
{
    if (*sigqueue == -1)
        return;
    EV_SET(ev, sig, EVFILT_SIGNAL, EV_ADD, 0, 0, 0);
    if (kevent(*sigqueue, ev, 1, NULL, 0, NULL)) {
        perror("signal kevent");
        close(*sigqueue);
        *sigqueue = -1;
    }
    else {
        signal(sig, SIG_IGN);
    }
}
#endif

void trigger_profile_peek(void)
{
    jl_safe_printf("\n======================================================================================\n");
    jl_safe_printf("Information request received. A stacktrace will print followed by a %.1f second profile\n", profile_peek_duration);
    jl_safe_printf("======================================================================================\n");
    if (bt_size_max == 0){
        // If the buffer hasn't been initialized, initialize with default size
        // Keep these values synchronized with Profile.default_init()
        if (jl_profile_init(10000000 * jl_n_threads, 1000000) == -1){
            jl_safe_printf("ERROR: could not initialize the profile buffer");
            return;
        }
    }
    bt_size_cur = 0; // clear profile buffer
    if (jl_profile_start_timer() < 0)
        jl_safe_printf("ERROR: Could not start profile timer\n");
    else
        profile_autostop_time = jl_hrtime() + (profile_peek_duration * 1e9);
}

static void *signal_listener(void *arg)
{
    static jl_bt_element_t bt_data[JL_MAX_BT_SIZE + 1];
    static size_t bt_size = 0;
    sigset_t sset;
    int sig, critical, profile;
    jl_sigsetset(&sset);
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 199309L
    siginfo_t info;
#endif
#ifdef HAVE_KEVENT
    struct kevent ev;
    int sigqueue = kqueue();
    if (sigqueue == -1) {
        perror("signal kqueue");
    }
    else {
        kqueue_signal(&sigqueue, &ev, SIGINT);
        kqueue_signal(&sigqueue, &ev, SIGTERM);
        kqueue_signal(&sigqueue, &ev, SIGABRT);
        kqueue_signal(&sigqueue, &ev, SIGQUIT);
#ifdef SIGINFO
        kqueue_signal(&sigqueue, &ev, SIGINFO);
#else
        kqueue_signal(&sigqueue, &ev, SIGUSR1);
#endif
#if defined(HAVE_TIMER)
        kqueue_signal(&sigqueue, &ev, SIGUSR1);
#elif defined(HAVE_ITIMER)
        kqueue_signal(&sigqueue, &ev, SIGPROF);
#endif
    }
#endif
    while (1) {
        sig = 0;
        errno = 0;
#ifdef HAVE_KEVENT
        if (sigqueue != -1) {
            int nevents = kevent(sigqueue, NULL, 0, &ev, 1, NULL);
            if (nevents == -1) {
                if (errno == EINTR)
                    continue;
                perror("signal kevent");
            }
            if (nevents != 1) {
                close(sigqueue);
                sigqueue = -1;
                continue;
            }
            sig = ev.ident;
        }
        else
#endif
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 199309L
        sig = sigwaitinfo(&sset, &info);
#else
        if (sigwait(&sset, &sig))
            sig = -1;
#endif
        if (sig == -1) {
            if (errno == EINTR)
                continue;
            sig = SIGABRT; // this branch can't occur, unless we had stack memory corruption of sset
        }
        profile = 0;
#ifndef HAVE_MACH
#if defined(HAVE_TIMER)
        profile = (sig == SIGUSR1);
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 199309L
        if (profile && !(info.si_code == SI_TIMER &&
	            info.si_value.sival_ptr == &timerprof))
            profile = 0;
#endif
#elif defined(HAVE_ITIMER)
        profile = (sig == SIGPROF);
#endif
#endif

        if (sig == SIGINT) {
            if (jl_ignore_sigint()) {
                continue;
            }
            else if (exit_on_sigint) {
                critical = 1;
            }
            else {
                jl_try_deliver_sigint();
                continue;
            }
        }
        else {
            critical = 0;
        }

        critical |= (sig == SIGTERM);
        critical |= (sig == SIGABRT);
        critical |= (sig == SIGQUIT);
#ifdef SIGINFO
        critical |= (sig == SIGINFO);
#else
        critical |= (sig == SIGUSR1 && !profile);
#endif

        int doexit = critical;
#ifdef SIGINFO
        if (sig == SIGINFO) {
            if (running != 1)
                trigger_profile_peek();
            doexit = 0;
        }
#else
        if (sig == SIGUSR1) {
            if (running != 1 && timer_graceperiod_elapsed())
                trigger_profile_peek();
            doexit = 0;
        }
#endif

        bt_size = 0;
#if !defined(JL_DISABLE_LIBUNWIND)
        unw_context_t *signal_context;
        // sample each thread, round-robin style in reverse order
        // (so that thread zero gets notified last)
        if (critical || profile) {
            jl_lock_profile();
            int *randperm;
            if (profile)
                 randperm = profile_get_randperm(jl_n_threads);
            for (int idx = jl_n_threads; idx-- > 0; ) {
                // Stop the threads in the random or reverse round-robin order.
                int i = profile ? randperm[idx] : idx;
                // notify thread to stop
                jl_thread_suspend_and_get_state(i, &signal_context);
                if (signal_context == NULL)
                    continue;

                // do backtrace on thread contexts for critical signals
                // this part must be signal-handler safe
                if (critical) {
                    bt_size += rec_backtrace_ctx(bt_data + bt_size,
                            JL_MAX_BT_SIZE / jl_n_threads - 1,
                            signal_context, NULL);
                    bt_data[bt_size++].uintptr = 0;
                }

                // do backtrace for profiler
                if (profile && running) {
                    if (jl_profile_is_buffer_full()) {
                        // Buffer full: Delete the timer
                        jl_profile_stop_timer();
                    }
                    else {
                        // unwinding can fail, so keep track of the current state
                        // and restore from the SEGV handler if anything happens.
                        jl_jmp_buf *old_buf = jl_get_safe_restore();
                        jl_jmp_buf buf;

                        jl_set_safe_restore(&buf);
                        if (jl_setjmp(buf, 0)) {
                            jl_safe_printf("WARNING: profiler attempt to access an invalid memory location\n");
                        } else {
                            // Get backtrace data
                            bt_size_cur += rec_backtrace_ctx((jl_bt_element_t*)bt_data_prof + bt_size_cur,
                                    bt_size_max - bt_size_cur - 1, signal_context, NULL);
                        }
                        jl_set_safe_restore(old_buf);

                        jl_ptls_t ptls2 = jl_all_tls_states[i];

                        // store threadid but add 1 as 0 is preserved to indicate end of block
                        bt_data_prof[bt_size_cur++].uintptr = ptls2->tid + 1;

                        // store task id
                        bt_data_prof[bt_size_cur++].jlvalue = (jl_value_t*)jl_atomic_load_relaxed(&ptls2->current_task);

                        // store cpu cycle clock
                        bt_data_prof[bt_size_cur++].uintptr = cycleclock();

                        // store whether thread is sleeping but add 1 as 0 is preserved to indicate end of block
                        bt_data_prof[bt_size_cur++].uintptr = jl_atomic_load_relaxed(&ptls2->sleep_check_state) + 1;

                        // Mark the end of this block with two 0's
                        bt_data_prof[bt_size_cur++].uintptr = 0;
                        bt_data_prof[bt_size_cur++].uintptr = 0;
                    }
                }

                // notify thread to resume
                jl_thread_resume(i, sig);
            }
            jl_unlock_profile();
        }
#ifndef HAVE_MACH
        if (profile && running) {
            jl_check_profile_autostop();
#if defined(HAVE_TIMER)
            timer_settime(timerprof, 0, &itsprof, NULL);
#elif defined(HAVE_ITIMER)
            setitimer(ITIMER_PROF, &timerprof, NULL);
#endif
        }
#endif
#endif

        // this part is async with the running of the rest of the program
        // and must be thread-safe, but not necessarily signal-handler safe
        if (critical) {
            if (doexit) {
                thread0_exit_count++;
                jl_exit_thread0(128 + sig, bt_data, bt_size);
            }
            else {
#ifndef SIGINFO // SIGINFO already prints this automatically
                int nrunning = 0;
                for (int idx = jl_n_threads; idx-- > 0; ) {
                    jl_ptls_t ptls2 = jl_all_tls_states[idx];
                    nrunning += !jl_atomic_load_relaxed(&ptls2->sleep_check_state);
                }
                jl_safe_printf("\ncmd: %s %d running %d of %d\n", jl_options.julia_bin ? jl_options.julia_bin : "julia", uv_os_getpid(), nrunning, jl_n_threads);
#endif

                jl_safe_printf("\nsignal (%d): %s\n", sig, strsignal(sig));
                size_t i;
                for (i = 0; i < bt_size; i += jl_bt_entry_size(bt_data + i)) {
                    jl_print_bt_entry_codeloc(bt_data + i);
                }
            }
        }
    }
    return NULL;
}

void restore_signals(void)
{
    sigemptyset(&jl_sigint_sset);
    sigaddset(&jl_sigint_sset, SIGINT);

    sigset_t sset;
    jl_sigsetset(&sset);
    sigprocmask(SIG_SETMASK, &sset, 0);

#if !defined(HAVE_MACH) && !defined(JL_DISABLE_LIBUNWIND)
    if (pthread_mutex_init(&in_signal_lock, NULL) != 0 ||
        pthread_cond_init(&exit_signal_cond, NULL) != 0 ||
        pthread_cond_init(&signal_caught_cond, NULL) != 0) {
        jl_error("SIGUSR pthread init failed");
    }
#endif

    if (pthread_create(&signals_thread, NULL, signal_listener, NULL) != 0) {
        jl_error("pthread_create(signal_listener) failed");
    }
}

static void fpe_handler(int sig, siginfo_t *info, void *context)
{
    (void)info;
    if (jl_get_safe_restore()) { // restarting jl_ or profile
        jl_call_in_ctx(NULL, &jl_sig_throw, sig, context);
        return;
    }
    jl_task_t *ct = jl_get_current_task();
    if (ct == NULL) // exception on foreign thread is fatal
        sigdie_handler(sig, info, context);
    else
        jl_throw_in_ctx(ct, jl_diverror_exception, sig, context);
}

static void sigint_handler(int sig)
{
    jl_sigint_passed = 1;
}

void jl_install_default_signal_handlers(void)
{
    struct sigaction actf;
    memset(&actf, 0, sizeof(struct sigaction));
    sigemptyset(&actf.sa_mask);
    actf.sa_sigaction = fpe_handler;
    actf.sa_flags = SA_ONSTACK | SA_SIGINFO;
    if (sigaction(SIGFPE, &actf, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
    struct sigaction actint;
    memset(&actint, 0, sizeof(struct sigaction));
    sigemptyset(&actint.sa_mask);
    actint.sa_handler = sigint_handler;
    actint.sa_flags = 0;
    if (sigaction(SIGINT, &actint, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        jl_error("fatal error: Couldn't set SIGPIPE");
    }
    if (signal(SIGTRAP, SIG_IGN) == SIG_ERR) {
        jl_error("fatal error: Couldn't set SIGTRAP");
    }

#if defined(HAVE_MACH)
    allocate_mach_handler();
#else
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = usr2_handler;
    act.sa_flags = SA_ONSTACK | SA_SIGINFO | SA_RESTART;
    if (sigaction(SIGUSR2, &act, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
#endif

    allocate_segv_handler();

    struct sigaction act_die;
    memset(&act_die, 0, sizeof(struct sigaction));
    sigemptyset(&act_die.sa_mask);
    act_die.sa_sigaction = sigdie_handler;
    act_die.sa_flags = SA_SIGINFO | SA_RESETHAND;
    if (sigaction(SIGILL, &act_die, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
    if (sigaction(SIGABRT, &act_die, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
    if (sigaction(SIGSYS, &act_die, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
    // need to ensure the following signals are not SIG_IGN, even though they will be blocked
    act_die.sa_flags = SA_SIGINFO | SA_RESTART | SA_RESETHAND;
#if defined(HAVE_ITIMER)
    if (sigaction(SIGPROF, &act_die, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
#endif
#ifdef SIGINFO
    if (sigaction(SIGINFO, &act_die, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
#else
    if (sigaction(SIGUSR1, &act_die, NULL) < 0) {
        jl_errorf("fatal error: sigaction: %s", strerror(errno));
    }
#endif
}

JL_DLLEXPORT void jl_install_sigint_handler(void)
{
    // TODO: ?
}

JL_DLLEXPORT int jl_repl_raise_sigtstp(void)
{
    return raise(SIGTSTP);
}
