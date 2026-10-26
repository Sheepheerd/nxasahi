// Linux build, first milestone: the host without a window or a GPU. Stands in for what the C sources call in the Objective-C parts the Linux build leaves out:
//   - the Metal backend of the NVN layer (nvn_mtl.m, the mtl_* calls of nvn_obj.h): command buffers are dropped, every fence completes at once, presents go
//     to the window when there is one (vk_window.c) and are counted otherwise; textures read back as left (no GPU wrote them). This is the seam a Vulkan backend replaces
//   - the front end's software keyboard (front_mac.m; the window is front_sdl.c)
//   - the graphics dumps of the DM_CTL control file (dm_gfx_*), and libSystem's private thread_selfcounts
// The game's CPU side runs as on the Mac: it boots, loads, and submits frames that nobody draws
#include "dm.h"
#include "front.h"
#include "../gfx/nvn/nvn_obj.h"
#include "vk_window.h"
#include <stdatomic.h>

// ---- NVN on nothing ----
int nvn_game_fps(void);   // hle_nvn.c: the rate the game paces itself at
static _Atomic uint64_t g_fence, g_presents, g_frame_no;
void mtl_exec(const uint8_t *handle) { (void)handle; }
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_sub_begin(void) {}
uint64_t mtl_sub_commit(void) { return atomic_fetch_add(&g_fence, 1) + 1; }
uint64_t mtl_last_fence(void) { return atomic_load(&g_fence); }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) { (void)fence; (void)timeout_ns; return 1; }
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { (void)fence; (void)timeout_ns; return 1; }
void mtl_finish(void) {}
void mtl_drain(void) {}
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)t; (void)crop;
    if (vkw_active()) return vkw_present(period_s);   // a window (front_sdl.c, DM_WINDOW=1): the present thread shows it
    uint64_t n = atomic_fetch_add(&g_presents, 1) + 1;
    if (n == 1 || !(n & (n - 1))) LOG("[headless] present %llu (no window: frames are not drawn)", (unsigned long long)n);
    return n;
}
void mtl_present_wait(uint64_t ticket) { if (vkw_active()) vkw_present_wait(ticket); }
double mtl_presented_time(uint64_t n) { (void)n; return -1; }
uint32_t mtl_present_qos(void) { return 0; }
int mtl_display_hz(void) { return vkw_display_hz(); }
int mtl_has_window(void) { return vkw_active(); }
int mtl_fps_target(void) { const char *e = getenv("DM_FPS"); int v = e ? atoi(e) : 0; return v == 30 || v == 60 ? v : nvn_game_fps() == 30 ? 30 : 60; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
int mtl_link_stalled(void) { return 0; }
int mtl_snap_on(void) { return 0; }
uint64_t mtl_snap_acquire_wait(void) { return 0; }
void mtl_set_frame(uint64_t f) { atomic_store(&g_frame_no, f); }
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_set_depth_zero(int on) { (void)on; }
void mtl_set_origin_lower_left(int on) { (void)on; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_tex_init(const tex_t *t) { (void)t; }
void mtl_tex_release(tex_t *t) { (void)t; }
void mtl_tex_cpu_written(const tex_t *t) { (void)t; }
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void mtl_reg_samp(int id, const samp_par *p) { (void)id; (void)p; }
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    (void)t; (void)level; (void)layer0; (void)r; (void)dst; (void)row; (void)img;
    static int warned; if (!warned++) LOG("[RED] [headless] texture read-back: no GPU, the destination keeps its bytes");
}
// Memory pools: counted by the alignment of their storage and size, which decides whether a Vulkan backend can map device memory over them in place (VK_EXT_map_memory_placed: 16 KiB)
static _Atomic uint64_t g_pools, g_pool_bytes, g_pool_a16, g_pool_a16_bytes;
void mtl_pool_add(const void *mem, uint64_t size) {
    uint64_t n = atomic_fetch_add(&g_pools, 1) + 1; atomic_fetch_add(&g_pool_bytes, size);
    int a16 = !(((uintptr_t)mem | size) & 0x3FFF); if (a16) { atomic_fetch_add(&g_pool_a16, 1); atomic_fetch_add(&g_pool_a16_bytes, size); }
    if (getenv("DM_POOL_LOG")) LOG("[headless] pool %llu at %p, %#llx bytes (storage %s16 KiB aligned, size %s)", (unsigned long long)n, mem, (unsigned long long)size, ((uintptr_t)mem & 0x3FFF) ? "not " : "", (size & 0x3FFF) ? "not a 16 KiB multiple" : "a 16 KiB multiple");
    if (n == 1 || !(n & (n - 1))) LOG("[headless] memory pools: %llu (%llu MiB); with 16 KiB aligned storage and size %llu (%llu MiB)", (unsigned long long)n, (unsigned long long)(atomic_load(&g_pool_bytes) >> 20),
                                     (unsigned long long)atomic_load(&g_pool_a16), (unsigned long long)(atomic_load(&g_pool_a16_bytes) >> 20));
}
void mtl_pool_del(const void *mem) { (void)mem; }
void mtl_alias_drop_pool(const void *pool) { (void)pool; }
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) { (void)addr; (void)len; *buf = NULL; *off = 0; return 0; }
int mtl_pso_prebuild_tool(void) { return 0; }
// Statistics: nothing measured
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = *draws = 0; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = *reuse = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = *store = *full = 0; }
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip) { *resolve = *reuse = *set = *skip = 0; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = *reuploads = 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = *pso = 0; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = *pso_ns = 0; }
void mtl_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none) { *draws = *made = *none = 0; }
void mtl_alloc_stats(uint64_t *w, uint64_t *g, int *n) { *w = *g = 0; *n = 0; }
void mtl_pso_line(char *out, size_t n) { snprintf(out, n, "none (headless)"); }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_pkg_edge_skips(void) { return 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
double mtl_gpu_ms(uint64_t f) { (void)f; return -1; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; *t0 = *t1 = 0; return 0; }
void mtl_ab_gpu(uint64_t f, double gpu_ms, double frame_ms) { (void)f; (void)gpu_ms; (void)frame_ms; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }

// ---- DM_CTL graphics dumps ----
void dm_gfx_shot_at(long k, const char *path) { (void)k; LOG("[RED] [headless] screenshot %s: no picture", path); }
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_texlist(int target) { (void)target; }
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; }

// ---- front end: front_sdl.c; the software keyboard has no window of its own yet ----
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap;
    return -1;   // front.h: no window, nothing shown (hle_swkbd.c then answers from DM_KBD or the initial text)
}

// ---- libSystem's private per-thread counters: not on Linux (the instruction counts read 0) ----
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; memset(buf, 0, nbytes); return -1; }
