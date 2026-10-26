// The Linux thread-slot scheme (loader.c tp_slot_offsets, DM_THREAD_REG=tpidr_el0): the rewritten code reads a host slot as
//   mrs xT, tpidr_el0; adrp/ldr xK, key page entry; add xT, xT, xK; ldr xS, [xT]
// This builds that sequence in executable memory (the key page entry passed in x0) and checks, on several threads at once, that it reads and writes what
// pthread_getspecific / pthread_setspecific see, for keys created before and after other libraries' keys. Build and run: make -f Makefile.linux t_tpslot
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define NKEY 4
static pthread_key_t g_key[NKEY];
static int64_t g_off[NKEY];
typedef uint64_t (*rd_fn)(const int64_t *entry);
typedef void (*wr_fn)(const int64_t *entry, uint64_t v);
static rd_fn g_rd; static wr_fn g_wr;
static void probe(void) {   // as loader.c tp_slot_offsets
    uint64_t tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    uint64_t *lo = (uint64_t *)pthread_self(), *hi = (uint64_t *)tp;
    if (lo >= hi || hi - lo > 0x1000) { printf("FAIL layout: pthread_self %p tp %#llx\n", (void *)lo, (unsigned long long)tp); exit(1); }
    for (int i = 0; i < NKEY; i++) {
        uint64_t *hit = NULL;
        for (int pass = 0; pass < 2; pass++) {
            uint64_t mark = 0x5d6d000000000000ull ^ ((uint64_t)i << 8) ^ (uint64_t)pass; int n = 0; uint64_t *f = NULL;
            pthread_setspecific(g_key[i], (void *)mark);
            for (uint64_t *p = lo; p < hi; p++) if (*p == mark) { f = p; n++; }
            if (n != 1 || (pass && f != hit)) { printf("FAIL key %d: %d matches\n", i, n); exit(1); }
            hit = f;
        }
        pthread_setspecific(g_key[i], NULL);
        g_off[i] = (int64_t)((uint64_t)hit - tp);
    }
}
static void *worker(void *arg) {
    uintptr_t id = (uintptr_t)arg; int bad = 0;
    for (int round = 0; round < 10000; round++)
        for (int i = 0; i < NKEY; i++) {
            uint64_t v = (id << 40) ^ ((uint64_t)round << 8) ^ (uint64_t)i;
            pthread_setspecific(g_key[i], (void *)v);
            if (g_rd(&g_off[i]) != v) bad++;                      // the rewritten code reads what the host wrote
            g_wr(&g_off[i], v ^ 0xff);                             // and the host reads what the rewritten code wrote (the guest sets KTLS / K18)
            if ((uint64_t)pthread_getspecific(g_key[i]) != (v ^ 0xff)) bad++;
        }
    return (void *)(uintptr_t)bad;
}
int main(void) {
    pthread_key_t other[3]; for (int i = 0; i < 3; i++) pthread_key_create(&other[i], NULL);   // keys the process already holds before the host's
    for (int i = 0; i < NKEY; i++) if (pthread_key_create(&g_key[i], NULL)) { printf("FAIL key create\n"); return 1; }
    probe();
    printf("keys %u..%u, offsets from tpidr_el0:", g_key[0], g_key[NKEY - 1]); for (int i = 0; i < NKEY; i++) printf(" %lld", (long long)g_off[i]); printf("\n");
    uint32_t *code = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint32_t rd[] = { 0xD53BD041 /* mrs x1, tpidr_el0 */, 0xF9400002 /* ldr x2, [x0] */, 0x8B020021 /* add x1, x1, x2 */, 0xF9400020 /* ldr x0, [x1] */, 0xD65F03C0 /* ret */ };
    uint32_t wr[] = { 0xD53BD042 /* mrs x2, tpidr_el0 */, 0xF9400003 /* ldr x3, [x0] */, 0x8B030042 /* add x2, x2, x3 */, 0xF9000041 /* str x1, [x2] */, 0xD65F03C0 /* ret */ };
    memcpy(code, rd, sizeof rd); memcpy(code + 16, wr, sizeof wr);
    __builtin___clear_cache((char *)code, (char *)code + 0x4000);
    if (mprotect(code, 0x4000, PROT_READ | PROT_EXEC)) { printf("FAIL mprotect\n"); return 1; }
    g_rd = (rd_fn)(void *)code; g_wr = (wr_fn)(void *)(code + 16);
    pthread_t t[8]; long bad = (long)(uintptr_t)worker((void *)0);
    for (uintptr_t i = 0; i < 8; i++) pthread_create(&t[i], NULL, worker, (void *)(i + 1));
    for (int i = 0; i < 8; i++) { void *r; pthread_join(t[i], &r); bad += (long)(uintptr_t)r; }
    printf("%s: %ld mismatches over 9 threads x 10000 rounds x %d keys\n", bad ? "FAIL" : "PASS", bad, NKEY);
    return bad != 0;
}
