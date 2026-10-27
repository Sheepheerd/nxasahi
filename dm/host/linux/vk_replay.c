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
#include "vk_draw.h"
#include "vk_mem.h"
int nvn_dpool_base(const void *pool, int tex);   // hle_nvn.c: a descriptor pool's ID base
#include "../gfx/nvn/nvn_obj.h"
#include "../gfx/nvn/nvn_dirty.h"
#include "../gfx/tex/nvn_deswizzle.h"
#include <stdatomic.h>

#define CB_RING 16   // command buffers in flight: a submission is flushed in parts (vkr_flush), so several per frame
#include "vk_replay.h"
pthread_mutex_t g_tmu = PTHREAD_MUTEX_INITIALIZER;
// Images are shared by storage, as nvn_mtl.m's sharing table does: an NVN texture object is only a descriptor, and an object rebuilt over the same storage with the same description
// is the same texture. Mario Wonder rebuilds its transient render targets (and the cube maps its lighting samples) on new objects every frame: one image per object made 74,546
// images in two minutes over 447 storages, and each new one started empty, so what the GPU had rendered into the old one was lost (black lit models). Serials map to images
// (g_tab); images are found by storage (g_stab) and stay while no object refers to them, until a texture of another description takes over their storage
typedef struct sref { uint64_t serial; vtex *t; struct sref *next; } sref;
static sref *g_tab[4096]; static vtex *g_stab[4096]; static vtex *g_dead;
static uint32_t stor_hash(const uint8_t *mem, const nvn_texdesc *d) { uint64_t h = (uint64_t)(uintptr_t)mem >> 6; h ^= (uint64_t)d->width * 31 + d->height * 131 + (uint64_t)d->format * 7919 + (uint64_t)d->target * 104729; return (uint32_t)(h ^ h >> 13) % 4096; }
static VkSemaphore g_tl; static _Atomic uint64_t g_fence;
static VkCommandPool g_pool; static VkCommandBuffer g_cb[CB_RING]; static uint64_t g_cb_val[CB_RING]; static int g_cb_i = -1, g_open;
static int g_on = -1;
static const tex_t *g_rt[8], *g_rt_depth; static view_t g_rt_view[8], g_rt_dview; static uint8_t g_rt_has_view[8], g_rt_has_dview; static int g_rt_n;
static uint64_t g_ops[64], g_clears, g_subs; static uint64_t g_live;
static VkCommandBuffer g_up[CB_RING]; static int g_up_open;   // the uploads of the submission in progress (loading textures)
static char g_dump_dir[900]; static int g_dump_left = -1; static uint64_t g_dump_ser[64]; static int g_ndump;   // DM_VK_TEXDUMP

// GPU time per frame (mtl_gpu_ms, the long-frame log and DM_NVN_TSV's @gpu_ms): a timestamp at the start and end of each submission, summed into the frame the game was
// recording when it began. Read once the submission is done, by the next use of its ring slot or by mtl_gpu_ms
static VkQueryPool g_qp; static uint64_t g_cb_frame[CB_RING]; static int g_cb_timed[CB_RING];
typedef struct { uint64_t f, ns, t0, t1; } gpu_frame;
static gpu_frame g_gf[64]; static pthread_mutex_t g_gfmu = PTHREAD_MUTEX_INITIALIZER;
uint64_t vkh_frame(void); static uint64_t done_value(void);
static uint64_t mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static void gpu_harvest(int i) {   // slot i's timestamps into its frame (its submission is done)
    if (!g_cb_timed[i]) return;
    uint64_t q[2]; if (vkGetQueryPoolResults(g_vkd.dev, g_qp, (uint32_t)(2 * i), 2, sizeof q, q, 8, VK_QUERY_RESULT_64_BIT)) return;
    g_cb_timed[i] = 0;
    double per = g_vkd.props.limits.timestampPeriod; uint64_t ns = q[1] > q[0] ? (uint64_t)((q[1] - q[0]) * per) : 0;
    pthread_mutex_lock(&g_gfmu);
    gpu_frame *g = &g_gf[g_cb_frame[i] % 64];
    if (g->f != g_cb_frame[i]) *g = (gpu_frame){g_cb_frame[i], 0, UINT64_MAX, 0};
    g->ns += ns; if ((uint64_t)(q[0] * per) < g->t0) g->t0 = (uint64_t)(q[0] * per); if ((uint64_t)(q[1] * per) > g->t1) g->t1 = (uint64_t)(q[1] * per);
    pthread_mutex_unlock(&g_gfmu);
}
static void gpu_harvest_done(void) { uint64_t d = done_value(); for (int i = 0; i < CB_RING; i++) if (g_cb_timed[i] && g_cb_val[i] && g_cb_val[i] <= d) gpu_harvest(i); }
double mtl_gpu_ms(uint64_t f) {
    if (g_on <= 0 || !g_qp) return -1;
    pthread_mutex_lock(&g_tmu); gpu_harvest_done(); pthread_mutex_unlock(&g_tmu);
    pthread_mutex_lock(&g_gfmu); double r = g_gf[f % 64].f == f ? g_gf[f % 64].ns / 1e6 : -1; pthread_mutex_unlock(&g_gfmu);
    return r;
}
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; *t0 = *t1 = 0; return 0; }   // (GPU clock, not the host's: not given)
static int vkr_on(void) {
    if (g_on >= 0) return g_on;
    if (!vkd_ok()) return g_on = 0;
    VkSemaphoreTypeCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vkd.qf};
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = CB_RING};
    if (vkCreateSemaphore(g_vkd.dev, &si, NULL, &g_tl) || vkCreateCommandPool(g_vkd.dev, &pci, NULL, &g_pool)) { LOG("[RED] vulkan replay: no timeline semaphore or command pool"); return g_on = 0; }
    ai.commandPool = g_pool;
    if (vkAllocateCommandBuffers(g_vkd.dev, &ai, g_cb) || vkAllocateCommandBuffers(g_vkd.dev, &ai, g_up)) { LOG("[RED] vulkan replay: no command buffers"); return g_on = 0; }
    VkQueryPoolCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * CB_RING};
    if (!g_vkd.props.limits.timestampComputeAndGraphics || vkCreateQueryPool(g_vkd.dev, &qci, NULL, &g_qp)) { g_qp = VK_NULL_HANDLE; LOG("vulkan replay: no GPU timestamps (GPU time is not measured)"); }
    // DM_VK_TEXDUMP=<dir>[:n]: the first n textures loaded from guest memory (16 by default, at most 64) as PNG files in dir, level 0 and layer 0
    const char *td = getenv("DM_VK_TEXDUMP");
    if (td && *td) { snprintf(g_dump_dir, sizeof g_dump_dir, "%s", td); char *cn = strrchr(g_dump_dir, ':'); g_dump_left = 16; if (cn) { g_dump_left = atoi(cn + 1); *cn = 0; } if (g_dump_left > 64) g_dump_left = 64; }
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
static void tex_ready(VkCommandBuffer c, vtex *t);
static VkCommandBuffer up_cb(void);
static void rp_end(VkCommandBuffer c);
static vtex *tex_find(uint64_t serial) { for (sref *r = g_tab[serial % 4096]; r; r = r->next) if (r->serial == serial) return r->t; return NULL; }
static void sref_add(uint64_t serial, vtex *t) { sref *r = calloc(1, sizeof *r); r->serial = serial; r->t = t; r->next = g_tab[serial % 4096]; g_tab[serial % 4096] = r; t->refs++; }
static void stor_unlink(vtex *t) { for (vtex **p = &g_stab[t->khash]; *p; p = &(*p)->next) if (*p == t) { *p = t->next; return; } }
static void tex_retire(vtex *t) { t->release_after = atomic_load(&g_fence) + 1; t->next = g_dead; g_dead = t; }   // the submission in progress may use it too
static uint64_t g_shared_hits, g_taken;
static void reap(void) {   // images released once the GPU is past their last use (under g_tmu)
    uint64_t done = done_value();
    for (vtex **p = &g_dead; *p;) {
        vtex *t = *p;
        if (t->release_after <= done) { *p = t->next; for (vkview *v = t->views, *nv; v; v = nv) { nv = v->next; vkDestroyImageView(g_vkd.dev, v->view, NULL); free(v); } vkDestroyImage(g_vkd.dev, t->img, NULL); vkFreeMemory(g_vkd.dev, t->mem, NULL); free(t); g_live--; }
        else p = &t->next;
    }
}
// The image for a texture, made the first time it is needed (under g_tmu). NULL for what is not made (logged once per kind). Layers as nvn_mtl.m counts them: an array's and a
// cube array's depth (a cube array's counts layer-faces), 6 for a cube, a 1D array's height; 1D textures are one-row 2D images
static vtex *tex_get(const tex_t *o) {
    if (!o || !o->serial) return NULL;
    vtex *t = tex_find(o->serial); if (t) return t;
    const nvn_texdesc *d = &o->d;
    if (d->target == NVN_TEXTURE_TARGET_BUFFER || d->target < 0 || d->target > NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) {
        static int warned[16]; if (d->target >= 0 && d->target < 16 && !warned[d->target]++) LOG("[RED] vulkan replay: texture target %d is not an image (serial %llu)", d->target, (unsigned long long)o->serial);
        return NULL;
    }
    VkFormat f = vkf_from_nvn(d->format, NULL, NULL);
    if (!f) { static int warned[256]; if (!warned[d->format & 255]++) LOG("[RED] vulkan replay: NVN format %#x has no Vulkan format here", d->format); return NULL; }
    VkImageAspectFlags asp = vkf_aspect(f);
    uint32_t kh = stor_hash(o->mem, d);
    if (o->mem) for (vtex *s = g_stab[kh]; s; s = s->next) if (s->kmem == o->mem && !memcmp(&s->kd, d, sizeof *d)) { sref_add(o->serial, s); g_shared_hits++; return s; }
    int tg = d->target;
    uint32_t layers = tg == NVN_TEXTURE_TARGET_2D_ARRAY || tg == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY || tg == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY ? (d->depth ? d->depth : 1)
                    : tg == NVN_TEXTURE_TARGET_CUBEMAP ? 6 : tg == NVN_TEXTURE_TARGET_1D_ARRAY ? (d->height ? d->height : 1) : 1;
    uint32_t h = tg == NVN_TEXTURE_TARGET_1D || tg == NVN_TEXTURE_TARGET_1D_ARRAY ? 1 : (d->height ? d->height : 1);
    uint32_t depth = tg == NVN_TEXTURE_TARGET_3D ? (d->depth ? d->depth : 1) : 1;
    uint32_t samples = d->samples > 1 ? (uint32_t)d->samples : 1;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vkd.pd, f, &fp);
    VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
    VkImageUsageFlags use = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) use |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) use |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) use |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && samples == 1) use |= VK_IMAGE_USAGE_STORAGE_BIT;
    // a 3D image that can be a render target is rendered slice by slice through 2D-array views of it (2D_ARRAY_COMPATIBLE)
    VkImageCreateFlags cf = (tg == NVN_TEXTURE_TARGET_CUBEMAP || tg == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) && layers % 6 == 0 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    if (tg == NVN_TEXTURE_TARGET_3D && (use & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) cf |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cf,
        .imageType = tg == NVN_TEXTURE_TARGET_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, .format = f, .extent = {d->width ? d->width : 1, h, depth},
        .mipLevels = d->levels ? d->levels : 1, .arrayLayers = layers, .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = use, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    t = calloc(1, sizeof *t);
    VkResult r = vkCreateImage(g_vkd.dev, &ci, NULL, &t->img);
    if (r) { LOG("[RED] vulkan replay: image %ux%ux%u format %d (NVN %#x, target %d) not made (%d)", ci.extent.width, h, depth, (int)f, d->format, tg, (int)r); free(t); return NULL; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_vkd.dev, t->img, &mr);
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < g_vkd.mem.memoryTypeCount && mt == UINT32_MAX; i++) if ((mr.memoryTypeBits >> i & 1) && (g_vkd.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) mt = i;
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
    if (mt == UINT32_MAX || vkAllocateMemory(g_vkd.dev, &ma, NULL, &t->mem) || vkBindImageMemory(g_vkd.dev, t->img, t->mem, 0)) {
        LOG("[RED] vulkan replay: no memory for image %ux%u", ci.extent.width, h); vkDestroyImage(g_vkd.dev, t->img, NULL); free(t); return NULL; }
    t->serial = o->serial; t->fmt = f; t->aspect = asp; t->w = ci.extent.width; t->h = h; t->d = depth; t->layers = layers; t->levels = ci.mipLevels; t->samples = samples;
    t->target = tg; t->usage = use; t->fresh = 1;
    g_live++;
    t->kmem = o->mem; t->kend = o->mem ? o->mem + (o->L.size ? o->L.size : 1) : NULL; t->kd = *d; t->khash = kh;
    if (o->mem) {   // images no object refers to whose storage this one overlaps: taken over (their content is no longer the storage's)
        for (int i = 0; i < 4096; i++) for (vtex **p = &g_stab[i]; *p;) { vtex *s = *p;
            if (!s->refs && s->kmem < t->kend && t->kmem < s->kend) { *p = s->next; tex_retire(s); g_taken++; } else p = &s->next; }
    }
    t->next = g_stab[kh]; g_stab[kh] = t;
    sref_add(o->serial, t);
    if (g_open) tex_ready(up_cb(), t);   // into GENERAL before the submission runs: never inside a render pass
    { static int tl = -1; if (tl < 0) tl = getenv("DM_VK_TEXLOG") != NULL;   // diagnostic: each image made, with its guest storage
      if (tl) LOG("[tex] serial %llu target %d %ux%ux%u layers %u levels %u NVN %#x storage %p..%p (layer %llu bytes) pool %p off %lld", (unsigned long long)o->serial, tg, t->w, t->h, depth, layers, t->levels, d->format,
                  (void *)o->mem, (void *)(o->mem + o->L.size), (unsigned long long)o->L.layer_size, (void *)o->pool, (long long)o->off); }
    static uint64_t n; n++; if (!(n & (n - 1))) LOG("vulkan replay: %llu images made, %llu live, %llu texture objects given a shared one, %llu taken over (latest %ux%ux%u format %d, NVN %#x, target %d, %u layers, %u levels)", (unsigned long long)n, (unsigned long long)g_live, (unsigned long long)g_shared_hits, (unsigned long long)g_taken,
                                                   t->w, t->h, depth, (int)f, d->format, tg, layers, t->levels);
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

// ---- loading textures from guest memory ----
// A texture the GPU has not written is loaded from its storage the first time it is sampled, and again when the game has written the storage since (nvn_dirty.c write-protects
// it after each load). The block-linear storage is deswizzled on the CPU, level by level and layer by layer, into a staging buffer, and copied to the image by the submission's
// upload command buffer, which goes to the queue right before the submission's own (that one starts with a full barrier). Staging buffers are freed once the GPU is past them
typedef struct stage_buf { VkBuffer buf; VkDeviceMemory mem; uint64_t after; struct stage_buf *next; } stage_buf;
static stage_buf *g_stage;   // waiting to be freed (under g_tmu)
static uint64_t g_uploads, g_reuploads, g_up_bytes;
static uint32_t pick_mem(uint32_t bits, VkMemoryPropertyFlags want);
static VkCommandBuffer up_cb(void) {   // the upload command buffer of the submission in progress, begun on first use
    if (!g_up_open) {
        vkResetCommandBuffer(g_up[g_cb_i], 0);
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(g_up[g_cb_i], &bi); g_up_open = 1;
    }
    return g_up[g_cb_i];
}
static _Atomic uint64_t g_up_ns;   // time spent loading textures (the long-frame log)
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = g_uploads; *reuploads = g_reuploads; }
uint64_t mtl_upload_ns_take(void) { return atomic_exchange(&g_up_ns, 0); }
uint64_t mtl_upload_bytes(void) { return g_up_bytes; }
static int upload_(vtex *t, const tex_t *o);
static int upload(vtex *t, const tex_t *o) { uint64_t t0 = mono_ns(); int r = upload_(t, o); atomic_fetch_add(&g_up_ns, mono_ns() - t0); return r; }
static int upload_(vtex *t, const tex_t *o) {   // under g_tmu, inside a submission
    const nvn_texdesc *d = &o->d;
    if (!(t->aspect & VK_IMAGE_ASPECT_COLOR_BIT) || t->samples > 1 || !o->mem) return 0;   // depth / stencil and multisampled content is the GPU's
    if (o->pool && o->pool->vp) { static int w; if (!w++) LOG("[RED] vulkan replay: a texture in a sparse pool is not loaded yet"); return 0; }
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    if (!bpb) { static int w[256]; if (!w[d->format & 255]++) LOG("[RED] vulkan replay: NVN format %#x has no block size: not loaded", d->format); return 0; }
    int linear = (d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) != 0;   // rows d->stride apart, as nvn_deswizzle_geom reads the flags
    size_t lstride = d->stride ? d->stride : 0;
    // A linear texture loaded before takes only the rows on pages the game wrote since (its rows map straight onto pages): a glyph cache that grows by a few rows a
    // frame copied whole was 16 MB a time (Mario Wonder's 2048x8192 R8 at about 4 a second)
    int partial = linear && t->uploaded && t->levels == 1 && t->layers == 1 && t->d == 1;
    uint64_t prev = t->stamp;
    size_t total = 0;
    for (uint32_t l = 0; l < t->levels; l++) {
        uint32_t w = t->w >> l ? t->w >> l : 1, h = t->h >> l ? t->h >> l : 1, z = t->d >> l ? t->d >> l : 1;
        total += (size_t)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) * z * t->layers;
    }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = total ? total : 16, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    stage_buf *s = calloc(1, sizeof *s);
    if (vkCreateBuffer(g_vkd.dev, &bi, NULL, &s->buf)) { free(s); return 0; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vkd.dev, s->buf, &mr);
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = pick_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    void *map = NULL;
    if (ma.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g_vkd.dev, &ma, NULL, &s->mem) || vkBindBufferMemory(g_vkd.dev, s->buf, s->mem, 0) || vkMapMemory(g_vkd.dev, s->mem, 0, VK_WHOLE_SIZE, 0, &map)) {
        LOG("[RED] vulkan replay: no staging memory for a %zu byte texture", total); vkDestroyBuffer(g_vkd.dev, s->buf, NULL); if (s->mem) vkFreeMemory(g_vkd.dev, s->mem, NULL); free(s); return 0; }
    t->stamp = nvn_dirty_now(); nvn_dirty_protect(o->mem, o->L.size);   // stamped before reading: a write during the copy makes it dirty again
    size_t pg = (size_t)getpagesize(), part_bytes = 0;
    VkCommandBuffer c = up_cb(); tex_ready(c, t);
    VkBufferImageCopy rg[256]; int nr = 0; size_t off = 0; int bad = 0;
    for (uint32_t l = 0; l < t->levels && !bad; l++) {
        uint32_t w = t->w >> l ? t->w >> l : 1, h = t->h >> l ? t->h >> l : 1, z = t->d >> l ? t->d >> l : 1;
        size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
        for (uint32_t y = 0; y < t->layers && !bad; y++) {
            uint8_t *dst = (uint8_t *)map + off;
            if (linear) {   // linear storage: rows d->stride apart (one level, one layer)
                if (l || y) { bad = 1; break; }
                size_t st = lstride ? lstride : pitch, grp = pg / st ? pg / st : 1;   // rows per page
                for (size_t r0 = 0; r0 < rows * z; r0 += grp) {
                    size_t n = rows * z - r0 < grp ? rows * z - r0 : grp;
                    if (partial && !nvn_dirty_since(o->mem + r0 * st, (n - 1) * st + pitch, prev)) continue;
                    for (size_t r = r0; r < r0 + n; r++) memcpy(dst + r * pitch, o->mem + r * st, pitch);
                    if (partial) {
                        if (nr == 256) { bad = 1; break; }
                        rg[nr++] = (VkBufferImageCopy){.bufferOffset = off + r0 * pitch, .bufferRowLength = (uint32_t)(pitch / bpb * bw), .bufferImageHeight = (uint32_t)(n * bh),
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {0, (int32_t)(r0 * bh), 0}, .imageExtent = {w, (uint32_t)(n * bh) < h - r0 * bh ? (uint32_t)(n * bh) : h - (uint32_t)(r0 * bh), 1}};
                        part_bytes += n * pitch;
                    }
                }
                if (partial) { off += pitch * rows * z; continue; }
            } else if (nvn_deswizzle(d, l, y, o->mem, dst, pitch)) {
                static int w2[256]; if (!w2[d->format & 255]++) LOG("[RED] vulkan replay: deswizzle failed (NVN format %#x, target %d)", d->format, d->target);
                bad = 1; break;
            }
            if (nr == 256) { bad = 1; break; }
            rg[nr++] = (VkBufferImageCopy){.bufferOffset = off, .bufferRowLength = (uint32_t)(pitch / bpb * bw), .bufferImageHeight = (uint32_t)(rows * bh),
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, t->d > 1 ? 0 : y, 1}, .imageExtent = {w, h, z}};
            off += pitch * rows * z;
        }
    }
    if (nr) vkCmdCopyBufferToImage(c, s->buf, t->img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)nr, rg);
    s->after = atomic_load(&g_fence) + 1; s->next = g_stage; g_stage = s;
    if (partial) off = part_bytes;
    if (t->uploaded) g_reuploads++; else g_uploads++;
    t->uploaded = 1; g_up_bytes += off;
    if (g_dump_left > 0 && g_ndump < 64 && !bad) { g_dump_ser[g_ndump++] = t->serial; g_dump_left--; }
    if (!((g_uploads + g_reuploads) & (g_uploads + g_reuploads - 1)))
        LOG("vulkan replay: textures loaded from guest memory: %llu, again after a write %llu, %llu MiB (latest %ux%u NVN %#x target %d, %u levels %u layers%s)", (unsigned long long)g_uploads,
            (unsigned long long)g_reuploads, (unsigned long long)(g_up_bytes >> 20), t->w, t->h, d->format, d->target, t->levels, t->layers, linear ? ", linear" : "");
    return !bad;
}
// A texture about to be sampled: its image, loaded from guest memory first if the GPU does not own its content and it was not loaded or was written since (under g_tmu)
static vtex *tex_sampled(const tex_t *o) {
    vtex *t = tex_get(o); if (!t) return NULL;
    if (t->gpu_owned) return t;
    // Every write to tracked memory moves the dirty clock: a texture found clean at clock N is still clean while the clock reads N. Checked page by page on every bind
    // (with a scan of the tracked ranges to find its own), this was 28% of the host's CPU time in Mario Wonder's levels
    uint64_t now = nvn_dirty_now();
    if (t->uploaded && t->clean_at == now) return t;
    if (!t->span) t->span = nvn_dirty_span(o->mem, o->L.size);
    if (!t->uploaded || nvn_dirty_since_span(t->span, o->mem, o->L.size, t->stamp)) upload(t, o); else t->clean_at = now;
    return t;
}
static void stage_reap(void) {   // under g_tmu
    uint64_t done = done_value();
    for (stage_buf **p = &g_stage; *p;) { stage_buf *s = *p; if (s->after <= done) { *p = s->next; vkDestroyBuffer(g_vkd.dev, s->buf, NULL); vkFreeMemory(g_vkd.dev, s->mem, NULL); free(s); } else p = &s->next; }
}

// ---- descriptor pools: texture and sampler IDs (mtl_reg_tex / mtl_reg_samp) ----
typedef struct { int ok, has_v; tex_t t; view_t v; } treg_t;
static treg_t *g_treg; static int g_ntreg; static samp_par *g_sreg; static uint8_t *g_sreg_ok; static int g_nsreg;
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (id >= g_ntreg) { int n = id + 1024; g_treg = realloc(g_treg, n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    treg_t *r = &g_treg[id]; r->ok = t != NULL; if (t) r->t = *t; r->has_v = v != NULL; if (v) r->v = *v;
    pthread_mutex_unlock(&g_rmu);
}
void mtl_reg_samp(int id, const samp_par *p) {
    if (id < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (id >= g_nsreg) { int n = id + 256; g_sreg = realloc(g_sreg, n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, n); memset(g_sreg_ok + g_nsreg, 0, n - g_nsreg); g_nsreg = n; }
    g_sreg[id] = *p; g_sreg_ok[id] = 1;
    pthread_mutex_unlock(&g_rmu);
}
static int treg_get(int id, treg_t *out) { pthread_mutex_lock(&g_rmu); int ok = id > 0 && id < g_ntreg && g_treg[id].ok; if (ok) *out = g_treg[id]; pthread_mutex_unlock(&g_rmu); return ok; }
static uint64_t g_tex_unreg;
// Texture binds: each bound texture is made ready to sample now (its draws are not translated yet)
static void op_bind_tex(uint32_t op, const void *a) {
    p_bind_handles t1; const p_bind_handles *b = bind_rec(op, a, &t1);
    if (b->n < 0 || b->n > 32) return;
    uint64_t h[32]; memcpy(h, b->h, 8 * (size_t)b->n);
    if (b->defer && b->src) memcpy(h, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);   // BindTexturesDeferred: the handles as they are at execution
    for (int k = 0; k < b->n; k++) {
        int tid = (int)(h[k] & 0xfffff); if (!tid) continue;
        treg_t r; if (!treg_get(tid, &r)) { g_tex_unreg++; continue; }
        pthread_mutex_lock(&g_tmu); tex_sampled(&r.t); pthread_mutex_unlock(&g_tmu);
    }
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
    sref **p = &g_tab[o->serial % 4096];
    while (*p && (*p)->serial != o->serial) p = &(*p)->next;
    if (*p) { sref *r = *p; vtex *t = r->t; *p = r->next; free(r);
        if (--t->refs <= 0 && !t->kmem) { stor_unlink(t); tex_retire(t); } }   // with storage it stays for the next object over it
    pthread_mutex_unlock(&g_tmu);
}

// ---- submissions ----
static void full_barrier(VkCommandBuffer c);
static void dump_uploads(uint64_t fence);
static void sub_open(void) {   // the next ring slot's command buffer, begun (its last use waited for)
    g_cb_i = (g_cb_i + 1) % CB_RING;
    wait_value(g_cb_val[g_cb_i], UINT64_MAX);   // its last use is done
    if (g_qp) { pthread_mutex_lock(&g_tmu); gpu_harvest(g_cb_i); pthread_mutex_unlock(&g_tmu); }
    vkResetCommandBuffer(g_cb[g_cb_i], 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(g_cb[g_cb_i], &bi); g_open = 1;
    if (g_qp) { vkCmdResetQueryPool(g_cb[g_cb_i], g_qp, (uint32_t)(2 * g_cb_i), 2); vkCmdWriteTimestamp(g_cb[g_cb_i], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_qp, (uint32_t)(2 * g_cb_i)); g_cb_frame[g_cb_i] = vkh_frame(); }
    g_up_open = 0;
    full_barrier(g_cb[g_cb_i]);   // after everything before it on the queue: the upload command buffer, earlier submissions, presents
    vkx_sub_begin(g_cb_i, g_cb[g_cb_i]);
}
static uint64_t g_flush_draws, g_flushes; static int g_noflush = -1;   // draws recorded since the command buffer began; mid-submission flushes made
static uint64_t sub_submit(void) {   // the open command buffer (after its uploads) to the queue, signalling the next timeline value
    VkCommandBuffer c = g_cb[g_cb_i];
    if (g_qp) { vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qp, (uint32_t)(2 * g_cb_i + 1)); g_cb_timed[g_cb_i] = 1; }
    vkEndCommandBuffer(c); g_open = 0;
    VkCommandBuffer cbs[2]; uint32_t ncb = 0;   // the uploads first, then the submission (which starts with a full barrier)
    if (g_up_open) { vkEndCommandBuffer(g_up[g_cb_i]); g_up_open = 0; cbs[ncb++] = g_up[g_cb_i]; }
    cbs[ncb++] = c;
    uint64_t v = atomic_load(&g_fence) + 1;
    VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &v};
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts, .commandBufferCount = ncb, .pCommandBuffers = cbs, .signalSemaphoreCount = 1, .pSignalSemaphores = &g_tl};
    pthread_mutex_lock(&g_vkd_qlock); VkResult r = vkQueueSubmit(g_vkd.q, 1, &si, VK_NULL_HANDLE); pthread_mutex_unlock(&g_vkd_qlock);
    if (r) die("vulkan replay: submit failed (%d)", (int)r);
    g_cb_val[g_cb_i] = v; atomic_store(&g_fence, v); g_flush_draws = 0;
    return v;
}
void mtl_sub_begin(void) {
    if (!vkr_on()) return;
    if (g_noflush < 0) g_noflush = getenv("DM_VK_NOFLUSH") != NULL;   // diagnostic: one command buffer per submission, as before
    sub_open();
}
// A long submission goes to the GPU in parts: when enough draws are recorded and no render pass is open, what there is is submitted and recording goes on in the next
// command buffer, so the GPU works on a frame's first passes while the rest is replayed (one command buffer per submission left the GPU idle for the whole replay, about
// 10 ms of Mario Wonder's 24 ms frame, and the CPU idle while the GPU ran). Fences stay in order: each part signals the next timeline value
#define FLUSH_DRAWS 128
static void vkr_flush(void) { sub_submit(); g_flushes++; sub_open(); }
uint64_t mtl_sub_commit(void) {
    if (!g_on) return atomic_fetch_add(&g_fence, 1) + 1;
    rp_end(g_cb[g_cb_i]);
    uint64_t v = sub_submit(); g_subs++;
    static int opslog = -1; if (opslog < 0) opslog = getenv("DM_VK_OPS") != NULL;
    if (opslog && !(g_subs & (g_subs - 1))) {
        char b[600]; int q = 0;
        for (int i = 0; i < 64 && q < (int)sizeof b - 24; i++) if (g_ops[i]) q += snprintf(b + q, sizeof b - q, " %d:%llu", i, (unsigned long long)g_ops[i]);
        LOG("vulkan replay: %llu submissions, %llu clears; records by op:%s", (unsigned long long)g_subs, (unsigned long long)g_clears, b);
    }
    pthread_mutex_lock(&g_tmu); reap(); stage_reap(); pthread_mutex_unlock(&g_tmu);
    if (g_ndump) dump_uploads(v);
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

// ---- render passes over the bound render targets (step 3.4c) ----
// Dynamic rendering begins at the first draw after the targets were set, and ends before anything that cannot be inside it (a clear, a target change, a barrier, the
// submission's end), each end followed by a full barrier so the next pass or a sampling sees what was drawn
static int g_rp_on, g_rp_ncolor, g_rp_samples; static uint32_t g_rp_w, g_rp_h; static uint64_t g_passes; static uint64_t g_ub_updates; static int g_passlog; static uint64_t g_pass0; static long g_passstop;
static VkImageView att_view_n(vtex *t, uint32_t level, uint32_t layer, uint32_t n, VkImageAspectFlags asp) {   // under g_tmu; n layers (a 3D image's depth slices)
    uint32_t key = 0x80000000u | (level & 0xff) | (layer & 0xfff) << 8 | (n & 0x7ff) << 20;
    for (vkview *v = t->views; v; v = v->next) if (v->key == key) return v->view;
    if (t->d > 1 && !(t->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return VK_NULL_HANDLE;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img, .viewType = n > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, .format = t->fmt,
        .subresourceRange = {asp, level, 1, layer, n}};   // of a 3D image (2D_ARRAY_COMPATIBLE), the layers are depth slices of the level
    vkview *v = calloc(1, sizeof *v); v->key = key;
    if (vkCreateImageView(g_vkd.dev, &vi, NULL, &v->view)) { free(v); return VK_NULL_HANDLE; }
    v->next = t->views; t->views = v; return v->view;
}
static VkImageView att_view(vtex *t, uint32_t level, uint32_t layer, VkImageAspectFlags asp) { return att_view_n(t, level, layer, 1, asp); }
static void rp_end(VkCommandBuffer c) { if (!g_rp_on) return; vkCmdEndRendering(c); g_rp_on = 0; full_barrier(c); }
static int rp_begin(VkCommandBuffer c) {
    if (g_rp_on) return 1;
    VkRenderingAttachmentInfo ca[8], da, sa; int nc = 0, have = 0, samples = 1; uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = 1;
    memset(ca, 0, sizeof ca); memset(&da, 0, sizeof da); memset(&sa, 0, sizeof sa);
    pthread_mutex_lock(&g_tmu);
    for (int i = 0; i < g_rt_n; i++) {
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        nc = i + 1;
        vtex *t = g_rt[i] ? tex_get(g_rt[i]) : NULL;
        if (!t || !(t->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) continue;
        VkImageSubresourceRange r = view_range(t, &g_rt_view[i], g_rt_has_view[i], VK_IMAGE_ASPECT_COLOR_BIT);
        if (t->d > 1) {   // a 3D target: the view's slices (all of the level's without one), drawn as layers (the shaders' layer output picks the slice)
            uint32_t dl = t->d >> r.baseMipLevel ? t->d >> r.baseMipLevel : 1, z0 = 0, nz = dl;
            if (g_rt_has_view[i] && g_rt_view[i].set_layers) { z0 = g_rt_view[i].min_layer < dl ? g_rt_view[i].min_layer : dl - 1; nz = g_rt_view[i].nlayers ? g_rt_view[i].nlayers : 1; if (z0 + nz > dl) nz = dl - z0; }
            ca[i].imageView = att_view_n(t, r.baseMipLevel, z0, nz, VK_IMAGE_ASPECT_COLOR_BIT); if (nz > layers) layers = nz;
        } else ca[i].imageView = att_view(t, r.baseMipLevel, r.baseArrayLayer, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!ca[i].imageView) continue;
        t->gpu_owned = 1; have = 1; samples = (int)t->samples;
        uint32_t lw = t->w >> r.baseMipLevel ? t->w >> r.baseMipLevel : 1, lh = t->h >> r.baseMipLevel ? t->h >> r.baseMipLevel : 1;
        if (lw < w) w = lw; if (lh < h) h = lh;
    }
    int has_d = 0, has_s = 0;
    vtex *dt = g_rt_depth ? tex_get(g_rt_depth) : NULL;
    if (dt && (dt->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        VkImageSubresourceRange r = view_range(dt, &g_rt_dview, g_rt_has_dview, dt->aspect);
        VkImageView v = att_view(dt, r.baseMipLevel, r.baseArrayLayer, dt->aspect);
        if (v) {
            has_d = (dt->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0; has_s = (dt->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
            da = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            sa = da; dt->gpu_owned = 1; have = 1; if (!nc) samples = (int)dt->samples;
            uint32_t lw = dt->w >> r.baseMipLevel ? dt->w >> r.baseMipLevel : 1, lh = dt->h >> r.baseMipLevel ? dt->h >> r.baseMipLevel : 1;
            if (lw < w) w = lw; if (lh < h) h = lh;
        }
    }
    pthread_mutex_unlock(&g_tmu);
    if (!have) return 0;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = layers, .colorAttachmentCount = (uint32_t)nc, .pColorAttachments = ca,
        .pDepthAttachment = has_d ? &da : NULL, .pStencilAttachment = has_s ? &sa : NULL};
    vkCmdBeginRendering(c, &ri);
    g_rp_on = 1; g_rp_ncolor = nc; g_rp_w = w; g_rp_h = h; g_rp_samples = samples; g_passes++;
    if (g_passlog) LOG("[pass] %llu: %ux%u, %d colour (first serial %llu), depth %s", (unsigned long long)g_passes, w, h, nc, g_rt[0] ? (unsigned long long)g_rt[0]->serial : 0ull, has_d || has_s ? "yes" : "no");
    return 1;
}
static void op_clear_color(VkCommandBuffer c, const p_clear_color *p) {
    rp_end(c);
    if (p->index < 0 || p->index >= g_rt_n || !g_rt[p->index]) return;
    pthread_mutex_lock(&g_tmu);
    vtex *t = tex_get(g_rt[p->index]);
    // multisampled targets too (vkCmdClearColorImage / vkCmdClearDepthStencilImage take any sample count): Pokémon Shining Pearl draws its scene into a 4x target whose depth,
    // never cleared, failed its reversed-Z GEQUAL test everywhere, and its background and characters were not drawn
    if (t && (t->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
        t->gpu_owned = 1;
        tex_ready(c, t);
        if (g_passlog) LOG("[pass] clear colour %d serial %llu to %g %g %g %g", p->index, (unsigned long long)t->serial, p->rgba[0], p->rgba[1], p->rgba[2], p->rgba[3]);
        VkClearColorValue v; memcpy(v.float32, p->rgba, sizeof v.float32);   // a float clear; integer targets take the same bits (open: NVN's integer clears)
        VkImageSubresourceRange r = view_range(t, &g_rt_view[p->index], g_rt_has_view[p->index], VK_IMAGE_ASPECT_COLOR_BIT);
        vkCmdClearColorImage(c, t->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r); full_barrier(c); g_clears++;
    }
    pthread_mutex_unlock(&g_tmu);
}
static void op_clear_ds(VkCommandBuffer c, const p_clear_ds *p) {
    rp_end(c);
    if (!g_rt_depth) return;
    pthread_mutex_lock(&g_tmu);
    vtex *t = tex_get(g_rt_depth);
    if (g_passlog) LOG("[pass] clear depth %g (mask %d) stencil %d (mask %#x) serial %llu%s", p->depth, p->depth_mask, p->stencil, p->stencil_mask, t ? (unsigned long long)t->serial : 0ull, t ? "" : ": no image");
    if (t && !(t->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {   // multisampled too (op_clear_color)
        VkImageAspectFlags asp = 0;
        if ((t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && p->depth_mask) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ((t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && p->stencil_mask) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (asp) {
            t->gpu_owned = 1;
            tex_ready(c, t);
            VkClearDepthStencilValue v = {p->depth < 0 ? 0 : p->depth > 1 ? 1 : p->depth, (uint32_t)p->stencil};
            VkImageSubresourceRange r = view_range(t, &g_rt_dview, g_rt_has_dview, asp);
            vkCmdClearDepthStencilImage(c, t->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r); full_barrier(c); g_clears++;
        }
    }
    pthread_mutex_unlock(&g_tmu);
}
// ---- copies and texture clears (the records nvn_obj.h shares with nvn_mtl.m) ----
// Each runs outside a render pass and is followed by a full barrier. A texture a copy writes becomes the GPU's (never reloaded from guest memory); one that still holds
// packaged content is loaded first, so a copy into part of it keeps the rest
static int64_t g_copy_row, g_copy_img;   // SetCopyRowStride / SetCopyImageStride (0 = tightly packed)
static uint64_t g_copies, g_copy_fail;
static void copy_fail(const char *what) { g_copy_fail++; if (!(g_copy_fail & (g_copy_fail - 1))) LOG("[RED] vulkan replay: %llu copies not made (latest: %s)", (unsigned long long)g_copy_fail, what); }
// The subresource and offset / extent of a copy region on an image (layers from the view and the region's z for layered images, depth slices for a 3D one)
static void region_of(const vtex *t, const view_t *v, int has_v, const NVNcopyRegion *r, VkImageSubresourceLayers *sl, VkOffset3D *o, VkExtent3D *e) {
    VkImageSubresourceRange vr = view_range(t, v, has_v, t->aspect);
    *sl = (VkImageSubresourceLayers){t->aspect, vr.baseMipLevel, 0, 1};
    uint32_t w = r->width > 0 ? (uint32_t)r->width : 1, h = r->height > 0 ? (uint32_t)r->height : 1, d = r->depth > 0 ? (uint32_t)r->depth : 1;
    if (t->d > 1) { *o = (VkOffset3D){r->xoffset, r->yoffset, r->zoffset}; *e = (VkExtent3D){w, h, d}; }
    else { sl->baseArrayLayer = vr.baseArrayLayer + (uint32_t)(r->zoffset > 0 ? r->zoffset : 0); sl->layerCount = d; *o = (VkOffset3D){r->xoffset, r->yoffset, 0}; *e = (VkExtent3D){w, h, 1}; }
    if (sl->baseArrayLayer + sl->layerCount > t->layers) sl->layerCount = sl->baseArrayLayer < t->layers ? t->layers - sl->baseArrayLayer : 0;
}
static void op_copy_t2t(VkCommandBuffer c, const p_copy_t2t *p) {
    rp_end(c);
    pthread_mutex_lock(&g_tmu);
    vtex *s = tex_sampled(p->src), *d = tex_get(p->dst);
    if (!s || !d) { pthread_mutex_unlock(&g_tmu); copy_fail("texture to texture: an image is missing"); return; }
    if (g_passlog) LOG("[pass] copy texture serial %llu %ux%u -> serial %llu %ux%u%s (objects: serial %llu storage %p -> serial %llu storage %p)", (unsigned long long)s->serial, s->w, s->h, (unsigned long long)d->serial, d->w, d->h,
                       p->flags & DM_COPY_RESOLVE ? " (resolve)" : "", (unsigned long long)p->src->serial, (const void *)p->src->mem, (unsigned long long)p->dst->serial, (const void *)p->dst->mem);
    if (!d->gpu_owned && !d->uploaded) upload(d, p->dst);
    d->gpu_owned = 1;
    VkImageSubresourceLayers ss, ds; VkOffset3D so, dof; VkExtent3D se, de;
    region_of(s, &p->sv, p->has_sv, &p->sr, &ss, &so, &se); region_of(d, &p->dv, p->has_dv, &p->dr, &ds, &dof, &de);
    if (!ss.layerCount || !ds.layerCount) { pthread_mutex_unlock(&g_tmu); copy_fail("texture to texture: layers out of range"); return; }
    if (p->flags & DM_COPY_RESOLVE) {
        VkImageResolve rr = {ss, so, ds, dof, se};
        vkCmdResolveImage(c, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &rr);
    } else if (s->fmt == d->fmt && se.width == de.width && se.height == de.height && se.depth == de.depth && ss.layerCount == ds.layerCount && s->samples == d->samples) {
        VkImageCopy ic = {ss, so, ds, dof, se};
        vkCmdCopyImage(c, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    } else if ((s->aspect & VK_IMAGE_ASPECT_COLOR_BIT) && (d->aspect & VK_IMAGE_ASPECT_COLOR_BIT) && s->samples == 1 && d->samples == 1) {
        if (ss.layerCount != ds.layerCount) ss.layerCount = ds.layerCount = ss.layerCount < ds.layerCount ? ss.layerCount : ds.layerCount;
        VkImageBlit bl = {ss, {so, {so.x + (int32_t)se.width, so.y + (int32_t)se.height, so.z + (int32_t)se.depth}}, ds, {dof, {dof.x + (int32_t)de.width, dof.y + (int32_t)de.height, dof.z + (int32_t)de.depth}}};
        vkCmdBlitImage(c, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, (p->flags & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);   // NVN_COPY_FLAGS_LINEAR_FILTER_BIT
    } else { pthread_mutex_unlock(&g_tmu); copy_fail("texture to texture: depth or multisampled images of different sizes or formats"); return; }
    pthread_mutex_unlock(&g_tmu);
    full_barrier(c); g_copies++;
}
// Buffer ↔ texture: the buffer side is linear, rows g_copy_row bytes apart (tight when 0)
static void op_copy_bt(VkCommandBuffer c, const p_copy_b2t *p, int to_tex) {
    rp_end(c);
    pthread_mutex_lock(&g_tmu);
    vtex *t = to_tex ? tex_get(p->tex) : tex_sampled(p->tex);
    if (!t) { pthread_mutex_unlock(&g_tmu); copy_fail("buffer and texture: no image"); return; }
    if (to_tex) { if (!t->gpu_owned && !t->uploaded) upload(t, p->tex); t->gpu_owned = 1; }
    VkImageSubresourceLayers sl; VkOffset3D o; VkExtent3D e; region_of(t, &p->view, p->has_view, &p->r, &sl, &o, &e);
    int bw, bh, bpb = nvn_fmt_bpb(p->tex->d.format, &bw, &bh);
    if (!bpb || !sl.layerCount) { pthread_mutex_unlock(&g_tmu); copy_fail("buffer and texture: no block size"); return; }
    uint64_t row = g_copy_row > 0 ? (uint64_t)g_copy_row : (uint64_t)((e.width + bw - 1) / bw) * bpb, rows = (e.height + bh - 1) / bh;
    uint64_t img = g_copy_img > 0 ? (uint64_t)g_copy_img : row * rows, n = img * (e.depth > 1 ? e.depth : sl.layerCount);
    VkBuffer b; VkDeviceSize off; VkDeviceAddress g;
    if (row % bpb || !vkm_lookup((const void *)(uintptr_t)p->src, n, &b, &off, &g)) { pthread_mutex_unlock(&g_tmu); copy_fail("buffer and texture: buffer not in GPU-visible memory or stride not a whole block"); return; }
    VkBufferImageCopy bc = {off, (uint32_t)(row / bpb * bw), (uint32_t)(img / row * bh), sl, o, e};
    if (to_tex) vkCmdCopyBufferToImage(c, b, t->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    else vkCmdCopyImageToBuffer(c, t->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &bc);
    pthread_mutex_unlock(&g_tmu);
    full_barrier(c); g_copies++;
}
static void op_clear_tex(VkCommandBuffer c, const p_clear_tex *p) {
    rp_end(c);
    pthread_mutex_lock(&g_tmu);
    vtex *t = tex_get(p->tex);
    if (!t || !(t->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) { pthread_mutex_unlock(&g_tmu); copy_fail("texture clear: no colour image"); return; }
    if (g_passlog) LOG("[pass] clear texture serial %llu %ux%u region %d %d %d %d", (unsigned long long)t->serial, t->w, t->h, p->r.xoffset, p->r.yoffset, p->r.width, p->r.height);
    if (!t->gpu_owned && !t->uploaded) upload(t, p->tex);
    t->gpu_owned = 1;
    VkImageSubresourceLayers sl; VkOffset3D o; VkExtent3D e; region_of(t, &p->view, p->has_view, &p->r, &sl, &o, &e);
    VkClearColorValue v; memcpy(&v, &p->c, sizeof v);
    uint32_t lw = t->w >> sl.mipLevel ? t->w >> sl.mipLevel : 1, lh = t->h >> sl.mipLevel ? t->h >> sl.mipLevel : 1;
    if (!o.x && !o.y && e.width >= lw && e.height >= lh && t->d == 1) {   // the whole of each layer: a plain clear
        VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, sl.mipLevel, 1, sl.baseArrayLayer, sl.layerCount};
        vkCmdClearColorImage(c, t->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rr);
    } else if (t->d == 1 && (t->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {   // part of it: a rectangle cleared in a pass over each layer
        for (uint32_t L = 0; L < sl.layerCount; L++) {
            VkImageView iv = att_view(t, sl.mipLevel, sl.baseArrayLayer + L, VK_IMAGE_ASPECT_COLOR_BIT); if (!iv) continue;
            VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = iv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {lw, lh}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
            vkCmdBeginRendering(c, &ri);
            VkClearAttachment cl = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = v}}; VkClearRect cr = {{{o.x, o.y}, {e.width, e.height}}, 0, 1};
            vkCmdClearAttachments(c, 1, &cl, 1, &cr);
            vkCmdEndRendering(c);
        }
    } else { pthread_mutex_unlock(&g_tmu); copy_fail("texture clear: part of an image that cannot be a render target"); return; }
    pthread_mutex_unlock(&g_tmu);
    full_barrier(c);
}

void mtl_exec(const uint8_t *handle) {
    if (!g_on || !g_open) return;
    VkCommandBuffer c = g_cb[g_cb_i];
    const uint8_t *p = handle, *ret[16]; int nret = 0;
    for (uint64_t guard = 0; p && guard < 50000000; guard++) {
        if (g_flush_draws >= FLUSH_DRAWS && !g_rp_on && !g_noflush) { vkr_flush(); c = g_cb[g_cb_i]; }   // between render passes: the part so far to the GPU
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        if (h.op < 64) g_ops[h.op]++;
        switch (h.op) {
        case OP_END: if (nret) { p = ret[--nret]; continue; } return;
        case OP_JUMP: memcpy(&p, a, 8); continue;
        case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); if (nret == 16) { LOG("[RED] vulkan replay: command buffer calls nested deeper than 16"); return; } ret[nret++] = p + h.size; p = t; continue; }
        case OP_SET_RT: {
            rp_end(c);
            const p_set_rt *s = a; g_rt_n = s->n < 0 ? 0 : s->n > 8 ? 8 : s->n;
            for (int i = 0; i < 8; i++) { g_rt[i] = i < g_rt_n ? s->color[i] : NULL; g_rt_view[i] = s->cview[i]; g_rt_has_view[i] = s->has_cview[i]; }
            g_rt_depth = s->depth; g_rt_dview = s->dview; g_rt_has_dview = s->has_dview;
            break;
        }
        case OP_CLEAR_COLOR: op_clear_color(c, a); break;
        case OP_CLEAR_DS: op_clear_ds(c, a); break;
        case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: {   // textures are made ready to sample now; the handles go to the draw state
            p_bind_handles t1; const p_bind_handles *b = bind_rec(h.op, a, &t1);
            if (b->n < 0 || b->n > 32) break;
            uint64_t hh[32]; memcpy(hh, b->h, 8 * (size_t)b->n);
            if (b->defer && b->src) memcpy(hh, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);   // BindTexturesDeferred: the handles as they are at execution
            vkx_bind_handles(h.op == OP_BIND_SAMP ? 1 : h.op == OP_BIND_IMG ? 2 : 0, b, hh);
            if (h.op == OP_BIND_TEX || h.op == OP_BIND_TEX1) op_bind_tex(h.op, a);
            break;
        }
        case OP_BIND_PROGRAM: vkx_bind_program(((const p_bind_program *)a)->prog, ((const p_bind_program *)a)->stages); break;
        case OP_BIND_UBO: case OP_BIND_UBO_SMALL: { p_bind_buf tb; vkx_bind_buf(0, bind_buf_rec(h.op, a, &tb)); break; }
        case OP_BIND_SSBO: vkx_bind_buf(1, a); break;
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: vkx_set_pool(h.op == OP_SET_TEX_POOL, nvn_dpool_base(((const p_set_pool *)a)->pool, h.op == OP_SET_TEX_POOL)); break;
        case OP_DRAW: case OP_DRAW_IDX: if (g_passlog && g_passstop && g_passes - g_pass0 > (uint64_t)g_passstop) break;   // DM_VK_PASSSTOP: the traced frame's later passes draw nothing
            if (rp_begin(c)) vkx_draw(c, h.op, a, h.size, g_rp_ncolor, g_rp_w, g_rp_h, g_rp_samples); g_flush_draws++; break;
        case OP_BARRIER: rp_end(c); full_barrier(c); break;
        case OP_COPY_T2T: op_copy_t2t(c, a); break;
        case OP_COPY_B2T: op_copy_bt(c, a, 1); break;
        case OP_COPY_T2B: op_copy_bt(c, a, 0); break;
        case OP_CLEAR_TEX: op_clear_tex(c, a); break;
        case OP_COPY_STRIDE: { const p_copy_stride *s = a; if (s->image) g_copy_img = s->stride; else g_copy_row = s->stride; break; }
        case OP_DISPATCH: rp_end(c); vkx_dispatch(c, a); full_barrier(c); g_flush_draws++; break;
        case OP_UPDATE_UB: {   // {dst, n, data}: the bytes written into the buffer at this point of the GPU's work (UpdateUniformBuffer)
            uint64_t dst, n; memcpy(&dst, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8);
            VkBuffer b; VkDeviceSize off; VkDeviceAddress g;
            if (n && n <= 65536 && !(n & 3) && !(dst & 3) && vkm_lookup((const void *)(uintptr_t)dst, n, &b, &off, &g)) {
                rp_end(c); vkCmdUpdateBuffer(c, b, off, n, (const uint8_t *)a + 16); full_barrier(c); g_ub_updates++;
            } else { static int w; if (!w++) LOG("[RED] vulkan replay: uniform update of %llu bytes at %#llx not made (not 4-aligned or not in GPU-visible memory)", (unsigned long long)n, (unsigned long long)dst); }
            break;
        }
        case OP_COPY_B2B: {   // {src, dst, n}: CopyBufferToBuffer
            uint64_t src, dst, n; memcpy(&src, a, 8); memcpy(&dst, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8);
            VkBuffer sb, db; VkDeviceSize so, dof; VkDeviceAddress g;
            if (n && vkm_lookup((const void *)(uintptr_t)src, n, &sb, &so, &g) && vkm_lookup((const void *)(uintptr_t)dst, n, &db, &dof, &g)) {
                rp_end(c); VkBufferCopy bc = {so, dof, n}; vkCmdCopyBuffer(c, sb, db, 1, &bc); full_barrier(c);
            } else { static int w; if (!w++) LOG("[RED] vulkan replay: buffer copy of %llu bytes %#llx -> %#llx not made (not in GPU-visible memory)", (unsigned long long)n, (unsigned long long)src, (unsigned long long)dst); }
            break;
        }
        case OP_BIND_STATE: vkx_bind_state(a); break;
        case OP_VIEWPORTS: vkx_viewports(a); break;
        case OP_SCISSOR: vkx_scissor(a); break;
        case OP_DEPTH_RANGE: vkx_depth_range(a); break;
        case OP_STENCIL: vkx_stencil(a); break;
        case OP_POLY_OFFSET: vkx_poly_offset(a); break;
        case OP_BLEND_COLOR: vkx_blend_color(a); break;
        case OP_DEPTH_CLAMP: vkx_depth_clamp(((const p_depth_clamp *)a)->on); break;
        case OP_PRIM_RESTART: vkx_prim_restart((int)((const p_prim_restart *)a)->on); break;
        case OP_BIND_VATTR: vkx_vattr(a); break;
        case OP_BIND_VSTREAM: vkx_vstream(a); break;
        case OP_BIND_VBUF: case OP_BIND_VBUF1: { p_bind_vbuf tv; vkx_vbuf(vbuf_rec(h.op, a, &tv)); break; }
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
static void shoot_vt_flip(vtex *t, uint32_t layer, int flip, uint64_t fence, const char *path) {
    if (!t || !(t->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) { LOG("[RED] screenshot %s: no colour image", path); return; }
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vkd.pd, t->fmt, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) { LOG("[RED] screenshot %s: format %d cannot be blitted to RGBA8 here", path, (int)t->fmt); return; }
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
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer < t->layers ? layer : 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
    if (flip) { bl.dstOffsets[0].y = (int32_t)h; bl.dstOffsets[1].y = 0; }   // a presented image of a lower-left-origin game is bottom-up
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
static void shoot_vt_layer(vtex *t, uint32_t layer, uint64_t fence, const char *path) { shoot_vt_flip(t, layer, 0, fence, path); }
static void shoot_vt(vtex *t, uint64_t fence, const char *path) { shoot_vt_layer(t, 0, fence, path); }
static void shoot(const tex_t *o, uint64_t fence, const char *path) { pthread_mutex_lock(&g_tmu); vtex *t = tex_get(o); pthread_mutex_unlock(&g_tmu); shoot_vt_flip(t, 0, vkx_origin_ll(), fence, path); }   // as presented
// DM_VK_TEXDUMP: the textures loaded during this submission, once it is done
static void dump_uploads(uint64_t fence) {
    static int n;
    for (int i = 0; i < g_ndump; i++) {
        pthread_mutex_lock(&g_tmu); vtex *t = tex_find(g_dump_ser[i]); pthread_mutex_unlock(&g_tmu);
        char p[1024]; snprintf(p, sizeof p, "%s/tex%03d_ser%llu_%ux%u_vkfmt%d.png", g_dump_dir, ++n, (unsigned long long)g_dump_ser[i], t ? t->w : 0, t ? t->h : 0, t ? (int)t->fmt : 0);
        if (t) shoot_vt(t, fence, p);
    }
    g_ndump = 0;
}
static pthread_mutex_t g_shot_mu = PTHREAD_MUTEX_INITIALIZER; static long g_shot_k; static char g_shot_path[1024]; static uint64_t g_presents;
void dm_gfx_shot_at(long k, const char *path) { pthread_mutex_lock(&g_shot_mu); g_shot_k = (long)g_presents + k; snprintf(g_shot_path, sizeof g_shot_path, "%s", path); pthread_mutex_unlock(&g_shot_mu); }
static void shot_check(const tex_t *t) {
    static int env = 0;
    if (!env) { env = 1; const char *e = getenv("DM_SHOT"); char *c; long k; if (e && (k = strtol(e, &c, 10)) > 0 && *c == ':') dm_gfx_shot_at(k, c + 1); }
    pthread_mutex_lock(&g_shot_mu);
    g_presents++;
    { static long pk = -1; if (pk < 0) { const char *e = getenv("DM_VK_PASSLOG"); pk = e ? atol(e) : 0; } g_passlog = pk > 0 && (long)g_presents + 1 == pk; vkx_trace(g_passlog); g_pass0 = g_passes;
      { static int se; if (!se) { se = 1; const char *e = getenv("DM_VK_PASSSTOP"); g_passstop = e ? atol(e) : 0; } } if (g_passlog) LOG("[pass] present %llu begins", (unsigned long long)g_presents + 1); }
    int now = g_shot_k && (long)g_presents >= g_shot_k; char path[1024]; if (now) { snprintf(path, sizeof path, "%s", g_shot_path); g_shot_k = 0; }
    pthread_mutex_unlock(&g_shot_mu);
    if (now && g_on > 0) shoot(t, atomic_load(&g_fence), path);
    // DM_VK_RTDUMP=<present>:<dir>: at that present, every colour image the GPU rendered (render targets), as <dir>/rt_<serial>_<w>x<h>.png
    static long rtk = -1; static char rtdir[900];
    if (rtk < 0) { const char *e = getenv("DM_VK_RTDUMP"); char *c; rtk = 0; if (e && (rtk = strtol(e, &c, 10)) > 0 && *c == ':') snprintf(rtdir, sizeof rtdir, "%s", c + 1); else rtk = 0; }
    if (rtk > 0 && (long)g_presents == rtk && g_on > 0) {
        vtex *all[256]; int na = 0;
        pthread_mutex_lock(&g_tmu); for (int i = 0; i < 4096 && na < 256; i++) for (vtex *t = g_stab[i]; t && na < 256; t = t->next) if (t->gpu_owned && (t->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) all[na++] = t; pthread_mutex_unlock(&g_tmu);
        for (int i = 0; i < na; i++) for (uint32_t ly = 0; ly < all[i]->layers && ly < 12; ly++) {   // layered images: each layer (the first 12)
            char p[1100]; snprintf(p, sizeof p, ly ? "%s/rt_%06llu_%ux%u_f%d_L%u.png" : "%s/rt_%06llu_%ux%u_f%d.png", rtdir, (unsigned long long)all[i]->serial, all[i]->w, all[i]->h, (int)all[i]->fmt, ly);
            shoot_vt_layer(all[i], ly, atomic_load(&g_fence), p); }
    }
    // DM_SHOT_EVERY=<n>:<dir>: every n-th presented frame as <dir>/present<number>.png (at most 40)
    static long every = -1; static char edir[900]; static int shots;
    if (every < 0) { const char *e = getenv("DM_SHOT_EVERY"); char *c; every = 0; if (e && (every = strtol(e, &c, 10)) > 0 && *c == ':') snprintf(edir, sizeof edir, "%s", c + 1); else every = 0; }
    if (every > 0 && g_on > 0 && shots < 40 && g_presents % (uint64_t)every == 0) { char p[1000]; snprintf(p, sizeof p, "%s/present%05llu.png", edir, (unsigned long long)g_presents); shots++; shoot(t, atomic_load(&g_fence), p); }
}

// ---- for vk_draw.c ----
vtex *vkr_tex_sampled(const tex_t *o, VkCommandBuffer unused) { (void)unused; return tex_sampled(o); }
int vkr_treg(int id, tex_t *t, view_t *v, int *has_v) { treg_t r; if (!treg_get(id, &r)) return 0; *t = r.t; *v = r.v; *has_v = r.has_v; return 1; }
int vkr_sreg(int id, samp_par *p) { pthread_mutex_lock(&g_rmu); int ok = id > 0 && id < g_nsreg && g_sreg_ok[id]; if (ok) *p = g_sreg[id]; pthread_mutex_unlock(&g_rmu); return ok; }
uint32_t vkr_pick_mem(uint32_t bits, VkMemoryPropertyFlags want) { return pick_mem(bits, want); }
uint64_t vkr_fence_next(void) { return atomic_load(&g_fence) + 1; }
int vkr_slot(void) { return g_cb_i; }
VkCommandBuffer vkr_cmd(void) { return g_cb[g_cb_i]; }
