// The Darwin-shaped signal context of darwin_compat.c (sig_trampoline): a handler written for Darwin (uc->uc_mcontext->__ss / __ns, as crash.c, probe.c and
// hle_thread.c's pause signal) reads the interrupted registers and its changes reach the kernel's context. A brk traps; the handler checks x0, x1, sp-relative
// state and v0, sets x0, and moves pc past the brk (as probe.c resumes at a slot); a write fault shows its ESR WnR bit and address (crash.c). Build and run: make -f Makefile.linux t_sigctx
#include <darwin_compat.h>
#include <stdio.h>
static volatile int g_seen_x0_ok, g_seen_v0_ok, g_wnr = -1; static volatile uint64_t g_far;
// A write to a read-only page: Linux gives data aborts an ESR record (a brk gets none); crash.c reads its WnR bit
static void on_segv(int sig, siginfo_t *si, void *ucv) {
    (void)sig; ucontext_t *uc = ucv;
    g_wnr = (int)((uc->uc_mcontext->__es.__esr >> 6) & 1); g_far = uc->uc_mcontext->__es.__far;
    if ((uint64_t)si->si_addr != g_far) g_far = 0;
    uc->uc_mcontext->__ss.__pc += 4;   // skip the store
}
static void on_trap(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    ucontext_t *uc = ucv;
    g_seen_x0_ok = uc->uc_mcontext->__ss.__x[0] == 0x1111 && uc->uc_mcontext->__ss.__x[1] == 0x2222;
    uint64_t v0; memcpy(&v0, &uc->uc_mcontext->__ns.__v[0], 8); g_seen_v0_ok = v0 == 0x3333;
    uc->uc_mcontext->__ss.__x[0] = 0xabcd;
    uc->uc_mcontext->__ss.__pc += 4;
}
int main(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_trap; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTRAP, &sa, NULL)) { printf("FAIL sigaction\n"); return 1; }
    struct sigaction back; sigaction(SIGTRAP, NULL, &back);
    int old_ok = back.sa_sigaction == on_trap;   // the old action reads as the handler installed, not the trampoline
    register uint64_t x0 __asm__("x0") = 0x1111, x1 __asm__("x1") = 0x2222;
    __asm__ volatile("fmov d0, %2\n brk #0x18" : "+r"(x0) : "r"(x1), "r"((uint64_t)0x3333) : "v0", "memory");
    uint64_t after = x0;   // a register variable is only bound inside the asm: read it before any call
    sa.sa_sigaction = on_segv; sigaction(SIGSEGV, &sa, NULL);
    volatile uint64_t *ro = mmap(NULL, 0x4000, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    static volatile uintptr_t want; want = (uintptr_t)&ro[1];   // before the store: the skipped store may have been the one to advance the address register
    ro[1] = 7;   // faults; the handler skips the store
    int seg_ok = g_wnr == 1 && g_far == want;
    if (!seg_ok) printf("fault address %#llx, store to %p\n", (unsigned long long)g_far, (void *)want);
    int ok = after == 0xabcd && g_seen_x0_ok && g_seen_v0_ok && old_ok && seg_ok;
    printf("%s: registers seen %d, v0 seen %d, x0 after %#llx, old action %d; write fault WnR %d, address %s\n", ok ? "PASS" : "FAIL", g_seen_x0_ok, g_seen_v0_ok, (unsigned long long)after, old_ok, g_wnr, seg_ok ? "matches" : "wrong");
    return !ok;
}
