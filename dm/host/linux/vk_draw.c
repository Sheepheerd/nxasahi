// Linux build: the draw path of the Vulkan replay (step 3.4 of the backend, LINUX.md). Step 3.4b: shaders and their resources.
//   - the bound state, per NVN stage, as the replay's walker records it: uniform and storage buffers, texture / image / sampler handles, the descriptor pools' bases, the program
//   - a program's stages as shader objects (VK_EXT_shader_object), made the first time from <key>.vk.spv in DM_VK_SPV (dm/gfx/tools/dmsh_spv.py), fitted to the device
//     (spvr_fit), with one set layout for every set: dmsh_spv.py moved every shader's resources to fixed bindings, a set per stage
//   - per draw, a descriptor set for each of the program's stages, written from the shader's binding list (<key>.vkb): the support buffer, the shader's c1, the driver
//     banks c0 / c2 (filled from the bound state), uniform and storage buffers (vkm_lookup), textures with views of the shader's dimension and their samplers
//     (cached by their parameters). Whatever is missing gets a dummy, so a shader never reads an unwritten descriptor; the misses are counted (DM_VK_DRAWLOG=1)
// The draws themselves (vertex input, dynamic state, render passes) are step 3.4c
#include "dm.h"
#include "vk_dev.h"
#include "vk_fmt.h"
#include "vk_mem.h"
#include "vk_replay.h"
#include "spv_reflect.h"
#include "vk_draw.h"
#include <stdatomic.h>
#include <dirent.h>

#define NST 6
typedef struct { uint64_t addr, size; } bbuf;
static struct { bbuf ubo[NST][16], ssbo[NST][16]; uint64_t tex[NST][256], samp[NST][128], img[NST][32]; int tex_base, samp_base; const prog_t *prog; } S;

// ---- shaders ----
#define NBIND_MAX 96
typedef struct { uint16_t b; char desc; char kind[12]; int reg, off; char skind[12]; int sreg, soff; uint8_t dim, arr, ms, dep; uint16_t cnt; } vkbind;
typedef struct vks {
    char key[17]; struct vks *next; int stage, ok;
    VkShaderEXT sh; VkShaderStageFlagBits vstage;
    int nb; vkbind bd[NBIND_MAX];
    VkBuffer c1; VkDeviceMemory c1m; uint32_t c1_len;
} vks;
static vks *g_sh[1024]; static pthread_mutex_t g_shmu = PTHREAD_MUTEX_INITIALIZER;
static VkDescriptorSetLayout g_dsl; static VkPipelineLayout g_pl_gfx, g_pl_cs;
static int g_init = -1; static char g_spv[1024]; static uint32_t g_per_set[5];   // descriptors of a set by type: uniform, storage, combined, storage image, texel buffer
static uint64_t g_st_load, g_st_fail, g_st_nokey, g_st_draws, g_st_sets, g_miss[16];
enum { M_UBO, M_TEX, M_TEXREG, M_SAMP, M_SBUF, M_IMG, M_TBUF, M_UNALIGNED, M_NOSHADER, M_OTHER };
static const char *const MISS[] = {"uniform", "texture", "texture ID", "sampler", "storage", "image", "texel buffer", "unaligned", "shader", "other"};

static uint32_t hexbyte(const char *p) { unsigned v = 0; sscanf(p, "%2x", &v); return v; }
static int read_file(const char *path, uint8_t **out, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    *out = malloc((size_t)len + 1); *n = fread(*out, 1, (size_t)len, f); (*out)[*n] = 0; fclose(f); return 1;
}
static void buffer_with(const void *data, size_t n, VkBuffer *b, VkDeviceMemory *m) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n ? n : 16, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
    vkCreateBuffer(g_vkd.dev, &bi, NULL, b);
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vkd.dev, *b, &mr);
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vkr_pick_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    vkAllocateMemory(g_vkd.dev, &ma, NULL, m); vkBindBufferMemory(g_vkd.dev, *b, *m, 0);
    void *p; vkMapMemory(g_vkd.dev, *m, 0, VK_WHOLE_SIZE, 0, &p); memset(p, 0, mr.size); if (data && n) memcpy(p, data, n); vkUnmapMemory(g_vkd.dev, *m);
}
static VkShaderStageFlagBits vstage_of(int st) {
    static const VkShaderStageFlagBits m[NST] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                 VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
    return st >= 0 && st < NST ? m[st] : 0;
}
static void parse_vkb(vks *s, const char *txt);
static int draw_init(void) {
    if (g_init >= 0) return g_init;
    const char *d = getenv("DM_VK_SPV");
    if (!vkd_ok() || !g_vkd.shader_object || !d || !*d) {
        LOG("%svulkan draws: %s", d && *d ? "[RED] " : "", !vkd_ok() ? "no device" : !g_vkd.shader_object ? "no VK_EXT_shader_object" : "no shaders (set DM_VK_SPV to dmsh_spv.py's spv-<chain> directory)");
        return g_init = 0;
    }
    snprintf(g_spv, sizeof g_spv, "%s", d);
    // One set layout for every stage's set: the union of the bindings the game's shaders use (dmsh_spv.py's canonical bindings, listed in each <key>.vkb), each partially
    // bound. A set takes pool space for every binding of its layout, so only the used ones are in it
    static uint16_t cnt[1024]; static char typ[1024];
    DIR *dir = opendir(g_spv); int nfiles = 0;
    if (!dir) { LOG("[RED] vulkan draws: DM_VK_SPV=%s is not a directory", g_spv); return g_init = 0; }
    for (struct dirent *de; (de = readdir(dir));) {
        size_t l = strlen(de->d_name); if (l < 5 || strcmp(de->d_name + l - 4, ".vkb")) continue;
        char path[1400]; snprintf(path, sizeof path, "%s/%s", g_spv, de->d_name);
        uint8_t *txt; size_t n; if (!read_file(path, &txt, &n)) continue;
        static vks tmp; tmp.nb = 0; parse_vkb(&tmp, (const char *)txt); free(txt); nfiles++;
        for (int i = 0; i < tmp.nb; i++) { const vkbind *b = &tmp.bd[i]; if (b->b >= 1024) continue;
            if (typ[b->b] && typ[b->b] != b->desc) LOG("[RED] vulkan draws: binding %u is %c in one shader and %c in another", b->b, typ[b->b], b->desc);
            typ[b->b] = b->desc; uint16_t c = b->cnt ? b->cnt : 1; if (c > cnt[b->b]) cnt[b->b] = c; }
    }
    closedir(dir);
    VkDescriptorSetLayoutBinding b[1024]; VkDescriptorBindingFlags f[1024]; uint32_t n = 0;
    for (uint32_t i = 0; i < 1024; i++) {
        if (!typ[i]) continue;
        VkDescriptorType t = typ[i] == 'u' ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : typ[i] == 's' ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : typ[i] == 't' ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                             typ[i] == 'i' ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : typ[i] == 'b' ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLER;
        b[n] = (VkDescriptorSetLayoutBinding){i, t, cnt[i], VK_SHADER_STAGE_ALL, NULL}; f[n++] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        for (int k = 0; k < 5; k++) if ((int)t == (int[]){VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER}[k]) g_per_set[k] += cnt[i];
    }
    if (!nfiles) { LOG("[RED] vulkan draws: no <key>.vkb in %s (run dm/gfx/tools/dmsh_spv.py)", g_spv); return g_init = 0; }
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = n, .pBindingFlags = f};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &fi, .bindingCount = n, .pBindings = b};
    if (vkCreateDescriptorSetLayout(g_vkd.dev, &li, NULL, &g_dsl)) { LOG("[RED] vulkan draws: no descriptor set layout"); return g_init = 0; }
    VkDescriptorSetLayout sets[5] = {g_dsl, g_dsl, g_dsl, g_dsl, g_dsl};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 5, .pSetLayouts = sets};
    VkPipelineLayoutCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = sets};
    if (vkCreatePipelineLayout(g_vkd.dev, &pi, NULL, &g_pl_gfx) || vkCreatePipelineLayout(g_vkd.dev, &pc, NULL, &g_pl_cs)) { LOG("[RED] vulkan draws: no pipeline layouts"); return g_init = 0; }
    LOG("vulkan draws: shaders from %s (%d), one set layout of %u bindings for every stage (%u uniform, %u storage, %u textures, %u images, %u texel buffers per set)", g_spv, nfiles, n,
        g_per_set[0], g_per_set[1], g_per_set[2], g_per_set[3], g_per_set[4]);
    return g_init = 1;
}
static void parse_vkb(vks *s, const char *txt) {
    for (const char *p = txt; *p;) {
        const char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) : strlen(p);
        if (*p != '#' && len && s->nb < NBIND_MAX) {
            char line[512]; snprintf(line, sizeof line, "%.*s", (int)(len < 511 ? len : 511), p);
            char *f[13] = {0}; int nf = 0; for (char *q = line, *t; nf < 13 && (t = strsep(&q, "\t")); ) f[nf++] = t;
            if (nf == 13) {
                vkbind *b = &s->bd[s->nb++];
                b->b = (uint16_t)atoi(f[0]); b->desc = f[1][0]; snprintf(b->kind, sizeof b->kind, "%s", f[2]); b->reg = *f[3] ? atoi(f[3]) : -1; b->off = *f[4] ? atoi(f[4]) : 0;
                snprintf(b->skind, sizeof b->skind, "%s", f[5]); b->sreg = *f[6] ? atoi(f[6]) : -1; b->soff = *f[7] ? atoi(f[7]) : 0;
                b->dim = (uint8_t)atoi(f[8]); b->arr = (uint8_t)atoi(f[9]); b->ms = (uint8_t)atoi(f[10]); b->dep = (uint8_t)atoi(f[11]); b->cnt = (uint16_t)atoi(f[12]);
            }
        }
        if (!e) break; p = e + 1;
    }
}
// The shader of a program stage, made the first time (its key from prog_key; NULL when there is none or it cannot be made, logged once per key)
static vks *shader_get(const prog_t *p, int st) {
    uint8_t k8[8]; if (!prog_key(p, (unsigned)st, k8)) return NULL;
    char key[17]; for (int i = 0; i < 8; i++) snprintf(key + 2 * i, 3, "%02x", k8[i]);
    unsigned hsh = (k8[0] | k8[1] << 8) & 1023;
    pthread_mutex_lock(&g_shmu);
    for (vks *s = g_sh[hsh]; s; s = s->next) if (!strcmp(s->key, key)) { pthread_mutex_unlock(&g_shmu); return s->ok ? s : NULL; }
    vks *s = calloc(1, sizeof *s); snprintf(s->key, sizeof s->key, "%s", key); s->stage = st; s->vstage = vstage_of(st);
    s->next = g_sh[hsh]; g_sh[hsh] = s;
    char path[1200]; uint8_t *code = NULL, *vkb = NULL, *job = NULL; size_t ncode = 0, nvkb = 0, njob = 0;
    snprintf(path, sizeof path, "%s/%s.vk.spv", g_spv, key); int have = read_file(path, &code, &ncode);
    snprintf(path, sizeof path, "%s/%s.vkb", g_spv, key); have = have && read_file(path, &vkb, &nvkb);
    snprintf(path, sizeof path, "%s/%s.job.json", g_spv, key); if (have) read_file(path, &job, &njob);
    if (!have) { g_st_nokey++; if (g_st_nokey <= 8) LOG("[RED] vulkan draws: no translated shader %s (stage %d) in %s", key, st, g_spv); free(code); free(vkb); pthread_mutex_unlock(&g_shmu); return NULL; }
    parse_vkb(s, (const char *)vkb);
    // c1: the shader's own constants, from its translation job
    const char *c1 = job ? strstr((const char *)job, "\"c1\": \"") : NULL;
    uint8_t *c1d = NULL; size_t c1n = 0;
    if (c1) { c1 += 7; const char *e = strchr(c1, '"'); c1n = e ? (size_t)(e - c1) / 2 : 0; c1d = malloc(c1n ? c1n : 1); for (size_t i = 0; i < c1n; i++) c1d[i] = (uint8_t)hexbyte(c1 + 2 * i); }
    buffer_with(c1d, c1n > 65536 ? 65536 : (c1n ? c1n : 256), &s->c1, &s->c1m); s->c1_len = (uint32_t)c1n; free(c1d);
    spvr_device dev = {0, 0, 0}; size_t fw; unsigned ch;
    uint32_t *fit = spvr_fit((const uint32_t *)code, ncode / 4, &dev, &fw, &ch);
    VkDescriptorSetLayout sets[5] = {g_dsl, g_dsl, g_dsl, g_dsl, g_dsl};
    VkShaderCreateInfoEXT ci = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT, .stage = s->vstage,
        .nextStage = st == 0 ? VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT : st == 2 || st == 4 ? VK_SHADER_STAGE_FRAGMENT_BIT :
                     st == 3 ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT : 0,
        .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT, .codeSize = fw * 4, .pCode = fit, .pName = "main", .setLayoutCount = st == 5 ? 1 : 5, .pSetLayouts = sets};
    VkResult r = fit ? g_vkd.create_shaders(g_vkd.dev, 1, &ci, NULL, &s->sh) : VK_ERROR_INITIALIZATION_FAILED;
    free(fit); free(code); free(vkb); free(job);
    s->ok = r == VK_SUCCESS;
    if (s->ok) g_st_load++; else { g_st_fail++; LOG("[RED] vulkan draws: shader %s (stage %d) not made (%d)", key, st, (int)r); }
    if (s->ok && !(g_st_load & (g_st_load - 1))) LOG("vulkan draws: %llu shader objects made (latest %s, stage %d, %d bindings%s)", (unsigned long long)g_st_load, key, st, s->nb,
                                                     ch & SPVR_FIT_STORES ? ", [RED] writes a vertex-stage buffer the device cannot" : "");
    pthread_mutex_unlock(&g_shmu);
    return s->ok ? s : NULL;
}

// ---- resources ----
static VkBuffer g_zero, g_support; static VkDeviceMemory g_zero_m, g_support_m;
static VkImage g_dimg[6]; static VkImageView g_dview[6]; static VkDeviceMemory g_dmem[6]; static VkSampler g_dsamp;
static int g_res_ready;
// Dummies: a zero buffer, and a 1x1 zero image of each view type (1D/2D as 2D, 2D array, 3D, cube, cube array), cleared and in GENERAL layout before the first use
static void res_init(VkCommandBuffer c) {
    if (g_res_ready) return; g_res_ready = 1;
    buffer_with(NULL, 65536, &g_zero, &g_zero_m);
    float sup[16384] = {0}; for (int i = 0; i < 73; i++) sup[(192 + 16 * i) / 4] = 1.0f;   // render_scale = 1 (Ryujinx SupportBuffer, as nvn_mtl.m support_buf)
    buffer_with(sup, sizeof sup, &g_support, &g_support_m);
    for (int k = 0; k < 6; k++) {
        int cube = k == 3 || k == 4, three = k == 2;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, .imageType = three ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = cube ? 6 : 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        vkCreateImage(g_vkd.dev, &ci, NULL, &g_dimg[k]);
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_vkd.dev, g_dimg[k], &mr);
        VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vkr_pick_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        vkAllocateMemory(g_vkd.dev, &ma, NULL, &g_dmem[k]); vkBindImageMemory(g_vkd.dev, g_dimg[k], g_dmem[k], 0);
        static const VkImageViewType vt[6] = {VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, VK_IMAGE_VIEW_TYPE_2D};
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_dimg[k], .viewType = vt[k], .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1}};
        vkCreateImageView(g_vkd.dev, &vi, NULL, &g_dview[k]);
        VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_dimg[k], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1}};
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkClearColorValue z = {{0, 0, 0, 0}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1};
        vkCmdClearColorImage(c, g_dimg[k], VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr);
    }
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .maxLod = VK_LOD_CLAMP_NONE,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    vkCreateSampler(g_vkd.dev, &si, NULL, &g_dsamp);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static int dummy_index(const vkbind *b) { return b->dim == 3 ? 2 : b->dim == 4 ? (b->arr ? 4 : 3) : b->arr ? 1 : 0; }

// Samplers, cached by their parameters (the device allows a few thousand)
typedef struct samp_ent { samp_par p; VkSampler s; struct samp_ent *next; } samp_ent;
static samp_ent *g_samps[256]; static int g_nsamp;
static VkSamplerAddressMode wrap(int w) {
    switch (w) { case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT; case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; case 5: case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                 case 2: case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; }
}
static VkSampler sampler_get(const samp_par *p) {
    uint32_t h = 2166136261u; for (size_t i = 0; i < sizeof *p; i++) h = (h ^ ((const uint8_t *)p)[i]) * 16777619u;
    for (samp_ent *e = g_samps[h & 255]; e; e = e->next) if (!memcmp(&e->p, p, sizeof *p)) return e->s;
    if (g_nsamp >= 3900) return g_dsamp;
    int minf = p->minf, mip = minf >= 2;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = p->magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (minf == 1 || minf == 3 || minf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = (minf == 4 || minf == 5) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = wrap(p->ws), .addressModeV = wrap(p->wt), .addressModeW = wrap(p->wr), .mipLodBias = p->lod_bias,
        .anisotropyEnable = p->aniso > 1, .maxAnisotropy = p->aniso > 16 ? 16 : p->aniso < 1 ? 1 : p->aniso,
        .compareEnable = p->cmp_mode == 1, .compareOp = p->cmp_func >= 1 && p->cmp_func <= 8 ? (VkCompareOp)(p->cmp_func - 1) : VK_COMPARE_OP_ALWAYS,
        .minLod = p->lod_min, .maxLod = mip ? p->lod_max : 0.25f, .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    samp_ent *e = calloc(1, sizeof *e); e->p = *p;
    if (vkCreateSampler(g_vkd.dev, &si, NULL, &e->s)) { free(e); return g_dsamp; }
    e->next = g_samps[h & 255]; g_samps[h & 255] = e; g_nsamp++;
    return e->s;
}
// A view of an image as the shader declares it (dim / arrayed from its binding), cached on the image (under g_tmu)
static VkImageView view_get(vtex *t, const vkbind *b, const tex_t *o) {
    VkImageViewType vt = b->dim == 3 ? VK_IMAGE_VIEW_TYPE_3D : b->dim == 4 ? (b->arr ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE)
                       : b->arr ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    if ((vt == VK_IMAGE_VIEW_TYPE_3D) != (t->d > 1)) return VK_NULL_HANDLE;   // the image is not of this kind
    if ((vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) && t->layers % 6) return VK_NULL_HANDLE;
    uint32_t key = (uint32_t)vt;
    for (vkview *v = t->views; v; v = v->next) if (v->key == key) return v->view;
    VkComponentMapping sw; vkf_from_nvn(o->d.format, NULL, &sw);
    VkImageAspectFlags asp = t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : t->aspect;   // a depth-stencil image samples its depth
    uint32_t layers = vt == VK_IMAGE_VIEW_TYPE_2D || vt == VK_IMAGE_VIEW_TYPE_3D ? 1 : vt == VK_IMAGE_VIEW_TYPE_CUBE ? 6 : vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY ? t->layers - t->layers % 6 : t->layers;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img, .viewType = vt, .format = t->fmt, .components = sw,
        .subresourceRange = {asp, 0, t->levels, 0, layers}};
    vkview *v = calloc(1, sizeof *v); v->key = key;
    if (vkCreateImageView(g_vkd.dev, &vi, NULL, &v->view)) { free(v); return VK_NULL_HANDLE; }
    v->next = t->views; t->views = v;
    return v->view;
}

// ---- descriptor sets: a pool per ring slot, reset when its submission is reused ----
#define POOL_SETS 256   // sets per pool; a submission takes as many pools as it needs, all reset when its ring slot is reused
typedef struct { VkDescriptorPool p[256]; int n, cur; } pool_chain;
static pool_chain g_pools[16]; static int g_pool_slot = -1;
static VkDescriptorPool pool_new(void) {
    VkDescriptorPoolSize ps[5]; uint32_t np = 0;
    static const VkDescriptorType T[5] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                          VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER};
    for (int k = 0; k < 5; k++) if (g_per_set[k]) ps[np++] = (VkDescriptorPoolSize){T[k], g_per_set[k] * POOL_SETS};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = POOL_SETS, .poolSizeCount = np, .pPoolSizes = ps};
    VkDescriptorPool p = VK_NULL_HANDLE; if (vkCreateDescriptorPool(g_vkd.dev, &pi, NULL, &p)) LOG("[RED] vulkan draws: no descriptor pool"); return p;
}
void vkx_sub_begin(int slot, VkCommandBuffer c) {
    if (!draw_init()) return;
    res_init(c);
    pool_chain *pc = &g_pools[slot];   // the slot's last submission is done (vk_replay.c waited for it)
    for (int i = 0; i < pc->n; i++) vkResetDescriptorPool(g_vkd.dev, pc->p[i], 0);
    pc->cur = 0;
    g_pool_slot = slot;
}
// The 32-bit word at byte off of stage st's uniform slot reg (a bindless handle the game wrote there): 0 when unbound or out of range
static uint32_t ubo_word(int st, int reg, int off) {
    if (reg < 0 || reg >= 16 || off < 0) return 0;
    const bbuf *u = &S.ubo[st][reg]; if (!u->addr || (uint64_t)off + 4 > u->size) return 0;
    uint32_t w; memcpy(&w, (const void *)(uintptr_t)(u->addr + (uint64_t)off), 4); return w;
}
static int buf_desc(uint64_t addr, uint64_t size, uint64_t align, uint64_t maxr, VkDescriptorBufferInfo *out) {
    VkBuffer b; VkDeviceSize off; VkDeviceAddress g;
    if (!addr || !vkm_lookup((const void *)(uintptr_t)addr, size ? size : 1, &b, &off, &g)) return 0;
    if (off % align) { g_miss[M_UNALIGNED]++; return 0; }
    *out = (VkDescriptorBufferInfo){b, off, size && size < maxr ? size : maxr};
    return 1;
}
// The descriptor set of one stage's shader for the draw now: 0 when nothing could be allocated
static VkDescriptorSet stage_set(int st, vks *s) {
    VkDescriptorSet set = VK_NULL_HANDLE; pool_chain *pc = &g_pools[g_pool_slot];
    for (;;) {
        if (pc->cur == pc->n) { if (pc->n == 256 || !(pc->p[pc->n] = pool_new())) { static int w; if (!w++) LOG("[RED] vulkan draws: no more descriptor pools"); return VK_NULL_HANDLE; } pc->n++; }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pc->p[pc->cur], .descriptorSetCount = 1, .pSetLayouts = &g_dsl};
        if (!vkAllocateDescriptorSets(g_vkd.dev, &ai, &set)) break;
        pc->cur++;   // this pool is full: the next
    }
    VkWriteDescriptorSet wr[NBIND_MAX]; VkDescriptorBufferInfo bi[NBIND_MAX]; VkDescriptorImageInfo ii[NBIND_MAX]; int nw = 0;
    pthread_mutex_lock(&g_tmu);
    for (int i = 0; i < s->nb; i++) {
        const vkbind *b = &s->bd[i]; VkWriteDescriptorSet *w = &wr[nw];
        *w = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = b->b, .descriptorCount = 1};
        if (b->desc == 'u') {
            w->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w->pBufferInfo = &bi[nw];
            bi[nw] = (VkDescriptorBufferInfo){g_zero, 0, 65536};
            if (!strcmp(b->kind, "support")) bi[nw].buffer = g_support;
            else if (!strcmp(b->kind, "c1")) bi[nw] = (VkDescriptorBufferInfo){s->c1, 0, VK_WHOLE_SIZE};
            else if (!strcmp(b->kind, "cbuf")) { if (!(b->reg >= 0 && b->reg < 16 && buf_desc(S.ubo[st][b->reg].addr, S.ubo[st][b->reg].size, 64, 65536, &bi[nw]))) g_miss[M_UBO]++; }
            else if (strcmp(b->kind, "drvcb")) g_miss[M_OTHER]++;   // drvcb: the driver banks read as zero for now (open: c0's storage buffer table, c2's handles)
        } else if (b->desc == 's') {
            w->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w->pBufferInfo = &bi[nw];
            bi[nw] = (VkDescriptorBufferInfo){g_zero, 0, 65536};
            const bbuf *u = !strcmp(b->kind, "sbuf") && b->reg >= 0 && b->reg < 16 ? &S.ssbo[st][b->reg] : !strcmp(b->kind, "ubo_ptr") && b->reg >= 0 && b->reg < 16 ? &S.ubo[st][b->reg] : NULL;
            if (!(u && buf_desc(u->addr, u->size, 16, 0x7fffffff, &bi[nw]))) g_miss[M_SBUF]++;
        } else if (b->desc == 't') {
            w->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w->pImageInfo = &ii[nw];
            ii[nw] = (VkDescriptorImageInfo){g_dsamp, g_dview[dummy_index(b)], VK_IMAGE_LAYOUT_GENERAL};
            if (b->cnt > 1) { w->descriptorCount = 1; }   // arrays (tex_c2a): element 0 only for now (open)
            uint64_t h = 0; int tid = 0, sid = 0;
            if (!strcmp(b->kind, "tex") && b->reg >= 0 && b->reg < 256) { h = S.tex[st][b->reg]; tid = (int)(h & 0xfffff); }
            else if (!strcmp(b->kind, "tex_ubo")) { uint32_t hw = ubo_word(st, b->reg, b->off); tid = (int)(hw & 0xfffff); if (tid) tid += S.tex_base; h = hw; }
            if (!strcmp(b->skind, "samp_tex")) sid = (int)((h >> 20) & 0xfff);
            else if (!strcmp(b->skind, "samp") && b->sreg >= 0 && b->sreg < 128) sid = (int)(S.samp[st][b->sreg] >> 20);
            else if (!strcmp(b->skind, "samp_id")) sid = b->sreg;
            else if (!strcmp(b->skind, "samp_ubo")) { uint32_t hw = ubo_word(st, b->sreg, b->soff); sid = (int)(hw >> 20); }
            if (sid && strcmp(b->skind, "samp_id") && !strcmp(b->kind, "tex_ubo")) sid += S.samp_base;
            tex_t o; view_t v; int hv;
            if (!tid) g_miss[M_TEX]++;
            else if (!vkr_treg(tid, &o, &v, &hv)) g_miss[M_TEXREG]++;
            else { vtex *t = vkr_tex_sampled(&o, VK_NULL_HANDLE); VkImageView iv = t ? view_get(t, b, &o) : VK_NULL_HANDLE; if (iv) ii[nw].imageView = iv; else g_miss[M_TEX]++; }
            samp_par sp; if (sid && vkr_sreg(sid, &sp)) ii[nw].sampler = sampler_get(&sp); else g_miss[M_SAMP]++;
        } else if (b->desc == 'i') {
            w->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w->pImageInfo = &ii[nw];
            ii[nw] = (VkDescriptorImageInfo){VK_NULL_HANDLE, g_dview[dummy_index(b)], VK_IMAGE_LAYOUT_GENERAL}; g_miss[M_IMG]++;   // open: storage images
        } else if (b->desc == 'b') { g_miss[M_TBUF]++; continue; }   // open: texel buffers (left unwritten: partially bound)
        else { g_miss[M_OTHER]++; continue; }
        nw++;
    }
    pthread_mutex_unlock(&g_tmu);
    if (nw) vkUpdateDescriptorSets(g_vkd.dev, (uint32_t)nw, wr, 0, NULL);
    g_st_sets++;
    return set;
}
// A draw: the program's shaders and their descriptor sets (bound in c). 0 when the draw cannot be made (no program, a shader missing)
int vkx_prepare_draw(VkCommandBuffer c) {
    if (!draw_init() || g_pool_slot < 0 || !S.prog) return 0;
    g_st_draws++;
    vks *sh[5] = {0}; int any = 0;
    for (int st = 0; st < 5; st++) {
        uint8_t k8[8];
        if (!prog_key(S.prog, (unsigned)st, k8)) continue;
        if (!(sh[st] = shader_get(S.prog, st))) { g_miss[M_NOSHADER]++; return 0; }
        any = 1;
    }
    if (!any || !sh[0]) { g_miss[M_NOSHADER]++; return 0; }
    VkDescriptorSet sets[5] = {0};
    for (int st = 0; st < 5; st++) if (sh[st]) { sets[st] = stage_set(st, sh[st]); if (!sets[st]) return 0; }
    for (int st = 0; st < 5; st++) if (sets[st]) vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl_gfx, (uint32_t)st, 1, &sets[st], 0, NULL);
    VkShaderStageFlagBits stages[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT};
    VkShaderEXT objs[5]; for (int st = 0; st < 5; st++) objs[st] = sh[st] ? sh[st]->sh : VK_NULL_HANDLE;
    g_vkd.bind_shaders(c, 5, stages, objs);
    if (getenv("DM_VK_DRAWLOG") && !(g_st_draws & (g_st_draws - 1))) {
        char m[400]; int q = 0; for (int i = 0; i < 10; i++) if (g_miss[i]) q += snprintf(m + q, sizeof m - q, " %s %llu", MISS[i], (unsigned long long)g_miss[i]);
        LOG("vulkan draws: %llu prepared, %llu descriptor sets, %llu shader objects (%llu failed, %llu keys without a translation); missing:%s", (unsigned long long)g_st_draws,
            (unsigned long long)g_st_sets, (unsigned long long)g_st_load, (unsigned long long)g_st_fail, (unsigned long long)g_st_nokey, q ? m : " nothing");
    }
    return 1;
}

// ---- the bound state, from the walker ----
void vkx_bind_program(const prog_t *p) { S.prog = p; }
void vkx_bind_buf(int ssbo, const p_bind_buf *b) { if (b->stage >= 0 && b->stage < NST && b->index >= 0 && b->index < 16) (ssbo ? S.ssbo : S.ubo)[b->stage][b->index] = (bbuf){b->addr, b->size}; }
void vkx_bind_handles(int what, const p_bind_handles *b, const uint64_t *h) {   // what: 0 textures, 1 samplers, 2 images
    if (b->stage < 0 || b->stage >= NST || b->first < 0 || b->n <= 0) return;
    int lim = what == 0 ? 256 : what == 1 ? 128 : 32;
    uint64_t *dst = what == 0 ? S.tex[b->stage] : what == 1 ? S.samp[b->stage] : S.img[b->stage];
    for (int k = 0; k < b->n && b->first + k < lim; k++) dst[b->first + k] = h[k];
}
void vkx_set_pool(int tex, int base) { if (tex) S.tex_base = base; else S.samp_base = base; }
