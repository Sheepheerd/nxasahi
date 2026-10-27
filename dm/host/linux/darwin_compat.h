// Linux build only (dm/host/Makefile.linux): the Darwin APIs the host uses, on glibc. Force-included into every host source (-include darwin_compat.h), and the Darwin
// headers in this directory (mach/mach.h, TargetConditionals.h, ...) only include it, so the shared sources build unchanged. Three kinds of stand-in:
//   - real equivalents: mach time, sys_icache_invalidate, CommonCrypto SHA-256, malloc_size, dispatch semaphores and timers, vm_allocate / vm_read_overwrite / vm_region_64,
//     getsectiondata (the host's registration tables, through the section symbols Makefile.linux's linker script defines)
//   - failures the callers already handle (KERN_FAILURE, -1): the diagnostics that read other threads' state (thread_info, thread_suspend, task_threads), task_info's
//     footprint and power counters, Mach real-time policy, sysctl
//   - vm_remap also fails: Linux cannot alias anonymous memory. The NVN sparse pools that use it then keep the mapping in their page table only (hle_nvn.c vpool_map); open
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <malloc.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <Block.h>

// ---- TargetConditionals.h: neither iOS nor macOS (the desktop paths are the !TARGET_OS_IPHONE ones) ----
#define TARGET_OS_MAC 0
#define TARGET_OS_OSX 0
#define TARGET_OS_IPHONE 0
#define TARGET_OS_IOS 0
#define TARGET_OS_SIMULATOR 0
#define TARGET_OS_LINUX 1

// ---- time.h / pthread.h extensions ----
static inline uint64_t clock_gettime_nsec_np(clockid_t c) { struct timespec ts; if (clock_gettime(c, &ts)) return 0; return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC_RAW
#endif
void *pthread_get_stackaddr_np(pthread_t t);   // the stack's top (highest address), as on Darwin
size_t pthread_get_stacksize_np(pthread_t t);
int pthread_threadid_np(pthread_t t, uint64_t *id);
// Darwin names only the calling thread (one argument); glibc takes the thread and truncates names past 15 bytes (it fails with ERANGE instead: cut here)
static inline int dm_setname2(pthread_t t, const char *n) { char b[16]; snprintf(b, sizeof b, "%s", n); return pthread_setname_np(t, b); }
static inline int dm_setname_self(const char *n) { return dm_setname2(pthread_self(), n); }
#define DM_SETNAME_PICK(_1, _2, f, ...) f
#define pthread_setname_np(...) DM_SETNAME_PICK(__VA_ARGS__, dm_setname2, dm_setname_self, 0)(__VA_ARGS__)
#ifndef F_GETPATH
#define F_GETPATH 0x7fff0032   // not a Linux fcntl command: fcntl fails with EINVAL and hle_fdstat.c counts the descriptor as "other"
#endif

// ---- mach/mach_time.h ----
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t, *mach_timebase_info_t;
static inline int mach_timebase_info(mach_timebase_info_t i) { i->numer = 1; i->denom = 1; return 0; }
uint64_t mach_absolute_time(void);   // nanoseconds on CLOCK_MONOTONIC (timebase 1/1); a real function: nvn_entry.S calls it
static inline uint64_t mach_continuous_time(void) { return clock_gettime_nsec_np(CLOCK_BOOTTIME); }
int mach_wait_until(uint64_t deadline);
#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_USEC 1000ull
#define USEC_PER_SEC 1000000ull
#endif

// ---- mach/mach.h: types, return codes, the calls the host makes ----
typedef int kern_return_t;
typedef uint32_t mach_port_t, thread_act_t, task_t, natural_t;
typedef thread_act_t *thread_act_array_t;
typedef natural_t mach_msg_type_number_t;
typedef int integer_t, boolean_t, vm_prot_t, vm_inherit_t, policy_t;
typedef uintptr_t vm_address_t, vm_offset_t, vm_size_t, mach_vm_address_t, mach_vm_size_t;
typedef integer_t *thread_info_t, *task_info_t, *thread_policy_t, *vm_region_info_t, *thread_state_t;
typedef natural_t thread_flavor_t, task_flavor_t, thread_policy_flavor_t, vm_region_flavor_t;
#define KERN_SUCCESS 0
#define KERN_INVALID_ARGUMENT 4
#define KERN_FAILURE 5
#define KERN_NO_SPACE 3
#define MACH_PORT_NULL 0
#ifndef FALSE
#define FALSE 0
#define TRUE 1
#endif
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_FLAGS_FIXED 0
#define VM_FLAGS_ANYWHERE 1
#define VM_FLAGS_OVERWRITE 0x4000
#define VM_INHERIT_NONE 2
static inline mach_port_t mach_task_self(void) { return 1; }
static inline mach_port_t mach_thread_self(void) { return 0; }    // 0 = "no port": the host's per-thread diagnostics skip a thread without one
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { (void)t; return 0; }
static inline kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t p) { (void)task; (void)p; return KERN_SUCCESS; }
kern_return_t vm_allocate(mach_port_t task, vm_address_t *addr, vm_size_t size, int flags);
kern_return_t vm_deallocate(mach_port_t task, vm_address_t addr, vm_size_t size);
kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *out);   // fails instead of faulting (through a pipe: works on device memory too)
kern_return_t vm_remap(mach_port_t task, vm_address_t *to, vm_size_t size, vm_address_t mask, int flags, mach_port_t src, vm_address_t from, boolean_t copy, vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inh);

typedef struct { vm_prot_t protection, max_protection; vm_inherit_t inheritance; boolean_t shared, reserved; uint64_t offset; int behavior; unsigned short user_wired_count; } vm_region_basic_info_data_64_t;
#define VM_REGION_BASIC_INFO_64 9
#define VM_REGION_BASIC_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_basic_info_data_64_t) / sizeof(int)))
// The mapping containing *addr, or the next one above it (as Darwin): from /proc/self/maps
kern_return_t vm_region_64(mach_port_t task, vm_address_t *addr, vm_size_t *size, vm_region_flavor_t flavor, vm_region_info_t info, mach_msg_type_number_t *cnt, mach_port_t *obj);
static inline kern_return_t mach_vm_region(mach_port_t task, mach_vm_address_t *addr, mach_vm_size_t *size, vm_region_flavor_t flavor, vm_region_info_t info, mach_msg_type_number_t *cnt, mach_port_t *obj) { return vm_region_64(task, addr, size, flavor, info, cnt, obj); }
static inline kern_return_t mach_vm_remap(mach_port_t task, mach_vm_address_t *to, mach_vm_size_t size, mach_vm_address_t mask, int flags, mach_port_t src, mach_vm_address_t from, boolean_t copy, vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inh) { return vm_remap(task, to, size, mask, flags, src, from, copy, cur, max, inh); }

// Thread and task introspection: diagnostics only, all fail
typedef struct { int seconds, microseconds; } time_value_t;
typedef struct { time_value_t user_time, system_time; integer_t cpu_usage; policy_t policy; integer_t run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
typedef struct { uint64_t pth_user_time, pth_system_time; int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority; char pth_name[64]; } thread_extended_info_data_t;
typedef struct { uint32_t period, computation, constraint; boolean_t preemptible; } thread_time_constraint_policy_data_t;
typedef struct { uint64_t virtual_size, region_count, page_size, resident_size, resident_size_peak, device, device_peak, internal, internal_peak, external, external_peak, reusable, reusable_peak,
                 purgeable_volatile_pmap, purgeable_volatile_resident, purgeable_volatile_virtual, compressed, compressed_peak, compressed_lifetime, phys_footprint, min_address, max_address,
                 ledger_phys_footprint_peak, ledger_purgeable_nonvolatile, ledger_purgeable_novolatile_compressed, ledger_purgeable_volatile, ledger_purgeable_volatile_compressed,
                 ledger_tag_network_nonvolatile, ledger_tag_network_nonvolatile_compressed, ledger_tag_network_volatile, ledger_tag_network_volatile_compressed,
                 ledger_tag_media_footprint, ledger_tag_media_footprint_compressed, ledger_tag_media_nofootprint, ledger_tag_media_nofootprint_compressed,
                 ledger_tag_graphics_footprint, ledger_tag_graphics_footprint_compressed, ledger_tag_graphics_nofootprint, ledger_tag_graphics_nofootprint_compressed,
                 ledger_tag_neural_footprint, ledger_tag_neural_footprint_compressed, ledger_tag_neural_nofootprint, ledger_tag_neural_nofootprint_compressed, limit_bytes_remaining; } task_vm_info_data_t;
typedef struct { uint64_t total_user, total_system, task_interrupt_wakeups, task_platform_idle_wakeups, task_timer_wakeups_bin_1, task_timer_wakeups_bin_2; } task_power_info_data_t;
typedef struct { uint64_t task_gpu_utilisation, task_gpu_stat_reserved0, task_gpu_stat_reserved1, task_gpu_stat_reserved2; } gpu_energy_data;
typedef struct { task_power_info_data_t cpu_energy; gpu_energy_data gpu_energy; uint64_t task_ptime, task_pset_switches, task_energy; } task_power_info_v2_data_t;
// Darwin's arm64 register state, with its field names: thread_get_state's result, and what a signal handler reaches through uc->uc_mcontext (below)
struct __darwin_arm_exception_state64 { unsigned long long __far; uint32_t __esr, __exception; };
struct __darwin_arm_thread_state64 { unsigned long long __x[29], __fp, __lr, __sp, __pc; uint32_t __cpsr, __pad; };
struct __darwin_arm_neon_state64 { __uint128_t __v[32]; uint32_t __fpsr, __fpcr; };
struct __darwin_mcontext64 { struct __darwin_arm_exception_state64 __es; struct __darwin_arm_thread_state64 __ss; struct __darwin_arm_neon_state64 __ns; };
#define _STRUCT_ARM_THREAD_STATE64 struct __darwin_arm_thread_state64
#define _STRUCT_ARM_NEON_STATE64 struct __darwin_arm_neon_state64
typedef struct __darwin_arm_thread_state64 arm_thread_state64_t;
#define __darwin_arm_thread_state64_get_pc(s) ((s).__pc)
#define __darwin_arm_thread_state64_get_lr(s) ((s).__lr)
#define __darwin_arm_thread_state64_get_fp(s) ((s).__fp)
#define __darwin_arm_thread_state64_get_sp(s) ((s).__sp)
#define arm_thread_state64_get_pc __darwin_arm_thread_state64_get_pc
#define arm_thread_state64_get_lr __darwin_arm_thread_state64_get_lr
#define arm_thread_state64_get_fp __darwin_arm_thread_state64_get_fp
#define arm_thread_state64_get_sp __darwin_arm_thread_state64_get_sp
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(int)))
#define THREAD_EXTENDED_INFO 5
#define THREAD_EXTENDED_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_extended_info_data_t) / sizeof(int)))
#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT ((mach_msg_type_number_t)(sizeof(thread_time_constraint_policy_data_t) / sizeof(int)))
#define TASK_VM_INFO 22
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(int)))
#define TASK_POWER_INFO_V2 26
#define TASK_POWER_INFO_V2_COUNT ((mach_msg_type_number_t)(sizeof(task_power_info_v2_data_t) / sizeof(int)))
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_thread_state64_t) / sizeof(int)))
#define TH_STATE_RUNNING 1
#define TH_STATE_STOPPED 2
#define TH_STATE_WAITING 3
#define TH_STATE_UNINTERRUPTIBLE 4
#define TH_STATE_HALTED 5
static inline kern_return_t thread_info(thread_act_t t, thread_flavor_t f, thread_info_t i, mach_msg_type_number_t *n) { (void)t; (void)f; (void)i; (void)n; return KERN_FAILURE; }
static inline kern_return_t thread_suspend(thread_act_t t) { (void)t; return KERN_FAILURE; }
static inline kern_return_t thread_resume(thread_act_t t) { (void)t; return KERN_FAILURE; }
static inline kern_return_t thread_get_state(thread_act_t t, int f, thread_state_t s, mach_msg_type_number_t *n) { (void)t; (void)f; (void)s; (void)n; return KERN_FAILURE; }
static inline kern_return_t task_threads(mach_port_t task, thread_act_array_t *a, mach_msg_type_number_t *n) { (void)task; *a = NULL; *n = 0; return KERN_FAILURE; }
static inline kern_return_t task_info(mach_port_t task, task_flavor_t f, task_info_t i, mach_msg_type_number_t *n) { (void)task; (void)f; (void)i; (void)n; return KERN_FAILURE; }
static inline kern_return_t thread_policy_set(thread_act_t t, thread_policy_flavor_t f, thread_policy_t p, mach_msg_type_number_t n) { (void)t; (void)f; (void)p; (void)n; return KERN_FAILURE; }
static inline kern_return_t thread_policy_get(thread_act_t t, thread_policy_flavor_t f, thread_policy_t p, mach_msg_type_number_t *n, boolean_t *def) { (void)t; (void)f; (void)p; (void)n; (void)def; return KERN_FAILURE; }

// ---- Signal contexts: a Darwin-shaped ucontext_t ----
// The host's SA_SIGINFO handlers read and write registers as uc->uc_mcontext->__ss.__pc (crash reports, the SVC pause signal, probes). sigaction(...) installs a trampoline
// instead, which hands the handler a Darwin-shaped copy of the kernel's context (general registers, the FP/SIMD record, ESR) and writes the registers back when it returns,
// so a handler that moves pc (probe.c) or changes a register takes effect. ucontext_t means this type from here on; glibc's own is struct ucontext_t / dm_linux_ucontext_t
#include <ucontext.h>
typedef ucontext_t dm_linux_ucontext_t;
typedef struct { unsigned long uc_onstack; stack_t uc_stack; sigset_t uc_sigmask; struct __darwin_mcontext64 *uc_mcontext; dm_linux_ucontext_t *uc_linux; } dm_ucontext_t;
#define ucontext_t dm_ucontext_t
int dm_sigaction(int sig, const struct sigaction *act, struct sigaction *old);
#define sigaction(sig, act, old) dm_sigaction(sig, act, old)

// ---- pthread/qos.h: no QoS classes on Linux; the host's QoS requests are accepted and ignored ----
typedef enum { QOS_CLASS_USER_INTERACTIVE = 0x21, QOS_CLASS_USER_INITIATED = 0x19, QOS_CLASS_DEFAULT = 0x15, QOS_CLASS_UTILITY = 0x11, QOS_CLASS_BACKGROUND = 0x09, QOS_CLASS_UNSPECIFIED = 0 } qos_class_t;
static inline int pthread_set_qos_class_self_np(qos_class_t q, int rel) { (void)q; (void)rel; return 0; }
static inline int pthread_attr_set_qos_class_np(pthread_attr_t *a, qos_class_t q, int rel) { (void)a; (void)q; (void)rel; return 0; }
static inline qos_class_t qos_class_self(void) { return QOS_CLASS_UNSPECIFIED; }
static inline int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *rel) { (void)t; if (q) *q = QOS_CLASS_UNSPECIFIED; if (rel) *rel = 0; return 0; }

// ---- libkern/OSCacheControl.h ----
static inline void sys_icache_invalidate(void *p, size_t n) { __builtin___clear_cache((char *)p, (char *)p + n); }
static inline void sys_dcache_flush(void *p, size_t n) { (void)p; (void)n; }

// ---- malloc/malloc.h: the Unreal heap's zone is the process heap ----
typedef struct malloc_zone malloc_zone_t;
static inline size_t malloc_size(const void *p) { return malloc_usable_size((void *)p); }
static inline size_t malloc_good_size(size_t n) { return n < 16 ? 16 : (n + 15) & ~(size_t)15; }   // glibc rounds to 16-byte chunks
static inline malloc_zone_t *malloc_create_zone(size_t start, unsigned flags) { (void)start; (void)flags; return (malloc_zone_t *)1; }
static inline void malloc_set_zone_name(malloc_zone_t *z, const char *n) { (void)z; (void)n; }
static inline void *malloc_zone_malloc(malloc_zone_t *z, size_t n) { (void)z; return malloc(n); }
static inline void *malloc_zone_memalign(malloc_zone_t *z, size_t a, size_t n) { (void)z; void *p; return posix_memalign(&p, a, n) ? NULL : p; }
static inline void *malloc_zone_realloc(malloc_zone_t *z, void *p, size_t n) { (void)z; return realloc(p, n); }
static inline void malloc_zone_free(malloc_zone_t *z, void *p) { (void)z; free(p); }

// ---- CommonCrypto/CommonDigest.h: SHA-256 only ----
typedef uint32_t CC_LONG;
#define CC_SHA256_DIGEST_LENGTH 32
#define CC_SHA256_BLOCK_BYTES 64
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t fill; } CC_SHA256_CTX;
int CC_SHA256_Init(CC_SHA256_CTX *c);
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len);
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c);
unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md);

// ---- mach-o/getsect.h: sections of the executable ----
// Darwin's section("__DATA,<name>") attributes become ELF sections named "__DATA,<name>"; Makefile.linux's linker script brackets each with __dm_start_<name> / __dm_stop_<name>
struct mach_header_64 { uint32_t magic; };
extern const struct mach_header_64 _mh_execute_header;
uint8_t *getsectiondata(const struct mach_header_64 *mh, const char *seg, const char *sect, unsigned long *size);

// ---- sys/sysctl.h: no BSD sysctl; callers fall back ----
#define CTL_KERN 1
#define KERN_PROC 14
#define KERN_PROC_PID 1
struct kinfo_proc { struct { struct timeval p_starttime; } kp_proc; };
static inline int sysctl(int *mib, unsigned n, void *old, size_t *oldn, void *nw, size_t nwn) { (void)mib; (void)n; (void)old; (void)oldn; (void)nw; (void)nwn; errno = ENOSYS; return -1; }
static inline int sysctlbyname(const char *name, void *old, size_t *oldn, void *nw, size_t nwn) { (void)name; (void)old; (void)oldn; (void)nw; (void)nwn; errno = ENOENT; return -1; }

// ---- os/proc.h ----
size_t os_proc_available_memory(void);   // what the system can still give: MemAvailable from /proc/meminfo

// ---- dispatch/dispatch.h: semaphores, one-shot timers on a global queue, signal sources (blocks: clang -fblocks with libBlocksRuntime) ----
typedef uint64_t dispatch_time_t;
#define DISPATCH_TIME_NOW 0ull
#define DISPATCH_TIME_FOREVER (~0ull)
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta);
typedef struct dm_dispatch_sem *dispatch_semaphore_t;
dispatch_semaphore_t dispatch_semaphore_create(long value);
long dispatch_semaphore_signal(dispatch_semaphore_t s);
long dispatch_semaphore_wait(dispatch_semaphore_t s, dispatch_time_t timeout);   // 0 = taken, nonzero = timed out
typedef struct dm_dispatch_queue *dispatch_queue_t;
typedef struct dm_dispatch_source *dispatch_source_t;
typedef void (^dispatch_block_t)(void);
dispatch_queue_t dispatch_get_global_queue(long qos, unsigned long flags);
void dispatch_after(dispatch_time_t when, dispatch_queue_t q, dispatch_block_t b);
void dispatch_async(dispatch_queue_t q, dispatch_block_t b);
#define DISPATCH_SOURCE_TYPE_SIGNAL ((const void *)1)
dispatch_source_t dispatch_source_create(const void *type, uintptr_t handle, unsigned long mask, dispatch_queue_t q);
void dispatch_source_set_event_handler(dispatch_source_t s, dispatch_block_t b);
void dispatch_resume(dispatch_source_t s);
void dm_dispatch_release(void *o);
#define dispatch_release(o) dm_dispatch_release((void *)(o))

// ---- pthread QoS overrides: none (see pthread/qos.h above) ----
typedef void *pthread_override_t;
static inline pthread_override_t pthread_override_qos_class_start_np(pthread_t t, qos_class_t q, int rel) { (void)t; (void)q; (void)rel; return NULL; }
static inline int pthread_override_qos_class_end_np(pthread_override_t o) { (void)o; return 0; }

// ---- sys/xattr.h: Darwin's calls take a position and options (XATTR_NOFOLLOW → the l* calls) ----
#include <sys/xattr.h>
#define XATTR_NOFOLLOW 1
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif
// Linux names carry a namespace (user., trusted., security., system.) and refuse others with EOPNOTSUPP; a Darwin name such as hle_fs.c's org.d2rmobile.guest-name goes in user.
static inline const char *dm_xattr_name(const char *n, char *buf, size_t cap) {
    if (!strncmp(n, "user.", 5) || !strncmp(n, "trusted.", 8) || !strncmp(n, "security.", 9) || !strncmp(n, "system.", 7)) return n;
    snprintf(buf, cap, "user.%s", n); return buf;
}
#define DM_XN(n) dm_xattr_name(n, (char[256]){0}, 256)
#define getxattr(p, n, v, s, pos, opt) ((void)(pos), ((opt) & XATTR_NOFOLLOW) ? lgetxattr(p, DM_XN(n), v, s) : getxattr(p, DM_XN(n), v, s))
#define setxattr(p, n, v, s, pos, opt) ((void)(pos), ((opt) & XATTR_NOFOLLOW) ? lsetxattr(p, DM_XN(n), v, s, 0) : setxattr(p, DM_XN(n), v, s, 0))
#define removexattr(p, n, opt) (((opt) & XATTR_NOFOLLOW) ? lremovexattr(p, DM_XN(n)) : removexattr(p, DM_XN(n)))

// ---- stdio.h renamex_np: RENAME_SWAP is renameat2's RENAME_EXCHANGE ----
#include <stdio.h>
#define RENAME_SWAP (1u << 1)
#define RENAME_EXCL (1u << 2)
static inline int renamex_np(const char *from, const char *to, unsigned flags) {
    return renameat2(AT_FDCWD, from, AT_FDCWD, to, (flags & RENAME_SWAP ? RENAME_EXCHANGE : 0) | (flags & RENAME_EXCL ? RENAME_NOREPLACE : 0));
}

// ---- pthread_cond_timedwait_relative_np: a relative timeout, as a CLOCK_REALTIME deadline for pthread_cond_timedwait ----
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += rel->tv_sec; ts.tv_nsec += rel->tv_nsec; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(c, m, &ts);
}
