// Linux build: the NVN command replay on Vulkan, behind the mtl_* calls hle_nvn.c makes (nvn_obj.h). Step 3.2 of the backend (LINUX.md): the skeleton.
//   - one command buffer per submission (mtl_sub_begin … mtl_sub_commit), signalling a timeline semaphore with the submission's fence value, so mtl_wait and
//     mtl_finish wait for the GPU itself
//   - textures as VkImages, made the first time a texture is a render target or presented, found by the texture's serial (the game copies tex_t by value)
//   - the record stream walked as nvn_mtl.m walks it (END returns from a CALL or ends the handle, JUMP follows a segment, CALL descends); set render targets and the
//     colour / depth-stencil clears are carried out, every other record is counted (DM_VK_OPS=1 logs the counts)
//   - mtl_present hands the presented texture to the window (vk_window.c), which copies it once the GPU has finished the submissions before it
// Images stay in VK_IMAGE_LAYOUT_GENERAL and each clear is followed by a full memory barrier: correct and simple, fast enough for a skeleton.
// Without a device (DM_VK=0) every call behaves as the headless backend did: fences complete at once.
#include "dm.h"
#include "vk_dev.h"
#include "vk_fmt.h"
#include "vk_window.h"
#include "../gfx/nvn/nvn_obj.h"
#include <stdatomic.h>

#define CB_RING 8
typedef struct vtex {
    uint64_t serial; struct vtex *next;
    VkImage img; VkDeviceMemory mem; VkFormat fmt; VkImageAspectFlags aspect;
    uint32_t w, h, layers, levels, samples; int fresh;   // fresh: still VK_IMAGE_LAYOUT_UNDEFINED
    uint64_t release_after;   // released: destroyed once the timeline passes this (0 = live)
} vtex;
static pthread_mutex_t g_tmu = PTHREAD_MUTEX_INITIALIZER;
static vtex *g_tab[4096]; static vtex *g_dead;
static VkSemaphore g_tl; static _Atomic uint64_t g_fence;
static VkCommandPool g_pool; static VkCommandBuffer g_cb[CB_RING]; static uint64_t g_cb_val[CB_RING]; static int g_cb_i = -1, g_open;
static int g_on = -1;
static const tex_t *g_rt[8], *g_rt_depth; static view_t g_rt_view[8], g_rt_dview; static uint8_t g_rt_has_view[8], g_rt_has_dview; static int g_rt_n;
static uint64_t g_ops[64], g_clears, g_subs;

static int vkr_on(void) {
    if (g_on >= 0) return g_on;
    if (!vkd_ok()) return g_on = 0;
    VkSemaphoreTypeCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vkd.qf};
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = CB_RING};
    if (vkCreateSemaphore(g_vkd.dev, &si, NULL, &g_tl) || vkCreateCommandPool(g_vkd.dev, &pci, NULL, &g_pool)) { LOG("[RED] vulkan replay: no timeline semaphore or command pool"); return g_on = 0; }
    ai.commandPool = g_pool;
    if (vkAllocateCommandBuffers(g_vkd.dev, &ai, g_cb)) { LOG("[RED] vulkan replay: no command buffers"); return g_on = 0; }
    LOG("vulkan replay: on (skeleton: render targets and clears; draws are counted, not drawn)");
    return g_on = 1;
}
VkSemaphore vkr_timeline(void) { return g_tl; }
static uint64_t done_value(void) { uint64_t v = 0; vkGetSemaphoreCounterValue(g_vkd.dev, g_tl, &v); return v; }
static int wait_value(uint64_t v, uint64_t timeout_ns) {
    if (!v) return 1;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_tl, .pValues = &v};
    VkResult r = vkWaitSemaphores(g_vkd.dev, &wi, timeout_ns);
    if (r == VK_TIMEOUT) return 0;
    if (r) { LOG("[RED] vulkan replay: vkWaitSemaphores failed (%d)", (int)r); return 1; }
    return 1;
}

// ---- textures ----
static vtex *tex_find(uint64_t serial) { for (vtex *t = g_tab[serial % 4096]; t; t = t->next) if (t->serial == serial) return t; return NULL; }
static void reap(void) {   // images released once the GPU is past their last use (under g_tmu)
    uint64_t done = done_value();
    for (vtex **p = &g_dead; *p;) {
        vtex *t = *p;
        if (t->release_after <= done) { *p = t->next; vkDestroyImage(g_vkd.dev, t->img, NULL); vkFreeMemory(g_vkd.dev, t->mem, NULL); free(t); }
        else p = &t->next;
    }
}
// The image for a texture, made the first time it is needed (under g_tmu). NULL for what the skeleton does not make (logged once per kind)
static vtex *tex_get(const tex_t *o) {
    if (!o || !o->serial) return NULL;
    vtex *t = tex_find(o->serial); if (t) return t;
    const nvn_texdesc *d = &o->d;
    if (d->target != NVN_TEXTURE_TARGET_2D && d->target != NVN_TEXTURE_TARGET_2D_ARRAY && d->target != NVN_TEXTURE_TARGET_2D_MULTISAMPLE && d->target != NVN_TEXTURE_TARGET_RECTANGLE) {
        static int warned[16]; if (d->target >= 0 && d->target < 16 && !warned[d->target]++) LOG("[RED] vulkan replay: texture target %d is not made yet (serial %llu)", d->target, (unsigned long long)o->serial);
        return NULL;
    }
    VkFormat f = vkf_from_nvn(d->format, NULL, NULL);
    if (!f) { static int warned[256]; if (!warned[d->format & 255]++) LOG("[RED] vulkan replay: NVN format %#x has no Vulkan format here", d->format); return NULL; }
    VkImageAspectFlags asp = vkf_aspect(f);
    uint32_t samples = d->samples > 1 ? (uint32_t)d->samples : 1, layers = d->target == NVN_TEXTURE_TARGET_2D_ARRAY ? (d->depth ? d->depth : 1) : 1;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {d->width ? d->width : 1, d->height ? d->height : 1, 1},
        .mipLevels = d->levels ? d->levels : 1, .arrayLayers = layers, .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 (asp & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    t = calloc(1, sizeof *t);
    VkResult r = vkCreateImage(g_vkd.dev, &ci, NULL, &t->img);
    if (r) { LOG("[RED] vulkan replay: image %ux%u format %d (NVN %#x) not made (%d)", ci.extent.width, ci.extent.height, (int)f, d->format, (int)r); free(t); return NULL; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_vkd.dev, t->img, &mr);
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < g_vkd.mem.memoryTypeCount && mt == UINT32_MAX; i++) if ((mr.memoryTypeBits >> i & 1) && (g_vkd.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) mt = i;
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
    if (mt == UINT32_MAX || vkAllocateMemory(g_vkd.dev, &ma, NULL, &t->mem) || vkBindImageMemory(g_vkd.dev, t->img, t->mem, 0)) {
        LOG("[RED] vulkan replay: no memory for image %ux%u", ci.extent.width, ci.extent.height); vkDestroyImage(g_vkd.dev, t->img, NULL); free(t); return NULL; }
    t->serial = o->serial; t->fmt = f; t->aspect = asp; t->w = ci.extent.width; t->h = ci.extent.height; t->layers = layers; t->levels = ci.mipLevels; t->samples = samples; t->fresh = 1;
    t->next = g_tab[o->serial % 4096]; g_tab[o->serial % 4096] = t;
    static uint64_t n; n++; if (!(n & (n - 1))) LOG("vulkan replay: %llu images made (latest %ux%u format %d, NVN %#x, %u layers, %u levels)", (unsigned long long)n, t->w, t->h, (int)f, d->format, layers, t->levels);
    return t;
}
// Into VK_IMAGE_LAYOUT_GENERAL the first time it is used (under g_tmu)
static void tex_ready(VkCommandBuffer c, vtex *t) {
    if (!t->fresh) return;
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = t->img, .subresourceRange = {t->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    t->fresh = 0;
}
// For the window's present thread: the image of a presented texture, made ready in c if new
int vkr_present_image(const tex_t *o, VkCommandBuffer c, VkImage *img, uint32_t *w, uint32_t *h, VkFormat *fmt) {
    pthread_mutex_lock(&g_tmu);
    vtex *t = tex_get(o); if (t) { tex_ready(c, t); *img = t->img; *w = t->w; *h = t->h; *fmt = t->fmt; }
    pthread_mutex_unlock(&g_tmu);
    return t != NULL;
}
void mtl_tex_init(const tex_t *t) { (void)t; }
void mtl_tex_release(tex_t *o) {
    if (!g_on || !o->serial) return;
    pthread_mutex_lock(&g_tmu);
    vtex **p = &g_tab[o->serial % 4096];
    while (*p && (*p)->serial != o->serial) p = &(*p)->next;
    if (*p) { vtex *t = *p; *p = t->next; t->release_after = atomic_load(&g_fence) + 1; t->next = g_dead; g_dead = t; }   // the submission in progress may use it too
    pthread_mutex_unlock(&g_tmu);
}

// ---- submissions ----
void mtl_sub_begin(void) {
    if (!vkr_on()) return;
    g_cb_i = (g_cb_i + 1) % CB_RING;
    wait_value(g_cb_val[g_cb_i], UINT64_MAX);   // its last use is done
    vkResetCommandBuffer(g_cb[g_cb_i], 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(g_cb[g_cb_i], &bi); g_open = 1;
}
uint64_t mtl_sub_commit(void) {
    if (!g_on) return atomic_fetch_add(&g_fence, 1) + 1;
    VkCommandBuffer c = g_cb[g_cb_i]; vkEndCommandBuffer(c); g_open = 0;
    uint64_t v = atomic_load(&g_fence) + 1;
    VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &v};
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts, .commandBufferCount = 1, .pCommandBuffers = &c, .signalSemaphoreCount = 1, .pSignalSemaphores = &g_tl};
    pthread_mutex_lock(&g_vkd_qlock); VkResult r = vkQueueSubmit(g_vkd.q, 1, &si, VK_NULL_HANDLE); pthread_mutex_unlock(&g_vkd_qlock);
    if (r) die("vulkan replay: submit failed (%d)", (int)r);
    g_cb_val[g_cb_i] = v; atomic_store(&g_fence, v); g_subs++;
    if (getenv("DM_VK_OPS") && !(g_subs & (g_subs - 1))) {
        char b[600]; int q = 0;
        for (int i = 0; i < 64 && q < (int)sizeof b - 24; i++) if (g_ops[i]) q += snprintf(b + q, sizeof b - q, " %d:%llu", i, (unsigned long long)g_ops[i]);
        LOG("vulkan replay: %llu submissions, %llu clears; records by op:%s", (unsigned long long)g_subs, (unsigned long long)g_clears, b);
    }
    pthread_mutex_lock(&g_tmu); reap(); pthread_mutex_unlock(&g_tmu);
    return v;
}
uint64_t mtl_last_fence(void) { return atomic_load(&g_fence); }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) { return g_on > 0 ? wait_value(fence, timeout_ns) : 1; }
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { return mtl_wait(fence, timeout_ns); }
void mtl_finish(void) { if (g_on > 0) wait_value(atomic_load(&g_fence), UINT64_MAX); }
void mtl_drain(void) { mtl_finish(); }

// ---- the record stream ----
static void full_barrier(VkCommandBuffer c) {
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static VkImageSubresourceRange view_range(const vtex *t, const view_t *v, int has, VkImageAspectFlags asp) {
    VkImageSubresourceRange r = {asp, 0, 1, 0, t->layers};
    if (has && v->set_levels) { r.baseMipLevel = v->base_level; r.levelCount = v->nlevels ? v->nlevels : 1; }
    if (has && v->set_layers) { r.baseArrayLayer = v->min_layer; r.layerCount = v->nlayers ? v->nlayers : 1; }
    if (r.baseMipLevel >= t->levels) r.baseMipLevel = t->levels - 1;
    if (r.baseMipLevel + r.levelCount > t->levels) r.levelCount = t->levels - r.baseMipLevel;
    if (r.baseArrayLayer >= t->layers) r.baseArrayLayer = t->layers - 1;
    if (r.baseArrayLayer + r.layerCount > t->layers) r.layerCount = t->layers - r.baseArrayLayer;
    return r;
}
static void op_clear_color(VkCommandBuffer c, const p_clear_color *p) {
    if (p->index < 0 || p->index >= g_rt_n || !g_rt[p->index]) return;
    pthread_mutex_lock(&g_tmu);
    vtex *t = tex_get(g_rt[p->index]);
    if (t && (t->aspect & VK_IMAGE_ASPECT_COLOR_BIT) && t->samples == 1) {
        tex_ready(c, t);
        VkClearColorValue v; memcpy(v.float32, p->rgba, sizeof v.float32);   // a float clear; integer targets take the same bits (open: NVN's integer clears)
        VkImageSubresourceRange r = view_range(t, &g_rt_view[p->index], g_rt_has_view[p->index], VK_IMAGE_ASPECT_COLOR_BIT);
        vkCmdClearColorImage(c, t->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r); full_barrier(c); g_clears++;
    }
    pthread_mutex_unlock(&g_tmu);
}
static void op_clear_ds(VkCommandBuffer c, const p_clear_ds *p) {
    if (!g_rt_depth) return;
    pthread_mutex_lock(&g_tmu);
    vtex *t = tex_get(g_rt_depth);
    if (t && !(t->aspect & VK_IMAGE_ASPECT_COLOR_BIT) && t->samples == 1) {
        VkImageAspectFlags asp = 0;
        if ((t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && p->depth_mask) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ((t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && p->stencil_mask) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (asp) {
            tex_ready(c, t);
            VkClearDepthStencilValue v = {p->depth < 0 ? 0 : p->depth > 1 ? 1 : p->depth, (uint32_t)p->stencil};
            VkImageSubresourceRange r = view_range(t, &g_rt_dview, g_rt_has_dview, asp);
            vkCmdClearDepthStencilImage(c, t->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r); full_barrier(c); g_clears++;
        }
    }
    pthread_mutex_unlock(&g_tmu);
}
void mtl_exec(const uint8_t *handle) {
    if (!g_on || !g_open) return;
    VkCommandBuffer c = g_cb[g_cb_i];
    const uint8_t *p = handle, *ret[16]; int nret = 0;
    for (uint64_t guard = 0; p && guard < 50000000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        if (h.op < 64) g_ops[h.op]++;
        switch (h.op) {
        case OP_END: if (nret) { p = ret[--nret]; continue; } return;
        case OP_JUMP: memcpy(&p, a, 8); continue;
        case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); if (nret == 16) { LOG("[RED] vulkan replay: command buffer calls nested deeper than 16"); return; } ret[nret++] = p + h.size; p = t; continue; }
        case OP_SET_RT: {
            const p_set_rt *s = a; g_rt_n = s->n < 0 ? 0 : s->n > 8 ? 8 : s->n;
            for (int i = 0; i < 8; i++) { g_rt[i] = i < g_rt_n ? s->color[i] : NULL; g_rt_view[i] = s->cview[i]; g_rt_has_view[i] = s->has_cview[i]; }
            g_rt_depth = s->depth; g_rt_dview = s->dview; g_rt_has_dview = s->has_dview;
            break;
        }
        case OP_CLEAR_COLOR: op_clear_color(c, a); break;
        case OP_CLEAR_DS: op_clear_ds(c, a); break;
        default: break;   // counted only, for now
        }
        if (h.size < 8) { LOG("[RED] vulkan replay: record of op %u with size %u: the stream is broken", h.op, h.size); return; }
        p += h.size;
    }
}
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }

// ---- present ----
static void shot_check(const tex_t *t);
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    shot_check(t);
    if (vkw_active()) return vkw_present(g_on > 0 ? t : NULL, g_on > 0 ? atomic_load(&g_fence) : 0, crop, period_s);
    static _Atomic uint64_t n; uint64_t k = atomic_fetch_add(&n, 1) + 1;
    if (k == 1 || !(k & (k - 1))) LOG("[headless] present %llu (no window: frames are not drawn)", (unsigned long long)k);
    return k;
}

// ---- screenshots: DM_CTL "shot <path>" (dm_gfx_shot_at) and DM_SHOT=<present number>:<path> ----
// The presented texture is blitted to an RGBA8 image (any colour format converts), copied to host memory once the frame's submissions are done, and written as a PNG
// (stored deflate blocks: no compressor needed)
static uint32_t crc_tab[256];
static uint32_t crc32b(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1; crc_tab[i] = k; }
    c = ~c; while (n--) c = crc_tab[(c ^ *p++) & 255] ^ (c >> 8); return ~c;
}
static void put32(FILE *f, uint32_t v) { uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v}; fwrite(b, 1, 4, f); }
static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
    put32(f, n); fwrite(type, 1, 4, f); if (n) fwrite(d, 1, n, f);
    uint32_t c = crc32b(0, (const uint8_t *)type, 4); c = crc32b(c, d, n); put32(f, c);
}
static int write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    size_t raw = (size_t)h * (1 + 4 * (size_t)w), nblk = (raw + 65534) / 65535, zn = 2 + raw + 5 * nblk + 4;
    uint8_t *z = malloc(zn), *q = z; if (!z) return 0;
    *q++ = 0x78; *q++ = 0x01;
    uint32_t a = 1, b = 0; size_t left = raw, row = 1 + 4 * (size_t)w, pos = 0;
    while (left) {
        uint16_t n = left > 65535 ? 65535 : (uint16_t)left; left -= n;
        *q++ = left ? 0 : 1; *q++ = n & 255; *q++ = n >> 8; *q++ = ~n & 255; *q++ = (uint16_t)~n >> 8;
        for (uint16_t i = 0; i < n; i++, pos++) {
            size_t y = pos / row, x = pos % row; uint8_t v = x ? rgba[y * 4 * w + x - 1] : 0;   // filter byte 0 at each row start
            *q++ = v; a = (a + v) % 65521; b = (b + a) % 65521;
        }
    }
    *q++ = b >> 8; *q++ = b; *q++ = a >> 8; *q++ = a;
    FILE *f = fopen(path, "wb"); if (!f) { free(z); return 0; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 6, 0, 0, 0};
    chunk(f, "IHDR", ih, 13); chunk(f, "IDAT", z, (uint32_t)(q - z)); chunk(f, "IEND", NULL, 0);
    free(z); return !fclose(f);
}
static uint32_t pick_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_vkd.mem.memoryTypeCount; i++) if ((bits >> i & 1) && (g_vkd.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
static void shoot(const tex_t *o, uint64_t fence, const char *path) {
    pthread_mutex_lock(&g_tmu); vtex *t = tex_get(o); pthread_mutex_unlock(&g_tmu);
    if (!t || !(t->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) { LOG("[RED] screenshot %s: the presented texture has no colour image", path); return; }
    uint32_t w = t->w, h = t->h; VkDevice d = g_vkd.dev;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1,
        .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkImage im; vkCreateImage(d, &ci, NULL, &im); VkMemoryRequirements mr; vkGetImageMemoryRequirements(d, im, &mr);
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = pick_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory imm; vkAllocateMemory(d, &ma, NULL, &imm); vkBindImageMemory(d, im, imm, 0);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buf; vkCreateBuffer(d, &bi, NULL, &buf); vkGetBufferMemoryRequirements(d, buf, &mr);
    ma.allocationSize = mr.size; ma.memoryTypeIndex = pick_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    VkDeviceMemory bm; vkAllocateMemory(d, &ma, NULL, &bm); vkBindBufferMemory(d, buf, bm, 0);
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer c; vkAllocateCommandBuffers(d, &ai, &c);
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; vkBeginCommandBuffer(c, &cbi);
    pthread_mutex_lock(&g_tmu); tex_ready(c, t); pthread_mutex_unlock(&g_tmu);
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
    vkCmdBlitImage(c, t->img, VK_IMAGE_LAYOUT_GENERAL, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(c, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bc);
    VkMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(c);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &fence};
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts, .waitSemaphoreCount = fence ? 1 : 0, .pWaitSemaphores = &g_tl, .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &c};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fe; vkCreateFence(d, &fci, NULL, &fe);
    pthread_mutex_lock(&g_vkd_qlock); VkResult r = vkQueueSubmit(g_vkd.q, 1, &si, fe); pthread_mutex_unlock(&g_vkd_qlock);
    if (!r) vkWaitForFences(d, 1, &fe, VK_TRUE, UINT64_MAX);
    void *px = NULL; vkMapMemory(d, bm, 0, VK_WHOLE_SIZE, 0, &px);
    int ok = !r && px && write_png(path, px, w, h);
    LOG("%sscreenshot %s: %ux%u (image format %d)%s", ok ? "" : "[RED] ", path, w, h, (int)t->fmt, ok ? "" : " not written");
    vkUnmapMemory(d, bm); vkDestroyFence(d, fe, NULL); vkFreeCommandBuffers(d, g_pool, 1, &c);
    vkDestroyBuffer(d, buf, NULL); vkFreeMemory(d, bm, NULL); vkDestroyImage(d, im, NULL); vkFreeMemory(d, imm, NULL);
}
static pthread_mutex_t g_shot_mu = PTHREAD_MUTEX_INITIALIZER; static long g_shot_k; static char g_shot_path[1024]; static uint64_t g_presents;
void dm_gfx_shot_at(long k, const char *path) { pthread_mutex_lock(&g_shot_mu); g_shot_k = (long)g_presents + k; snprintf(g_shot_path, sizeof g_shot_path, "%s", path); pthread_mutex_unlock(&g_shot_mu); }
static void shot_check(const tex_t *t) {
    static int env = 0;
    if (!env) { env = 1; const char *e = getenv("DM_SHOT"); char *c; long k; if (e && (k = strtol(e, &c, 10)) > 0 && *c == ':') dm_gfx_shot_at(k, c + 1); }
    pthread_mutex_lock(&g_shot_mu);
    g_presents++;
    int now = g_shot_k && (long)g_presents >= g_shot_k; char path[1024]; if (now) { snprintf(path, sizeof path, "%s", g_shot_path); g_shot_k = 0; }
    pthread_mutex_unlock(&g_shot_mu);
    if (now && g_on > 0) shoot(t, atomic_load(&g_fence), path);
}
