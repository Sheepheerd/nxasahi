// Linux build only: implementations behind darwin_compat.h
#include "darwin_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <fcntl.h>

// ---- pthread ----
void *pthread_get_stackaddr_np(pthread_t t) {
    pthread_attr_t a; void *lo = NULL; size_t n = 0;
    if (pthread_getattr_np(t, &a)) return NULL;
    pthread_attr_getstack(&a, &lo, &n); pthread_attr_destroy(&a);
    return (char *)lo + n;
}
size_t pthread_get_stacksize_np(pthread_t t) {
    pthread_attr_t a; void *lo = NULL; size_t n = 0;
    if (pthread_getattr_np(t, &a)) return 0;
    pthread_attr_getstack(&a, &lo, &n); pthread_attr_destroy(&a);
    return n;
}
int pthread_threadid_np(pthread_t t, uint64_t *id) {
    if (t && !pthread_equal(t, pthread_self())) { *id = (uint64_t)(uintptr_t)t; return 0; }
    *id = (uint64_t)gettid(); return 0;
}

// ---- mach time: nanoseconds on CLOCK_MONOTONIC ----
uint64_t mach_absolute_time(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
int mach_wait_until(uint64_t deadline) {
    struct timespec ts = { (time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    return KERN_SUCCESS;
}

// ---- vm ----
kern_return_t vm_allocate(mach_port_t task, vm_address_t *addr, vm_size_t size, int flags) {
    (void)task;
    void *hint = (flags & VM_FLAGS_ANYWHERE) ? NULL : (void *)*addr;
    void *p = mmap(hint, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | (hint ? MAP_FIXED_NOREPLACE : 0), -1, 0);
    if (p == MAP_FAILED) return KERN_NO_SPACE;
    *addr = (vm_address_t)p; return KERN_SUCCESS;
}
kern_return_t vm_deallocate(mach_port_t task, vm_address_t addr, vm_size_t size) { (void)task; return munmap((void *)addr, size) ? KERN_INVALID_ARGUMENT : KERN_SUCCESS; }
// A read that fails instead of faulting. Through a pipe: write() copies from the address as any user-memory access does, and fails with EFAULT where nothing is mapped.
// process_vm_readv cannot read the guest heap: it is Vulkan device memory mapped in place (vk_mem.c, VM_PFNMAP), which it refuses, so every check of a guest pointer
// failed (Pokémon: Let's Go's movie events were never signalled, and its opening movie never started)
kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *out) {
    (void)task;
    static __thread int fd[2] = {-1, -1};
    if (fd[0] < 0 && pipe2(fd, O_CLOEXEC | O_NONBLOCK)) { fd[0] = -1; if (out) *out = 0; return KERN_FAILURE; }
    vm_size_t done = 0;
    while (done < size) {
        size_t n = size - done > 16384 ? 16384 : size - done;   // well inside the pipe's 64 KiB
        ssize_t w = write(fd[1], (const void *)(addr + done), n);
        if (w <= 0) break;
        ssize_t r = read(fd[0], (void *)(data + done), (size_t)w);
        if (r != w) break;
        done += (vm_size_t)w;
        if ((size_t)w != n) break;
    }
    if (out) *out = done;
    return done == size ? KERN_SUCCESS : KERN_FAILURE;
}
kern_return_t vm_remap(mach_port_t task, vm_address_t *to, vm_size_t size, vm_address_t mask, int flags, mach_port_t src, vm_address_t from, boolean_t copy, vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inh) {
    (void)task; (void)to; (void)size; (void)mask; (void)flags; (void)src; (void)from; (void)copy; (void)inh;
    if (cur) *cur = 0; if (max) *max = 0;
    return KERN_FAILURE;   // anonymous memory cannot be aliased on Linux (see darwin_compat.h)
}
kern_return_t vm_region_64(mach_port_t task, vm_address_t *addr, vm_size_t *size, vm_region_flavor_t flavor, vm_region_info_t info, mach_msg_type_number_t *cnt, mach_port_t *obj) {
    (void)task; (void)flavor; (void)cnt;
    if (obj) *obj = 0;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return KERN_FAILURE;
    char line[512]; kern_return_t kr = KERN_INVALID_ARGUMENT;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi; char perm[5];
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perm) != 3 || hi <= *addr) continue;
        vm_region_basic_info_data_64_t *bi = (vm_region_basic_info_data_64_t *)info;
        memset(bi, 0, sizeof *bi);
        bi->protection = (perm[0] == 'r' ? VM_PROT_READ : 0) | (perm[1] == 'w' ? VM_PROT_WRITE : 0) | (perm[2] == 'x' ? VM_PROT_EXECUTE : 0);
        bi->max_protection = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE; bi->shared = perm[3] == 's';
        *addr = lo; *size = hi - lo; kr = KERN_SUCCESS; break;
    }
    fclose(f); return kr;
}

// ---- os/proc.h ----
size_t os_proc_available_memory(void) {
    FILE *f = fopen("/proc/meminfo", "r"); if (!f) return 0;
    char line[128]; unsigned long long kb = 0;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
    fclose(f); return (size_t)kb * 1024;
}

// ---- getsectiondata: Makefile.linux's linker script defines __dm_start_<name> / __dm_stop_<name> around each "__DATA,<name>" section ----
const struct mach_header_64 _mh_execute_header = { 0xfeedfacf };
#define SECTIONS(X) X(swhle) X(swforce) X(swnvn) X(swgl)
#define DECL(n) extern const uint8_t __dm_start_##n[] __attribute__((weak)), __dm_stop_##n[] __attribute__((weak));
SECTIONS(DECL)
uint8_t *getsectiondata(const struct mach_header_64 *mh, const char *seg, const char *sect, unsigned long *size) {
    (void)mh; *size = 0;
    if (strcmp(seg, "__DATA")) return NULL;
#define CASE(n) if (!strcmp(sect, #n)) { if (!__dm_start_##n) return NULL; *size = (unsigned long)(__dm_stop_##n - __dm_start_##n); return (uint8_t *)__dm_start_##n; }
    SECTIONS(CASE)
    return NULL;   // the iOS embedded-module sections: the Linux host loads .patched.bin files instead
}

// ---- CommonCrypto SHA-256 (FIPS 180-4) ----
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) { uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10); w[i] = w[i-16] + s0 + w[i-7] + s1; }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
int CC_SHA256_Init(CC_SHA256_CTX *c) {
    static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(c->h, iv, sizeof iv); c->len = 0; c->fill = 0; return 1;
}
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len) {
    const uint8_t *p = data; c->len += len;
    if (c->fill) { uint32_t k = 64 - c->fill < len ? 64 - c->fill : len; memcpy(c->buf + c->fill, p, k); c->fill += k; p += k; len -= k; if (c->fill < 64) return 1; sha256_block(c->h, c->buf); c->fill = 0; }
    for (; len >= 64; p += 64, len -= 64) sha256_block(c->h, p);
    memcpy(c->buf, p, len); c->fill = len; return 1;
}
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c) {
    uint64_t bits = c->len * 8; uint8_t pad = 0x80, z = 0;
    CC_SHA256_Update(c, &pad, 1);
    while (c->fill != 56) CC_SHA256_Update(c, &z, 1);
    uint8_t lb[8]; for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    CC_SHA256_Update(c, lb, 8);
    for (int i = 0; i < 8; i++) { md[4*i] = (uint8_t)(c->h[i] >> 24); md[4*i+1] = (uint8_t)(c->h[i] >> 16); md[4*i+2] = (uint8_t)(c->h[i] >> 8); md[4*i+3] = (uint8_t)c->h[i]; }
    return 1;
}
unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md) { CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, data, len); CC_SHA256_Final(md, &c); return md; }

// ---- dispatch ----
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta) {
    if (when == DISPATCH_TIME_FOREVER) return when;
    uint64_t base = when == DISPATCH_TIME_NOW ? mach_absolute_time() : when;
    return base + (uint64_t)delta;
}
struct dm_dispatch_sem { int kind; sem_t s; };   // kind 1; a source is kind 2
dispatch_semaphore_t dispatch_semaphore_create(long value) { dispatch_semaphore_t d = malloc(sizeof *d); if (d) d->kind = 1; if (d && sem_init(&d->s, 0, (unsigned)value)) { free(d); d = NULL; } return d; }
long dispatch_semaphore_signal(dispatch_semaphore_t d) { sem_post(&d->s); return 0; }
long dispatch_semaphore_wait(dispatch_semaphore_t d, dispatch_time_t timeout) {
    if (timeout == DISPATCH_TIME_FOREVER) { while (sem_wait(&d->s) && errno == EINTR) {} return 0; }
    // sem_clockwait takes the deadline on the same CLOCK_MONOTONIC as mach_absolute_time
    struct timespec ts = { (time_t)(timeout / 1000000000ull), (long)(timeout % 1000000000ull) };
    for (;;) { if (!sem_clockwait(&d->s, CLOCK_MONOTONIC, &ts)) return 0; if (errno != EINTR) return 1; }
}
struct dm_dispatch_queue { int unused; };
static struct dm_dispatch_queue g_global_q;
dispatch_queue_t dispatch_get_global_queue(long qos, unsigned long flags) { (void)qos; (void)flags; return &g_global_q; }
typedef struct { dispatch_time_t when; dispatch_block_t b; } after_t;
static void *after_run(void *p) {
    after_t *a = p;
    if (a->when) mach_wait_until(a->when);
    a->b(); _Block_release(a->b); free(a); return NULL;
}
static void run_detached(dispatch_time_t when, dispatch_block_t b) {
    after_t *a = malloc(sizeof *a); a->when = when; a->b = _Block_copy(b);
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, after_run, a)) { _Block_release(a->b); free(a); }
    pthread_attr_destroy(&at);
}
void dispatch_after(dispatch_time_t when, dispatch_queue_t q, dispatch_block_t b) { (void)q; run_detached(when, b); }
void dispatch_async(dispatch_queue_t q, dispatch_block_t b) { (void)q; run_detached(0, b); }
// Signal sources: a thread waiting in sigwaitinfo for the source's signal, which is blocked in every thread created afterwards (the host creates them at startup, hle_lifecycle.c)
struct dm_dispatch_source { int kind; int sig; dispatch_block_t handler; };
static void *source_run(void *p) {
    dispatch_source_t s = p; sigset_t set; sigemptyset(&set); sigaddset(&set, s->sig);
    for (;;) { if (sigwaitinfo(&set, NULL) == s->sig && s->handler) s->handler(); }
    return NULL;
}
dispatch_source_t dispatch_source_create(const void *type, uintptr_t handle, unsigned long mask, dispatch_queue_t q) {
    (void)mask; (void)q;
    if (type != DISPATCH_SOURCE_TYPE_SIGNAL) return NULL;
    dispatch_source_t s = calloc(1, sizeof *s); s->kind = 2; s->sig = (int)handle; return s;
}
void dispatch_source_set_event_handler(dispatch_source_t s, dispatch_block_t b) { s->handler = _Block_copy(b); }
void dispatch_resume(dispatch_source_t s) {
    sigset_t set; sigemptyset(&set); sigaddset(&set, s->sig); pthread_sigmask(SIG_BLOCK, &set, NULL);
    pthread_t t; if (!pthread_create(&t, NULL, source_run, s)) pthread_detach(t);
}
// A semaphore is freed; a signal source keeps its waiting thread, so it lives as long as the process
void dm_dispatch_release(void *o) { if (o && *(int *)o == 1) { sem_destroy(&((dispatch_semaphore_t)o)->s); free(o); } }

// ---- Signal contexts (darwin_compat.h "Signal contexts") ----
// The kernel's aarch64 context: x0-x30, sp, pc, pstate in uc_mcontext, then records in __reserved, each {magic, size}: FP/SIMD (fpsr, fpcr, v0-v31) and ESR
#define DM_FPSIMD_MAGIC 0x46508001u
#define DM_ESR_MAGIC 0x45535201u
struct dm_ctx_head { uint32_t magic, size; };
struct dm_fpsimd_ctx { struct dm_ctx_head head; uint32_t fpsr, fpcr; __uint128_t vregs[32]; };
struct dm_esr_ctx { struct dm_ctx_head head; uint64_t esr; };
static void *ctx_record(dm_linux_ucontext_t *k, uint32_t magic) {
    uint8_t *p = (uint8_t *)k->uc_mcontext.__reserved, *end = p + sizeof k->uc_mcontext.__reserved;
    while (p + sizeof(struct dm_ctx_head) <= end) {
        struct dm_ctx_head *h = (struct dm_ctx_head *)p;
        if (!h->magic || h->size < sizeof *h) return NULL;
        if (h->magic == magic) return h;
        p += h->size;
    }
    return NULL;
}
typedef void (*dm_sigfn)(int, siginfo_t *, void *);
static _Atomic(dm_sigfn) g_sig_fn[NSIG];
static void sig_trampoline(int sig, siginfo_t *si, void *kv) {
    dm_linux_ucontext_t *k = kv;
    struct __darwin_mcontext64 mc; memset(&mc, 0, sizeof mc);
    for (int i = 0; i < 29; i++) mc.__ss.__x[i] = k->uc_mcontext.regs[i];
    mc.__ss.__fp = k->uc_mcontext.regs[29]; mc.__ss.__lr = k->uc_mcontext.regs[30];
    mc.__ss.__sp = k->uc_mcontext.sp; mc.__ss.__pc = k->uc_mcontext.pc; mc.__ss.__cpsr = (uint32_t)k->uc_mcontext.pstate;
    mc.__es.__far = k->uc_mcontext.fault_address;
    struct dm_fpsimd_ctx *fp = ctx_record(k, DM_FPSIMD_MAGIC);
    if (fp) { memcpy(mc.__ns.__v, fp->vregs, sizeof mc.__ns.__v); mc.__ns.__fpsr = fp->fpsr; mc.__ns.__fpcr = fp->fpcr; }
    struct dm_esr_ctx *es = ctx_record(k, DM_ESR_MAGIC);
    if (es) mc.__es.__esr = (uint32_t)es->esr;
    dm_ucontext_t uc = { 0, k->uc_stack, k->uc_sigmask, &mc, k };
    dm_sigfn fn = atomic_load(&g_sig_fn[sig]);
    if (fn) fn(sig, si, &uc);
    for (int i = 0; i < 29; i++) k->uc_mcontext.regs[i] = mc.__ss.__x[i];
    k->uc_mcontext.regs[29] = mc.__ss.__fp; k->uc_mcontext.regs[30] = mc.__ss.__lr;
    k->uc_mcontext.sp = mc.__ss.__sp; k->uc_mcontext.pc = mc.__ss.__pc; k->uc_mcontext.pstate = (k->uc_mcontext.pstate & ~0xffffffffull) | mc.__ss.__cpsr;
    if (fp) { memcpy(fp->vregs, mc.__ns.__v, sizeof fp->vregs); fp->fpsr = mc.__ns.__fpsr; fp->fpcr = mc.__ns.__fpcr; }
}
#undef sigaction
int dm_sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (sig <= 0 || sig >= NSIG) { errno = EINVAL; return -1; }
    struct sigaction mine, prev;
    const struct sigaction *use = act;
    if (act && (act->sa_flags & SA_SIGINFO)) { mine = *act; mine.sa_sigaction = sig_trampoline; use = &mine; }
    dm_sigfn before = atomic_load(&g_sig_fn[sig]);
    if (act) atomic_store(&g_sig_fn[sig], (act->sa_flags & SA_SIGINFO) ? (dm_sigfn)act->sa_sigaction : NULL);
    if (sigaction(sig, use, &prev)) { if (act) atomic_store(&g_sig_fn[sig], before); return -1; }
    if (old) { *old = prev; if ((prev.sa_flags & SA_SIGINFO) && prev.sa_sigaction == sig_trampoline) old->sa_sigaction = before; }
    return 0;
}

// ---- CoreFoundation: a pointer-keyed open-addressing table ----
#include <CoreFoundation/CoreFoundation.h>
struct dm_cfdict { size_t cap, n, used; const void **k, **v; };   // a removed slot keeps key DM_CF_TOMB
#define DM_CF_TOMB ((const void *)-1)
static size_t cf_hash(const void *p) { uint64_t x = (uint64_t)(uintptr_t)p; x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; return (size_t)x; }
static void cf_grow(struct dm_cfdict *d) {
    size_t oc = d->cap; const void **ok = d->k, **ov = d->v;
    d->cap = oc ? oc * 2 : 16; d->k = calloc(d->cap, sizeof *d->k); d->v = calloc(d->cap, sizeof *d->v); d->n = d->used = 0;
    for (size_t i = 0; i < oc; i++) if (ok[i] && ok[i] != DM_CF_TOMB) CFDictionarySetValue(d, ok[i], ov[i]);
    free(ok); free(ov);
}
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex cap, const void *keycb, const void *valcb) {
    (void)a; (void)cap;
    if (keycb || valcb) { fprintf(stderr, "[RED] CFDictionaryCreateMutable: only NULL callbacks are implemented on Linux\n"); abort(); }
    struct dm_cfdict *d = calloc(1, sizeof *d); cf_grow(d); return d;
}
static size_t cf_find(CFDictionaryRef d, const void *key) {   // the key's slot, or (size_t)-1
    for (size_t i = cf_hash(key) & (d->cap - 1), n = 0; n < d->cap; i = (i + 1) & (d->cap - 1), n++) { if (!d->k[i]) return (size_t)-1; if (d->k[i] == key) return i; }
    return (size_t)-1;
}
const void *CFDictionaryGetValue(CFDictionaryRef d, const void *key) { size_t i = cf_find(d, key); return i == (size_t)-1 ? NULL : d->v[i]; }
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *key, const void *value) {
    size_t i = cf_find(d, key); if (i != (size_t)-1) { d->v[i] = value; return; }
    if ((d->used + 1) * 4 > d->cap * 3) cf_grow(d);
    for (i = cf_hash(key) & (d->cap - 1); d->k[i] && d->k[i] != DM_CF_TOMB; i = (i + 1) & (d->cap - 1)) {}
    if (!d->k[i]) d->used++;
    d->k[i] = key; d->v[i] = value; d->n++;
}
void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *key) { size_t i = cf_find(d, key); if (i != (size_t)-1) { d->k[i] = DM_CF_TOMB; d->v[i] = NULL; d->n--; } }
CFIndex CFDictionaryGetCount(CFDictionaryRef d) { return (CFIndex)d->n; }
void CFRelease(CFTypeRef o) { struct dm_cfdict *d = (struct dm_cfdict *)o; if (d) { free(d->k); free(d->v); free(d); } }

// ---- clonefile: recursive copy (sys/clonefile.h) ----
#include <sys/clonefile.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>
static int copy_xattrs(const char *src, const char *dst) {
    ssize_t n = llistxattr(src, NULL, 0); if (n <= 0) return 0;
    char *names = malloc((size_t)n); n = llistxattr(src, names, (size_t)n);
    for (ssize_t o = 0; o < n; o += (ssize_t)strlen(names + o) + 1) {
        char val[4096]; ssize_t v = lgetxattr(src, names + o, val, sizeof val);
        if (v >= 0 && lsetxattr(dst, names + o, val, (size_t)v, 0)) { free(names); return -1; }
    }
    free(names); return 0;
}
static int copy_file(const char *src, const char *dst, mode_t mode) {
    int in = open(src, O_RDONLY | O_CLOEXEC); if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 07777); if (out < 0) { int e = errno; close(in); errno = e; return -1; }
    int r = 0;
    if (ioctl(out, FICLONE, in)) {   // no reflink here: copy (copy_file_range stays in the kernel)
        for (;;) { ssize_t k = copy_file_range(in, NULL, out, NULL, 1 << 30, 0); if (k > 0) continue; if (k < 0) r = -1; break; }
        if (r && (errno == EXDEV || errno == EINVAL || errno == ENOSYS)) {
            r = 0; lseek(in, 0, SEEK_SET); lseek(out, 0, SEEK_SET); char b[65536]; ssize_t k;
            while ((k = read(in, b, sizeof b)) > 0) if (write(out, b, (size_t)k) != k) { r = -1; break; }
            if (k < 0) r = -1;
        }
    }
    int e = errno; close(in); if (close(out)) r = -1; errno = e;
    return r ? -1 : copy_xattrs(src, dst);
}
static int clone_tree(const char *src, const char *dst) {
    struct stat st; if (lstat(src, &st)) return -1;
    if (S_ISREG(st.st_mode)) return copy_file(src, dst, st.st_mode);
    if (S_ISLNK(st.st_mode)) { char t[PATH_MAX]; ssize_t n = readlink(src, t, sizeof t - 1); if (n < 0) return -1; t[n] = 0; return symlink(t, dst); }
    if (!S_ISDIR(st.st_mode)) { errno = ENOTSUP; return -1; }
    if (mkdir(dst, st.st_mode & 07777)) return -1;
    DIR *d = opendir(src); if (!d) return -1;
    struct dirent *e; int r = 0;
    while (!r && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s[PATH_MAX], t[PATH_MAX];
        if (snprintf(s, sizeof s, "%s/%s", src, e->d_name) >= (int)sizeof s || snprintf(t, sizeof t, "%s/%s", dst, e->d_name) >= (int)sizeof t) { errno = ENAMETOOLONG; r = -1; break; }
        r = clone_tree(s, t);
    }
    int er = errno; closedir(d); errno = er;
    return r ? -1 : copy_xattrs(src, dst);
}
int clonefile(const char *src, const char *dst, uint32_t flags) {
    (void)flags;
    if (!access(dst, F_OK)) { errno = EEXIST; return -1; }   // as clonefile: the destination must not exist
    return clone_tree(src, dst);
}

// ---- big cores ----
// On a CPU with cores of different capacity (Apple M1 under Asahi: CPUs 0-3 Icestorm at capacity 493, 4-7 Firestorm at 1024) the scheduler's energy-aware placement kept the
// game's threads on the small cores, where Mario Wonder's frame took 18 ms against 16.7 ms (its 60 fps) on the large ones. Before main, the process limits itself to the
// cores of the highest capacity; every thread inherits it. DM_CPUS=all leaves the placement to the scheduler, DM_CPUS=<list> (as taskset -c) picks the cores
#include <sched.h>
static int cpu_capacity(int c) {
    char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpu_capacity", c);
    FILE *f = fopen(p, "r"); int v = -1; if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); } return v;
}
__attribute__((constructor)) static void prefer_big_cores(void) {
    const char *e = getenv("DM_CPUS"); if (e && !strcmp(e, "all")) return;
    cpu_set_t set; CPU_ZERO(&set); int n = 0;
    if (e && *e) {   // a list: 4-7 or 4,5,6,7
        for (const char *p = e; *p;) { char *q; long a = strtol(p, &q, 10), b = a; if (q == p) break; if (*q == '-') { p = q + 1; b = strtol(p, &q, 10); }
            for (long c = a; c <= b && c < CPU_SETSIZE; c++) if (c >= 0) { CPU_SET((int)c, &set); n++; }
            p = *q == ',' ? q + 1 : q; if (*q && *q != ',') break; }
    } else {
        int cap[CPU_SETSIZE], ncpu = 0, best = -1, worst = 1 << 30;
        for (int c = 0; c < 256; c++) { cap[c] = cpu_capacity(c); if (cap[c] < 0) break; ncpu = c + 1; if (cap[c] > best) best = cap[c]; if (cap[c] < worst) worst = cap[c]; }
        if (ncpu < 2 || best <= worst) return;   // all cores alike (or not known): nothing to choose
        for (int c = 0; c < ncpu; c++) if (cap[c] == best) { CPU_SET(c, &set); n++; }
    }
    if (n && !sched_setaffinity(0, sizeof set, &set)) fprintf(stderr, "[dm] CPU: %d core%s of the highest capacity (DM_CPUS=all for every core)\n", n, n == 1 ? "" : "s");
}
