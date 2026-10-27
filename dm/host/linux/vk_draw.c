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
#include <errno.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/resource.h>
#include <dirent.h>

#define NST 6
typedef struct { uint64_t addr, size; } bbuf;
static struct { bbuf ubo[NST][16], ssbo[NST][16]; uint64_t tex[NST][256], samp[NST][128], img[NST][32]; int tex_base, samp_base; const prog_t *prog[NST]; } S;   // prog: the program bound to each stage (vkx_bind_program)

// ---- shaders ----
#define NBIND_MAX 96
// a binding's kind and sampler kind (the .vkb's names), as numbers for the per-draw descriptor writes
enum { BK_OTHER, BK_SUPPORT, BK_C1, BK_CBUF, BK_DRVCB, BK_SBUF, BK_UBO_PTR, BK_TEX, BK_TEX_UBO };
enum { BS_OTHER, BS_SAMP_TEX, BS_SAMP, BS_SAMP_ID, BS_SAMP_UBO };
typedef struct { uint16_t b; char desc; char kind[12]; int reg, off; char skind[12]; int sreg, soff; uint8_t dim, arr, ms, dep; uint16_t cnt; uint8_t k, sk; } vkbind;
typedef struct vks {
    char key[17]; uint64_t k64; struct vks *next; int stage, ok;
    VkShaderEXT sh; VkShaderStageFlagBits vstage;
    VkShaderEXT sh_gl; uint32_t *spv; size_t nspv; int gl_none;   // the GL-depth variant (spvr_gl_depth), made the first time it is needed from the fitted SPIR-V kept here
    int nb; vkbind bd[NBIND_MAX];
    VkBuffer c1; VkDeviceMemory c1m; uint32_t c1_len;
} vks;
static vks *g_sh[1024]; static pthread_mutex_t g_shmu = PTHREAD_MUTEX_INITIALIZER;
static VkDescriptorSetLayout g_dsl; static VkPipelineLayout g_pl_gfx, g_pl_cs;
static int g_trace;   // the replay's pass log is on (DM_VK_PASSLOG): each draw is logged with its shaders and textures
void vkx_trace(int on) { g_trace = on; }
static int g_depth_zero;   // NVN depth mode: 1 near = 0 (as Vulkan), 0 near = -w (GL, NVN's default): the last stage before rasterization then takes its GL-depth variant
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
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
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
static void shc_init(uint64_t layout_hash); static void precompile_start(void);
static char (*g_keys)[17]; static int g_nkeys;   // every shader key of the set (draw_init)
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
        if (l == 20) { if (!(g_nkeys & 1023)) g_keys = realloc(g_keys, (g_nkeys + 1024) * sizeof *g_keys); memcpy(g_keys[g_nkeys], de->d_name, 16); g_keys[g_nkeys++][16] = 0; }   // <16 hex>.vkb: for the precompiler
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
    { uint64_t lh = 1469598103934665603ull; for (uint32_t i = 0; i < n; i++) { lh = (lh ^ b[i].binding) * 1099511628211ull; lh = (lh ^ (uint64_t)b[i].descriptorType) * 1099511628211ull; lh = (lh ^ b[i].descriptorCount) * 1099511628211ull; }
      shc_init(lh); }   // shader binaries depend on the layout they were made with
    precompile_start();
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
                static const char *const kn[] = {"", "support", "c1", "cbuf", "drvcb", "sbuf", "ubo_ptr", "tex", "tex_ubo"}, *const sn[] = {"", "samp_tex", "samp", "samp_id", "samp_ubo"};
                b->k = BK_OTHER; for (int i = 1; i < 9; i++) if (!strcmp(b->kind, kn[i])) b->k = (uint8_t)i;
                b->sk = BS_OTHER; for (int i = 1; i < 5; i++) if (!strcmp(b->skind, sn[i])) b->sk = (uint8_t)i;
            }
        }
        if (!e) break; p = e + 1;
    }
}
// The i-th shader of a program, made the first time (its key from prog_key, which takes the index in the program's list, not a stage: a compute program is one
// shader at index 0; the shader's stage is the one its translation names, in its .vkb header). NULL when there is none or it cannot be made, logged once per key
static int stage_of_name(const char *s) { static const char *const n[NST] = {"vs", "ps", "gs", "hs", "ds", "cs"}; for (int i = 0; i < NST; i++) if (!strncmp(s, n[i], 2)) return i; return -1; }
// Shader objects made and the time they took (the long-frame log's "new pipelines + shader libraries": shaders as libraries, GL-depth variants as pipelines)
static _Atomic uint64_t g_mk_sh, g_mk_sh_ns, g_mk_var, g_mk_var_ns;
static uint64_t mk_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = atomic_load(&g_mk_sh); *pso = atomic_load(&g_mk_var); }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = atomic_load(&g_mk_sh_ns); *pso_ns = atomic_load(&g_mk_var_ns); }
static int drawlog(void) { static int v = -1; if (v < 0) v = getenv("DM_VK_DRAWLOG") != NULL; return v; }   // DM_VK_DRAWLOG: draw counters at powers of two
// ---- shader objects and their disk cache ----
// A shader's SPIR-V as the device takes it (fitted, outputs initialized), and its stage from the .vkb; with s, the binding table and c1 go into s. NULL when it is not there
static uint32_t *sh_code(const char *key, int *st_out, size_t *nw, vks *s, unsigned *ch_out) {
    char path[1200]; uint8_t *code = NULL, *vkb = NULL, *job = NULL; size_t ncode = 0, nvkb = 0, njob = 0;
    snprintf(path, sizeof path, "%s/%s.vk.spv", g_spv, key); int have = read_file(path, &code, &ncode);
    snprintf(path, sizeof path, "%s/%s.vkb", g_spv, key); have = have && read_file(path, &vkb, &nvkb);
    if (have && s) { snprintf(path, sizeof path, "%s/%s.job.json", g_spv, key); read_file(path, &job, &njob); }
    { const char *ov = getenv("DM_VK_SHOVERRIDE"), *m = ov ? strstr(ov, key) : NULL;   // diagnostic: DM_VK_SHOVERRIDE=<key>=<spv file>[,...] replaces that shader's code
      if (have && m && m[16] == '=') { char f[1024]; size_t n = strcspn(m + 17, ","); if (n >= sizeof f) n = sizeof f - 1; memcpy(f, m + 17, n); f[n] = 0;
          uint8_t *oc = NULL; size_t on = 0; if (read_file(f, &oc, &on)) { free(code); code = oc; ncode = on; if (s) LOG("vulkan draws: shader %s replaced by %s", key, f); } } }
    int st = -1;
    if (have) { const char *h = strchr((const char *)vkb, ' '); h = h ? strchr(h + 1, ' ') : NULL; st = h ? stage_of_name(h + 1) : -1; }   // "# <key> <stage> set <n>"
    if (!have || st < 0) { free(code); free(vkb); free(job); return NULL; }
    if (s) {
        parse_vkb(s, (const char *)vkb);
        const char *c1 = job ? strstr((const char *)job, "\"c1\": \"") : NULL;   // c1: the shader's own constants, from its translation job
        uint8_t *c1d = NULL; size_t c1n = 0;
        if (c1) { c1 += 7; const char *e = strchr(c1, '"'); c1n = e ? (size_t)(e - c1) / 2 : 0; c1d = malloc(c1n ? c1n : 1); for (size_t i = 0; i < c1n; i++) c1d[i] = (uint8_t)hexbyte(c1 + 2 * i); }
        buffer_with(c1d, c1n > 65536 ? 65536 : (c1n ? c1n : 256), &s->c1, &s->c1m); s->c1_len = (uint32_t)c1n; free(c1d);
    }
    spvr_device dev = {0, 0, 0}; size_t fw; unsigned ch = 0;
    uint32_t *fit = spvr_fit((const uint32_t *)code, ncode / 4, &dev, &fw, &ch);
    if (fit && (st == 0 || st == 4)) { size_t iw; uint32_t *io = spvr_init_outputs(fit, fw, &iw); if (io) { free(fit); fit = io; fw = iw; } }   // unwritten output components read 0, as on Maxwell
    free(code); free(vkb); free(job);
    *st_out = st; *nw = fw; if (ch_out) *ch_out = ch;
    return fit;
}
// Shader binaries on disk (VK_EXT_shader_object's binary form): the driver does not cache shader objects, so each one compiled again at every run and every new one in
// play stopped the frame for 5-15 ms (a 1.3 s stop when a level's 120 came at once). Found by key and variant under a directory named by the shader set, the descriptor
// layout and the driver's binary UUID and version (a driver update starts a new one). DM_VK_SHCACHE=<dir> moves it, DM_VK_SHCACHE=0 turns it off
static char g_shc[1400]; static int g_shc_on;
typedef struct { uint32_t magic, version; uint8_t uuid[16]; uint64_t size; } shc_hdr;
static VkPhysicalDeviceShaderObjectPropertiesEXT g_sop = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT};
static _Atomic uint64_t g_shc_hit, g_shc_saved, g_shc_hit_ns;
static void shc_init(uint64_t layout_hash) {
    const char *e = getenv("DM_VK_SHCACHE"); if (e && !strcmp(e, "0")) return;
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &g_sop}; vkGetPhysicalDeviceProperties2(g_vkd.pd, &p2);
    const char *base = strrchr(g_spv, '/'); base = base ? base + 1 : g_spv;
    char root[1024];
    if (e && *e) snprintf(root, sizeof root, "%s", e);
    else { const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME"); if (x && *x) snprintf(root, sizeof root, "%s/nxasahi/vkshaders", x); else if (h) snprintf(root, sizeof root, "%s/.cache/nxasahi/vkshaders", h); else return; }
    char u[33]; for (int i = 0; i < 16; i++) snprintf(u + 2 * i, 3, "%02x", g_sop.shaderBinaryUUID[i]);
    snprintf(g_shc, sizeof g_shc, "%s/%s-%016llx-%s-%u", root, base, (unsigned long long)layout_hash, u, g_sop.shaderBinaryVersion);
    char mk[1400]; snprintf(mk, sizeof mk, "%s", g_shc);   // mkdir -p
    for (char *q = mk + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(mk, 0755); *q = '/'; }
    if (mkdir(mk, 0755) && errno != EEXIST) { LOG("[RED] vulkan draws: no shader cache directory %s", g_shc); return; }
    g_shc_on = 1; LOG("vulkan draws: shader binaries cached in %s", g_shc);
}
static void shc_path(char *out, size_t n, const char *key, int st, int gl) { snprintf(out, n, "%s/%s.%d%d.bin", g_shc, key, st, gl); }
static int shc_has(const char *key, int st, int gl) { char p[1500]; shc_path(p, sizeof p, key, st, gl); return access(p, R_OK) == 0; }
static VkShaderCreateInfoEXT sh_ci(int st, VkShaderCodeTypeEXT type, const void *code, size_t size, const VkDescriptorSetLayout *sets) {
    return (VkShaderCreateInfoEXT){.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT, .stage = vstage_of(st),
        .nextStage = st == 0 ? VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT : st == 2 || st == 4 ? VK_SHADER_STAGE_FRAGMENT_BIT :
                     st == 3 ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT : 0,
        .codeType = type, .codeSize = size, .pCode = code, .pName = "main", .setLayoutCount = st == 5 ? 1 : 5, .pSetLayouts = sets};
}
// The shader object of a key's stage and variant (gl: the GL-depth form of the last stage before rasterization), from the disk cache or compiled (and then cached).
// spv: the fitted code. *compiled: 1 when it had to be compiled
static VkShaderEXT sh_object(const char *key, int st, int gl, const uint32_t *spv, size_t nw, int *compiled) {
    VkDescriptorSetLayout sets[5] = {g_dsl, g_dsl, g_dsl, g_dsl, g_dsl}; VkShaderEXT o = VK_NULL_HANDLE; char p[1500];
    if (compiled) *compiled = 0;
    if (g_shc_on) {
        shc_path(p, sizeof p, key, st, gl); uint8_t *b; size_t n; uint64_t t0 = mk_now();
        if (read_file(p, &b, &n)) {
            shc_hdr h; int ok = n >= sizeof h && (memcpy(&h, b, sizeof h), h.magic == 0x31636873u && h.version == g_sop.shaderBinaryVersion && !memcmp(h.uuid, g_sop.shaderBinaryUUID, 16) && h.size == n - sizeof h);
            if (ok) { VkShaderCreateInfoEXT ci = sh_ci(st, VK_SHADER_CODE_TYPE_BINARY_EXT, b + sizeof h, (size_t)h.size, sets); if (g_vkd.create_shaders(g_vkd.dev, 1, &ci, NULL, &o)) o = VK_NULL_HANDLE; }
            free(b);
            if (o) { atomic_fetch_add(&g_shc_hit, 1); atomic_fetch_add(&g_shc_hit_ns, mk_now() - t0);
                uint64_t hn = atomic_load(&g_shc_hit); if (!(hn & (hn - 1))) LOG("vulkan draws: %llu shaders loaded from the cache, %.2f ms each", (unsigned long long)hn, atomic_load(&g_shc_hit_ns) / 1e6 / hn);
                return o; }
        }
    }
    size_t gw = 0; uint32_t *g = gl ? spvr_gl_depth(spv, nw, &gw) : NULL;
    if (gl && !g) return VK_NULL_HANDLE;
    VkShaderCreateInfoEXT ci = sh_ci(st, VK_SHADER_CODE_TYPE_SPIRV_EXT, g ? g : spv, (g ? gw : nw) * 4, sets);
    VkResult r = g_vkd.create_shaders(g_vkd.dev, 1, &ci, NULL, &o); free(g);
    if (r) return VK_NULL_HANDLE;
    if (compiled) *compiled = 1;
    if (g_shc_on && g_vkd.shader_binary) {   // into the cache: written to a temporary name and renamed, so a reader never sees half a file
        size_t n = 0;
        if (!g_vkd.shader_binary(g_vkd.dev, o, &n, NULL) && n) {
            uint8_t *b = malloc(sizeof(shc_hdr) + n); shc_hdr h = {0x31636873u, g_sop.shaderBinaryVersion, {0}, n}; memcpy(h.uuid, g_sop.shaderBinaryUUID, 16); memcpy(b, &h, sizeof h);
            if (b && !g_vkd.shader_binary(g_vkd.dev, o, &n, b + sizeof h)) {
                char t[1600]; snprintf(t, sizeof t, "%s.%ld.tmp", p, (long)syscall(SYS_gettid)); shc_path(p, sizeof p, key, st, gl);
                FILE *f = fopen(t, "wb"); if (f) { int w = fwrite(b, 1, sizeof h + n, f) == sizeof h + n; w = !fclose(f) && w; if (w && !rename(t, p)) atomic_fetch_add(&g_shc_saved, 1); else unlink(t); }
            }
            free(b);
        }
    }
    return o;
}
static vks *shader_get(const prog_t *p, int idx) {
    uint8_t k8[8]; if (!prog_key(p, (unsigned)idx, k8)) return NULL;
    uint64_t k64; memcpy(&k64, k8, 8);   // found by the key's bytes; its hex form only for a shader made now
    unsigned hsh = (k8[0] | k8[1] << 8) & 1023;
    pthread_mutex_lock(&g_shmu);
    for (vks *s = g_sh[hsh]; s; s = s->next) if (s->k64 == k64) { pthread_mutex_unlock(&g_shmu); return s->ok ? s : NULL; }
    char key[17]; for (int i = 0; i < 8; i++) snprintf(key + 2 * i, 3, "%02x", k8[i]);
    uint64_t mk0 = mk_now();
    vks *s = calloc(1, sizeof *s); snprintf(s->key, sizeof s->key, "%s", key); s->k64 = k64;
    s->next = g_sh[hsh]; g_sh[hsh] = s;
    int st = -1; size_t fw = 0; unsigned ch = 0;
    uint32_t *fit = sh_code(key, &st, &fw, s, &ch);
    if (!fit) { g_st_nokey++; if (g_st_nokey <= 8 || !(g_st_nokey & (g_st_nokey - 1))) LOG("[RED] vulkan draws: no translated shader %s (index %d) in %s (%llu keys without one so far)", key, idx, g_spv, (unsigned long long)g_st_nokey); pthread_mutex_unlock(&g_shmu); return NULL; }
    s->stage = st; s->vstage = vstage_of(st);
    // Stages that can feed the rasterizer are made when a draw asks for a variant (sh_plain / gl_variant): the GL-depth form is the one drawn in a GL-depth game, and
    // making the plain one as well compiled every vertex shader twice
    if (st == 0 || st == 2 || st == 4) { s->spv = fit; s->nspv = fw; s->ok = 1; }
    else { int comp; s->sh = sh_object(key, st, 0, fit, fw, &comp); free(fit); s->ok = s->sh != VK_NULL_HANDLE;
        if (comp || !s->ok) { atomic_fetch_add(&g_mk_sh, 1); atomic_fetch_add(&g_mk_sh_ns, mk_now() - mk0); } }
    if (s->ok) g_st_load++; else { g_st_fail++; LOG("[RED] vulkan draws: shader %s (stage %d) not made", key, st); }
    if (s->ok && !(g_st_load & (g_st_load - 1))) LOG("vulkan draws: %llu shaders loaded (latest %s, stage %d, %d bindings%s)", (unsigned long long)g_st_load, key, st, s->nb,
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
// The components a sampled texture reads: the format's own (formats without alpha read a as 1 and so on, vk_fmt.c), then the texture's swizzle from its builder
// (TextureBuilderSetSwizzle: 3 bits per component, 0 zero 1 one 2 R 3 G 4 B 5 A), then the view's (TextureView SetSwizzle), each picking from what the one before gives
static VkComponentSwizzle swz_pick(int sel, const VkComponentSwizzle base[4]) {
    switch (sel) { case 0: return VK_COMPONENT_SWIZZLE_ZERO; case 1: return VK_COMPONENT_SWIZZLE_ONE; case 2: case 3: case 4: case 5: return base[sel - 2]; default: return VK_COMPONENT_SWIZZLE_IDENTITY; }
}
static VkComponentMapping tex_swizzle(const tex_t *o, const view_t *v, int has_v) {
    VkComponentMapping m; vkf_from_nvn(o->d.format, NULL, &m);
    VkComponentSwizzle c[4] = {m.r == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_R : m.r, m.g == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_G : m.g,
                               m.b == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_B : m.b, m.a == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_A : m.a};
    if (o->set_swz) { VkComponentSwizzle n[4]; for (int i = 0; i < 4; i++) n[i] = swz_pick((o->swz >> (3 * i)) & 7, c); memcpy(c, n, sizeof c); }
    if (has_v && v->set_swizzle) { VkComponentSwizzle n[4]; for (int i = 0; i < 4; i++) n[i] = swz_pick(v->swz[i], c); memcpy(c, n, sizeof c); }
    return (VkComponentMapping){c[0], c[1], c[2], c[3]};
}
// A view of an image as the shader declares it (dim / arrayed from its binding), cached on the image (under g_tmu)
static VkImageView view_get(vtex *t, const vkbind *b, const tex_t *o, const view_t *ov, int has_v) {
    VkImageViewType vt = b->dim == 3 ? VK_IMAGE_VIEW_TYPE_3D : b->dim == 4 ? (b->arr ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE)
                       : b->arr ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    if ((vt == VK_IMAGE_VIEW_TYPE_3D) != (t->d > 1)) return VK_NULL_HANDLE;   // the image is not of this kind
    if ((vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) && t->layers % 6) return VK_NULL_HANDLE;
    VkComponentMapping sw = tex_swizzle(o, ov, has_v);
    uint32_t key = (uint32_t)vt | (uint32_t)sw.r << 4 | (uint32_t)sw.g << 8 | (uint32_t)sw.b << 12 | (uint32_t)sw.a << 16;   // the view type and the components (below 0x80000000: attachment views)
    for (vkview *v = t->views; v; v = v->next) if (v->key == key) return v->view;
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
// Sets made in this ring slot, by a hash of everything written into them: a draw whose stage would get the same descriptors takes the set already made (allocating and
// writing a set per stage per draw was the host's largest cost after the dirty checks). Emptied with the slot's pools
#define SETC 4096
typedef struct { uint64_t h; VkDescriptorSet set; } setc_ent;
static setc_ent g_setc[16][SETC]; static uint64_t g_setc_hit, g_setc_miss;
static pool_chain g_pools[16]; static int g_pool_slot = -1;
static VkDescriptorPool pool_new(void) {
    VkDescriptorPoolSize ps[5]; uint32_t np = 0;
    static const VkDescriptorType T[5] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                          VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER};
    for (int k = 0; k < 5; k++) if (g_per_set[k]) ps[np++] = (VkDescriptorPoolSize){T[k], g_per_set[k] * POOL_SETS};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = POOL_SETS, .poolSizeCount = np, .pPoolSizes = ps};
    VkDescriptorPool p = VK_NULL_HANDLE; if (vkCreateDescriptorPool(g_vkd.dev, &pi, NULL, &p)) LOG("[RED] vulkan draws: no descriptor pool"); return p;
}
static void arena_reset(int slot);
void vkx_sub_begin(int slot, VkCommandBuffer c) {
    if (!draw_init()) return;
    res_init(c);
    pool_chain *pc = &g_pools[slot];   // the slot's last submission is done (vk_replay.c waited for it)
    for (int i = 0; i < pc->n; i++) vkResetDescriptorPool(g_vkd.dev, pc->p[i], 0);
    pc->cur = 0; memset(g_setc[slot], 0, sizeof g_setc[slot]);
    arena_reset(slot);
    g_pool_slot = slot;
}
// ---- the driver banks c0 / c2 (nvn_mtl.m's layout): filled per draw / dispatch from the bound state, for shaders that read them ----
// c0: per stage a storage buffer table (16 slots of address 64 + size 32 bits at base + 16 r: vertex 0x110, compute 0x310, geometry 0x410, pixel 0x510); compute also
// the dispatch size at 0x200 and the uniform buffer table at 0x210 + 16 r; graphics stages the largest point size at 0x610 and the base instance at 0x644.
// c2: the handle tables: combined texture slots from 0x20 (slots 32-39 are the image slots, 0x120), separate textures from 0x168, samplers from 0x568
enum { DRV_SIZE = 0xA00, ARENA = 8 << 20 };
typedef struct { VkBuffer b[64]; VkDeviceMemory m[64]; uint8_t *p[64]; int n, cur; size_t off; } arena_t;
static arena_t g_arena[16]; static uint32_t g_dispatch[3]; static uint32_t g_binst;
static int arena_get(size_t n, VkBuffer *b, VkDeviceSize *off, uint8_t **p) {
    arena_t *a = &g_arena[g_pool_slot];
    for (;;) {
        if (a->cur < a->n && a->off + n <= ARENA) { *b = a->b[a->cur]; *off = a->off; *p = a->p[a->cur] + a->off; a->off += (n + 255) & ~(size_t)255; return 1; }
        if (a->cur < a->n - 1) { a->cur++; a->off = 0; continue; }
        if (a->n == 64) return 0;
        buffer_with(NULL, ARENA, &a->b[a->n], &a->m[a->n]);
        void *mp; vkMapMemory(g_vkd.dev, a->m[a->n], 0, VK_WHOLE_SIZE, 0, &mp); a->p[a->n] = mp;
        a->cur = a->n++; a->off = 0;
    }
}
static uint64_t g_arena_gen[16];   // +1 when the slot's arena is reset (drv_fill's reuse is for the arena's current contents only)
static void arena_reset(int slot) { g_arena[slot].cur = 0; g_arena[slot].off = 0; g_arena_gen[slot]++; }
// The last fill of each stage's bank in this ring slot: a fill with the same bytes reuses it (most draws change none of what the banks hold), which also lets their
// descriptor sets be reused (stage_set)
static struct { int slot; uint64_t gen; VkDescriptorBufferInfo at; uint8_t d[DRV_SIZE]; } g_drv_last[NST][3];
static int drv_fill(int st, int bank, VkDescriptorBufferInfo *out) {
    VkBuffer b; VkDeviceSize off; uint8_t *ad, d[DRV_SIZE];
    memset(d, 0, DRV_SIZE);
    if (bank == 0) {
        static const int base[NST] = {0x110, 0x510, 0x410, -1, -1, 0x310};
        if (base[st] >= 0) for (int r = 0; r < 16; r++) { memcpy(d + base[st] + 16 * r, &S.ssbo[st][r].addr, 8); uint32_t n = (uint32_t)S.ssbo[st][r].size; memcpy(d + base[st] + 16 * r + 8, &n, 4); }
        if (st == 5) { memcpy(d + 0x200, g_dispatch, 12); for (int r = 0; r < 16; r++) { memcpy(d + 0x210 + 16 * r, &S.ubo[st][r].addr, 8); uint32_t n = (uint32_t)S.ubo[st][r].size; memcpy(d + 0x210 + 16 * r + 8, &n, 4); } }
        else { const float maxp = 2047.0f; memcpy(d + 0x610, &maxp, 4); memcpy(d + 0x644, &g_binst, 4); }
    } else if (bank == 2) {
        for (int k = 0; k < 41; k++) { uint64_t h = k >= 32 && k < 40 ? S.img[st][k - 32] : S.tex[st][k]; memcpy(d + 0x20 + 8 * k, &h, 8); }
        for (int k = 0; k < 128; k++) { memcpy(d + 0x168 + 8 * k, &S.tex[st][128 + k], 8); memcpy(d + 0x568 + 8 * k, &S.samp[st][k], 8); }
    }
    int bi = bank == 2 ? 2 : bank == 0 ? 0 : 1;
    if (g_drv_last[st][bi].slot == g_pool_slot && g_drv_last[st][bi].gen == g_arena_gen[g_pool_slot] && !memcmp(g_drv_last[st][bi].d, d, DRV_SIZE)) { *out = g_drv_last[st][bi].at; return 1; }
    if (!arena_get(DRV_SIZE, &b, &off, &ad)) return 0;
    memcpy(ad, d, DRV_SIZE);
    *out = (VkDescriptorBufferInfo){b, off, DRV_SIZE};
    g_drv_last[st][bi].slot = g_pool_slot; g_drv_last[st][bi].gen = g_arena_gen[g_pool_slot]; g_drv_last[st][bi].at = *out; memcpy(g_drv_last[st][bi].d, d, DRV_SIZE);
    return 1;
}
// A storage image view of the texture an image handle names, as the shader declares it (under g_tmu); the texture's content becomes the GPU's
static VkImageView image_view(vtex *t, const vkbind *b) {
    if (!(t->usage & VK_IMAGE_USAGE_STORAGE_BIT)) return VK_NULL_HANDLE;
    VkImageViewType vt = b->dim == 3 ? VK_IMAGE_VIEW_TYPE_3D : (b->arr || b->dim == 4) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;   // a cube is written as its 6 layers
    if ((vt == VK_IMAGE_VIEW_TYPE_3D) != (t->d > 1)) return VK_NULL_HANDLE;
    uint32_t key = 0x40000000u | (uint32_t)vt;
    for (vkview *v = t->views; v; v = v->next) if (v->key == key) return v->view;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img, .viewType = vt, .format = t->fmt,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, vt == VK_IMAGE_VIEW_TYPE_2D || vt == VK_IMAGE_VIEW_TYPE_3D ? 1 : t->layers}};
    vkview *v = calloc(1, sizeof *v); v->key = key;
    if (vkCreateImageView(g_vkd.dev, &vi, NULL, &v->view)) { free(v); return VK_NULL_HANDLE; }
    v->next = t->views; t->views = v; return v->view;
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
static uint64_t mix64(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); h *= 0xff51afd7ed558ccdull; return h ^ (h >> 33); }
static VkDescriptorSet stage_set(int st, vks *s) {
    VkDescriptorSet set = VK_NULL_HANDLE; pool_chain *pc = &g_pools[g_pool_slot];
    VkWriteDescriptorSet wr[NBIND_MAX]; VkDescriptorBufferInfo bi[NBIND_MAX]; VkDescriptorImageInfo ii[NBIND_MAX]; int nw = 0;
    pthread_mutex_lock(&g_tmu);
    for (int i = 0; i < s->nb; i++) {
        const vkbind *b = &s->bd[i]; VkWriteDescriptorSet *w = &wr[nw];
        *w = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = b->b, .descriptorCount = 1};
        if (b->desc == 'u') {
            w->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w->pBufferInfo = &bi[nw];
            bi[nw] = (VkDescriptorBufferInfo){g_zero, 0, 65536};
            if ((b->k == BK_SUPPORT)) bi[nw].buffer = g_support;
            else if ((b->k == BK_C1)) bi[nw] = (VkDescriptorBufferInfo){s->c1, 0, VK_WHOLE_SIZE};
            else if ((b->k == BK_CBUF)) { int ok = b->reg >= 0 && b->reg < 16 && buf_desc(S.ubo[st][b->reg].addr, S.ubo[st][b->reg].size, 64, 65536, &bi[nw]); if (!ok) g_miss[M_UBO]++;
                if (g_trace && b->reg >= 0 && b->reg < 16) { const float *f = (const float *)(uintptr_t)S.ubo[st][b->reg].addr;
                    LOG("[pass]     stage %d cbuf binding %u c%d: addr %#llx size %llu%s; first %g %g %g %g", st, b->b, b->reg, (unsigned long long)S.ubo[st][b->reg].addr, (unsigned long long)S.ubo[st][b->reg].size,
                        ok ? "" : " [NOT BOUND]", f ? f[0] : 0.f, f ? f[1] : 0.f, f ? f[2] : 0.f, f ? f[3] : 0.f); } }
            else if ((b->k == BK_DRVCB)) { if (!drv_fill(st, b->b == 17 ? 2 : b->b == 16 ? 0 : -1, &bi[nw])) g_miss[M_OTHER]++; }   // binding 16 = c0, 17 = c2 (dmsh_spv.py)
            else g_miss[M_OTHER]++;
        } else if (b->desc == 's') {
            w->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w->pBufferInfo = &bi[nw];
            bi[nw] = (VkDescriptorBufferInfo){g_zero, 0, 65536};
            const bbuf *u = (b->k == BK_SBUF) && b->reg >= 0 && b->reg < 16 ? &S.ssbo[st][b->reg] : (b->k == BK_UBO_PTR) && b->reg >= 0 && b->reg < 16 ? &S.ubo[st][b->reg] : NULL;
            int ok = u && buf_desc(u->addr, u->size, 16, 0x7fffffff, &bi[nw]); if (!ok) g_miss[M_SBUF]++;
            if (g_trace) { const float *f = u && u->size >= 24 ? (const float *)(uintptr_t)u->addr : NULL;   // what the buffer holds now (a buffer the GPU writes: as last written)
                LOG("[pass]     stage %d sbuf binding %u s%d: addr %#llx size %llu%s; first %g %g %g %g %g %g", st, b->b, b->reg, u ? (unsigned long long)u->addr : 0ull, u ? (unsigned long long)u->size : 0ull,
                    ok ? "" : " [NOT BOUND]", f ? f[0] : 0.f, f ? f[1] : 0.f, f ? f[2] : 0.f, f ? f[3] : 0.f, f ? f[4] : 0.f, f ? f[5] : 0.f);
                const char *dd = getenv("DM_VK_SBUFDUMP");   // DM_VK_SBUFDUMP=<dir> (with DM_VK_PASSLOG): each storage buffer of the traced frame saved as <dir>/<address>.bin, as it is at bind time
                if (dd && f) { char path[600]; snprintf(path, sizeof path, "%s/%llx.bin", dd, (unsigned long long)u->addr); FILE *o = fopen(path, "wb"); if (o) { fwrite(f, 1, u->size, o); fclose(o); } } }
        } else if (b->desc == 't') {
            w->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w->pImageInfo = &ii[nw];
            ii[nw] = (VkDescriptorImageInfo){g_dsamp, g_dview[dummy_index(b)], VK_IMAGE_LAYOUT_GENERAL};
            if (b->cnt > 1) { w->descriptorCount = 1; }   // arrays (tex_c2a): element 0 only for now (open)
            uint64_t h = 0; int tid = 0, sid = 0;
            if ((b->k == BK_TEX) && b->reg >= 0 && b->reg < 256) { h = S.tex[st][b->reg]; tid = (int)(h & 0xfffff); }
            else if ((b->k == BK_TEX_UBO)) { uint32_t hw = ubo_word(st, b->reg, b->off); tid = (int)(hw & 0xfffff); if (tid) tid += S.tex_base; h = hw; }
            if ((b->sk == BS_SAMP_TEX)) sid = (int)((h >> 20) & 0xfff);
            else if ((b->sk == BS_SAMP) && b->sreg >= 0 && b->sreg < 128) sid = (int)(S.samp[st][b->sreg] >> 20);
            else if ((b->sk == BS_SAMP_ID)) sid = b->sreg;
            else if ((b->sk == BS_SAMP_UBO)) { uint32_t hw = ubo_word(st, b->sreg, b->soff); sid = (int)(hw >> 20); }
            if (sid && (b->sk != BS_SAMP_ID) && (b->k == BK_TEX_UBO)) sid += S.samp_base;
            tex_t o; view_t v; int hv;
            vtex *tt = NULL;
            if (!tid) g_miss[M_TEX]++;
            else if (!vkr_treg(tid, &o, &v, &hv)) g_miss[M_TEXREG]++;
            else { vtex *t = tt = vkr_tex_sampled(&o, VK_NULL_HANDLE); VkImageView iv = t ? view_get(t, b, &o, &v, hv) : VK_NULL_HANDLE; if (iv) ii[nw].imageView = iv; else g_miss[M_TEX]++; }
            if (g_trace) LOG("[pass]     stage %d %s binding %u slot %d: texture ID %d serial %llu %ux%u fmt %#x target %d, sampler ID %d; image of serial %llu, %s, storage %p", st, b->kind, b->b, b->reg, tid,
                             tid && vkr_treg(tid, &o, &v, &hv) ? (unsigned long long)o.serial : 0ull, tid ? o.d.width : 0, tid ? o.d.height : 0, tid ? o.d.format : 0, tid ? o.d.target : -1, sid,
                             tt ? (unsigned long long)tt->serial : 0ull, !tt ? "none" : tt->gpu_owned ? "GPU-written" : tt->uploaded ? "loaded" : "never written", tt ? (const void *)tt->kmem : NULL);
            samp_par sp; if (sid && vkr_sreg(sid, &sp)) ii[nw].sampler = sampler_get(&sp); else g_miss[M_SAMP]++;
        } else if (b->desc == 'i') {
            w->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w->pImageInfo = &ii[nw];
            ii[nw] = (VkDescriptorImageInfo){VK_NULL_HANDLE, g_dview[dummy_index(b)], VK_IMAGE_LAYOUT_GENERAL};
            int tid = b->reg >= 0 && b->reg < 32 ? (int)(S.img[st][b->reg] & 0xfffff) : 0; tex_t o; view_t v; int hv;
            if (tid && vkr_treg(tid, &o, &v, &hv)) { vtex *t = vkr_tex_sampled(&o, VK_NULL_HANDLE); VkImageView iv = t ? image_view(t, b) : VK_NULL_HANDLE;
                if (iv) { ii[nw].imageView = iv; t->gpu_owned = 1; } else g_miss[M_IMG]++;
                if (g_trace) LOG("[pass]     stage %d image binding %u slot %d: texture ID %d serial %llu %ux%u fmt %#x target %d; image of serial %llu", st, b->b, b->reg, tid, (unsigned long long)o.serial, o.d.width, o.d.height, o.d.format, o.d.target, t ? (unsigned long long)t->serial : 0ull);
            } else g_miss[M_IMG]++;
        } else if (b->desc == 'b') { g_miss[M_TBUF]++; continue; }   // open: texel buffers (left unwritten: partially bound)
        else { g_miss[M_OTHER]++; continue; }
        nw++;
    }
    pthread_mutex_unlock(&g_tmu);
    uint64_t h = mix64(0x51ed270b27ull, (uint64_t)(uintptr_t)s);
    for (int i = 0; i < nw; i++) {
        h = mix64(h, (uint64_t)wr[i].dstBinding << 32 | (uint32_t)wr[i].descriptorType);
        if (wr[i].pBufferInfo) { h = mix64(h, (uint64_t)(uintptr_t)wr[i].pBufferInfo->buffer); h = mix64(h, wr[i].pBufferInfo->offset); h = mix64(h, wr[i].pBufferInfo->range); }
        if (wr[i].pImageInfo) { h = mix64(h, (uint64_t)(uintptr_t)wr[i].pImageInfo->sampler); h = mix64(h, (uint64_t)(uintptr_t)wr[i].pImageInfo->imageView); }
    }
    if (!h) h = 1;
    setc_ent *tab = g_setc[g_pool_slot], *e = NULL;
    for (int k = 0; k < 8; k++) { setc_ent *x = &tab[(h + (uint64_t)k) % SETC]; if (x->h == h) { g_setc_hit++; return x->set; } if (!x->h) { e = x; break; } }   // e: where a new one goes
    for (;;) {
        if (pc->cur == pc->n) { if (pc->n == 256 || !(pc->p[pc->n] = pool_new())) { static int w; if (!w++) LOG("[RED] vulkan draws: no more descriptor pools"); return VK_NULL_HANDLE; } pc->n++; }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pc->p[pc->cur], .descriptorSetCount = 1, .pSetLayouts = &g_dsl};
        if (!vkAllocateDescriptorSets(g_vkd.dev, &ai, &set)) break;
        pc->cur++;   // this pool is full: the next
    }
    for (int i = 0; i < nw; i++) wr[i].dstSet = set;
    if (nw) vkUpdateDescriptorSets(g_vkd.dev, (uint32_t)nw, wr, 0, NULL);
    g_st_sets++; g_setc_miss++;
    if (e) *e = (setc_ent){h, set};
    return set;
}
// Background precompiler: every shader of the set into the disk cache, in the form a draw will ask for (the GL-depth form for vertex, geometry and tessellation
// evaluation shaders in a GL-depth game), on low-priority threads while the game runs. The first run fills the cache (about a minute on the M1's large cores for
// Mario Wonder's 12,811); after it no shader is compiled in play. DM_VK_PRECOMPILE=<threads> (2 by default), 0 turns it off
static _Atomic int g_pc_next, g_pc_done; static _Atomic uint64_t g_pc_compiled; static uint64_t g_pc_t0;
static void *precompile_main(void *arg) {
    (void)arg; setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    for (int i; (i = atomic_fetch_add(&g_pc_next, 1)) < g_nkeys;) {
        const char *key = g_keys[i];
        uint8_t *vkb; size_t nv; char path[1200]; snprintf(path, sizeof path, "%s/%s.vkb", g_spv, key);   // the stage first, from the header: no work for what is cached
        if (!read_file(path, &vkb, &nv)) continue;
        const char *h = strchr((const char *)vkb, ' '); h = h ? strchr(h + 1, ' ') : NULL; int st = h ? stage_of_name(h + 1) : -1; free(vkb);
        if (st < 0) continue;
        int gl = (st == 0 || st == 2 || st == 4) && !g_depth_zero;
        if (shc_has(key, st, gl)) continue;
        size_t nw; int st2; uint32_t *spv = sh_code(key, &st2, &nw, NULL, NULL); if (!spv) continue;
        int comp = 0; VkShaderEXT o = sh_object(key, st, gl, spv, nw, &comp); free(spv);
        if (o) g_vkd.destroy_shader(g_vkd.dev, o, NULL);
        if (comp) atomic_fetch_add(&g_pc_compiled, 1);
    }
    if (atomic_fetch_add(&g_pc_done, 1) == 0) LOG("vulkan draws: shader precompile done: %llu compiled into the cache in %.1f s (%llu shaders in the set)",
        (unsigned long long)atomic_load(&g_pc_compiled), (mk_now() - g_pc_t0) / 1e9, (unsigned long long)g_nkeys);
    return NULL;
}
static void precompile_start(void) {
    const char *e = getenv("DM_VK_PRECOMPILE"); int n = e ? atoi(e) : 2;
    if (!g_shc_on || n <= 0 || !g_nkeys) return;
    if (n > 8) n = 8;
    g_pc_t0 = mk_now();
    for (int i = 0; i < n; i++) { pthread_t t; if (!pthread_create(&t, NULL, precompile_main, NULL)) pthread_detach(t); }
    LOG("vulkan draws: precompiling %d shaders into the cache on %d background threads", g_nkeys, n);
}
// The plain and GL-depth forms of a pre-rasterization shader, made the first time a draw asks (under no lock: the submit thread alone makes them)
static VkShaderEXT sh_plain(vks *s) {
    if (s->sh || !s->spv) return s->sh;
    uint64_t mk0 = mk_now(); int comp;
    s->sh = sh_object(s->key, s->stage, 0, s->spv, s->nspv, &comp);
    if (comp) { atomic_fetch_add(&g_mk_sh, 1); atomic_fetch_add(&g_mk_sh_ns, mk_now() - mk0); }
    if (!s->sh) LOG("[RED] vulkan draws: shader %s (stage %d) not made", s->key, s->stage);
    return s->sh;
}
static VkShaderEXT gl_variant(vks *s) {
    if (s->sh_gl) return s->sh_gl;
    if (s->gl_none) return sh_plain(s);
    uint64_t mk0 = mk_now(); int comp;
    s->sh_gl = s->spv ? sh_object(s->key, s->stage, 1, s->spv, s->nspv, &comp) : VK_NULL_HANDLE;
    if (!s->sh_gl) { s->gl_none = 1; static int w; if (!w++) LOG("[RED] vulkan draws: no GL-depth form of shader %s (no gl_Position this pass can find, or not made): drawn with GL depth uncorrected", s->key); return sh_plain(s); }
    if (comp) { atomic_fetch_add(&g_mk_var, 1); atomic_fetch_add(&g_mk_var_ns, mk_now() - mk0); }
    return s->sh_gl;
}
// A draw: the program's shaders and their descriptor sets (bound in c). 0 when the draw cannot be made (no program, a shader missing)
int vkx_prepare_draw(VkCommandBuffer c) {
    if (!draw_init() || g_pool_slot < 0 || !S.prog[0]) return 0;
    g_st_draws++;
    vks *sh[5] = {0};
    for (int st = 0; st < 5; st++) {   // each stage's shader from the program bound to that stage
        const prog_t *p = S.prog[st];
        if (!p) continue;
        uint8_t k8[8];
        for (int i = 0; i < NST && prog_key(p, (unsigned)i, k8); i++) {
            vks *s = shader_get(p, i);
            if (!s) { g_miss[M_NOSHADER]++; return 0; }
            if (s->stage == st) { sh[st] = s; break; }
        }
    }
    if (!sh[0]) { g_miss[M_NOSHADER]++; return 0; }
    VkDescriptorSet sets[5] = {0};
    for (int st = 0; st < 5; st++) if (sh[st]) { sets[st] = stage_set(st, sh[st]); if (!sets[st]) return 0; }
    for (int st = 0; st < 5; st++) if (sets[st]) vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl_gfx, (uint32_t)st, 1, &sets[st], 0, NULL);
    VkShaderStageFlagBits stages[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT};
    VkShaderEXT objs[5]; int last = sh[2] ? 2 : sh[4] ? 4 : 0;
    for (int st = 0; st < 5; st++) objs[st] = !sh[st] ? VK_NULL_HANDLE : st == 0 || st == 2 || st == 4 ? (st == last && !g_depth_zero ? gl_variant(sh[st]) : sh_plain(sh[st])) : sh[st]->sh;   // GL clip depth: the stage that feeds the rasterizer maps it to Vulkan's
    for (int st = 0; st < 5; st++) if (sh[st] && !objs[st]) { g_miss[M_NOSHADER]++; return 0; }
    g_vkd.bind_shaders(c, 5, stages, objs);
    if (drawlog() && !(g_st_draws & (g_st_draws - 1))) {
        char m[400]; int q = 0; for (int i = 0; i < 10; i++) if (g_miss[i]) q += snprintf(m + q, sizeof m - q, " %s %llu", MISS[i], (unsigned long long)g_miss[i]);
        LOG("vulkan draws: %llu prepared, %llu descriptor sets, %llu shader objects (%llu failed, %llu keys without a translation); missing:%s", (unsigned long long)g_st_draws,
            (unsigned long long)g_st_sets, (unsigned long long)g_st_load, (unsigned long long)g_st_fail, (unsigned long long)g_st_nokey, q ? m : " nothing");
    }
    return 1;
}

// ---- the bound state, from the walker ----
// NVN's BindProgram(program, stage bits) binds per stage: the stages in the bits take that program's shaders (a null program unbinds them), the others stay (as the Mac replay's
// bind_stages). Pokémon FireRed binds its vertex and fragment programs apart; replacing one bound program for every stage left its only draw without a vertex shader, and the screen black
void vkx_bind_program(const prog_t *p, int stages) { for (int st = 0; st < NST; st++) if (stages >> st & 1) S.prog[st] = p; }
void vkx_bind_buf(int ssbo, const p_bind_buf *b) { if (b->stage >= 0 && b->stage < NST && b->index >= 0 && b->index < 16) (ssbo ? S.ssbo : S.ubo)[b->stage][b->index] = (bbuf){b->addr, b->size}; }
void vkx_bind_handles(int what, const p_bind_handles *b, const uint64_t *h) {   // what: 0 textures, 1 samplers, 2 images
    if (b->stage < 0 || b->stage >= NST || b->first < 0 || b->n <= 0) return;
    int lim = what == 0 ? 256 : what == 1 ? 128 : 32;
    uint64_t *dst = what == 0 ? S.tex[b->stage] : what == 1 ? S.samp[b->stage] : S.img[b->stage];
    for (int k = 0; k < b->n && b->first + k < lim; k++) dst[b->first + k] = h[k];
}
void vkx_set_pool(int tex, int base) { if (tex) S.tex_base = base; else S.samp_base = base; }

// ---- step 3.4c: the draws ----
// The fixed-function state, as the walker records it (NVN state objects are copied by value into the records: nvn_obj.h's bit fields), set as dynamic state before each draw:
// with shader objects every piece of graphics state is dynamic, so there are no pipelines to make or cache
static struct {
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    float vp[16][6]; int nvp; float dn, df; int has_dr; int sc[4], has_sc;
    int st_vmask[2], st_wmask[2], st_ref[2];
    float pof, pou, poc; float bc[4]; int clamp, restart;
    va_st attr[16]; vs_st stream[16]; NVNbufferRange vbuf[16];
    int origin_ll, depth_zero;
} D = {.df = 1, .chmask = {0xffffffff}, .poly = {.cull = 0, .front = 1, .mode = 2}, .st_vmask = {0xff, 0xff}, .st_wmask = {0xff, 0xff}};
static PFN_vkCmdSetVertexInputEXT p_vin; static PFN_vkCmdSetPolygonModeEXT p_poly; static PFN_vkCmdSetRasterizationSamplesEXT p_rs; static PFN_vkCmdSetSampleMaskEXT p_sm;
static PFN_vkCmdSetAlphaToCoverageEnableEXT p_a2c; static PFN_vkCmdSetColorBlendEnableEXT p_cbe; static PFN_vkCmdSetColorBlendEquationEXT p_cbq; static PFN_vkCmdSetColorWriteMaskEXT p_cwm;
static PFN_vkCmdSetLogicOpEnableEXT p_loe; static PFN_vkCmdSetDepthClampEnableEXT p_dce; static PFN_vkCmdSetAlphaToOneEnableEXT p_a21;
static VkPhysicalDeviceFeatures g_feat; static uint64_t g_st_drawn, g_st_skip[8];
enum { SK_QUADS, SK_FORMAT, SK_VBUF, SK_IBUF, SK_INDIRECT, SK_PREP, SK_U8 };
static const char *const SKIP[] = {"quads", "vertex format", "vertex buffer", "index buffer", "indirect buffer", "shaders", "8-bit indices"};
static void dyn_init(void) {
    if (p_vin) return;
    #define P(v, n) v = (void *)vkGetDeviceProcAddr(g_vkd.dev, n)
    P(p_vin, "vkCmdSetVertexInputEXT"); P(p_poly, "vkCmdSetPolygonModeEXT"); P(p_rs, "vkCmdSetRasterizationSamplesEXT"); P(p_sm, "vkCmdSetSampleMaskEXT");
    P(p_a2c, "vkCmdSetAlphaToCoverageEnableEXT"); P(p_cbe, "vkCmdSetColorBlendEnableEXT"); P(p_cbq, "vkCmdSetColorBlendEquationEXT"); P(p_cwm, "vkCmdSetColorWriteMaskEXT");
    P(p_loe, "vkCmdSetLogicOpEnableEXT"); P(p_dce, "vkCmdSetDepthClampEnableEXT"); P(p_a21, "vkCmdSetAlphaToOneEnableEXT");
    #undef P
    vkGetPhysicalDeviceFeatures(g_vkd.pd, &g_feat);
}
void vkx_bind_state(const p_bind_state *s) {
    switch (s->kind) {
    case 0: { blend_st b; memcpy(&b, s->bytes, sizeof b); if (b.target < 8) D.blend[b.target] = b; break; }
    case 1: memcpy(&D.color, s->bytes, sizeof D.color); break;
    case 2: memcpy(&D.chmask, s->bytes, sizeof D.chmask); break;
    case 3: memcpy(&D.ms, s->bytes, sizeof D.ms); break;
    case 4: memcpy(&D.poly, s->bytes, sizeof D.poly); break;
    case 5: memcpy(&D.ds, s->bytes, sizeof D.ds); break;
    }
}
void vkx_viewports(const p_viewports *v) { for (int i = 0; i < v->n && v->first + i < 16; i++) memcpy(D.vp[v->first + i], v->v[i], sizeof D.vp[0]); if (v->first + v->n > D.nvp) D.nvp = v->first + v->n > 16 ? 16 : v->first + v->n; }
void vkx_scissor(const p_scissor *s) { D.sc[0] = s->x; D.sc[1] = s->y; D.sc[2] = s->w; D.sc[3] = s->h; D.has_sc = 1; }
void vkx_depth_range(const p_depth_range *r) { D.dn = r->n; D.df = r->f; D.has_dr = 1; }
void vkx_stencil(const p_stencil *s) {
    for (int f = 0; f < 2; f++) if (s->face & (f ? 2 : 1)) { if (s->which == 0) D.st_vmask[f] = s->value_mask; else if (s->which == 1) D.st_wmask[f] = s->mask; else D.st_ref[f] = s->ref; }
}
void vkx_poly_offset(const p_poly_offset *p) { D.pof = p->factor; D.pou = p->units; D.poc = p->clamp; }
void vkx_blend_color(const p_blend_color *b) { memcpy(D.bc, b->rgba, sizeof D.bc); }
void vkx_depth_clamp(int on) { D.clamp = on; }
void vkx_prim_restart(int on) { D.restart = on; }
void vkx_vattr(const p_bind_vattr *a) { for (int i = 0; i < a->n && a->first + i < 16; i++) memcpy(&D.attr[a->first + i], &a->st[i], sizeof(va_st)); }
void vkx_vstream(const p_bind_vstream *s) { for (int i = 0; i < s->n && s->first + i < 16; i++) D.stream[s->first + i] = s->st[i]; }
void vkx_vbuf(const p_bind_vbuf *b) { for (int i = 0; i < b->n && b->first + i < 16; i++) D.vbuf[b->first + i] = b->r[i]; }
void mtl_set_origin_lower_left(int on) { D.origin_ll = on; }
int vkx_origin_ll(void) { return D.origin_ll; }   // lower-left origin (NVN's default, GL): the window image is stored bottom-up, so presenting flips it
void mtl_set_depth_zero(int on) { D.depth_zero = on; g_depth_zero = on; }

static VkBlendFactor bfac(int f) {
    switch (f) {
    case 0x1: return VK_BLEND_FACTOR_ZERO; case 0x2: return VK_BLEND_FACTOR_ONE; case 0x3: return VK_BLEND_FACTOR_SRC_COLOR; case 0x4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x5: return VK_BLEND_FACTOR_SRC_ALPHA; case 0x6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; case 0x7: return VK_BLEND_FACTOR_DST_ALPHA; case 0x8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x9: return VK_BLEND_FACTOR_DST_COLOR; case 0xA: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; case 0xB: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR; case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR; case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA; case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp bop(int e) { return e == 2 ? VK_BLEND_OP_SUBTRACT : e == 3 ? VK_BLEND_OP_REVERSE_SUBTRACT : e == 4 ? VK_BLEND_OP_MIN : e == 5 ? VK_BLEND_OP_MAX : VK_BLEND_OP_ADD; }
static VkCompareOp cmpop(int f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }   // NVN NEVER 1 … ALWAYS 8 = Vulkan 0 … 7
static VkStencilOp sop(int o) { static const VkStencilOp m[9] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP,
    VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP}; return o >= 0 && o <= 8 ? m[o] : VK_STENCIL_OP_KEEP; }
// A vertex attribute's format: the texture table's where it has one, else the 3-component and the integer-read-as-float (scaled) layouts Metal had no texture format for
static VkFormat vfmt(int f) {
    switch (f) {
    case 0x19: return VK_FORMAT_R8G8B8_UNORM; case 0x1A: return VK_FORMAT_R8G8B8_SNORM; case 0x1B: return VK_FORMAT_R8G8B8_UINT; case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT; case 0x1E: return VK_FORMAT_R16G16B16_UNORM; case 0x1F: return VK_FORMAT_R16G16B16_SNORM; case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT; case 0x22: return VK_FORMAT_R32G32B32_SFLOAT; case 0x23: return VK_FORMAT_R32G32B32_UINT; case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x51: return VK_FORMAT_R8_USCALED; case 0x52: return VK_FORMAT_R8_SSCALED; case 0x53: return VK_FORMAT_R16_USCALED; case 0x54: return VK_FORMAT_R16_SSCALED;
    case 0x57: return VK_FORMAT_R8G8_USCALED; case 0x58: return VK_FORMAT_R8G8_SSCALED; case 0x59: return VK_FORMAT_R16G16_USCALED; case 0x5A: return VK_FORMAT_R16G16_SSCALED;
    case 0x5D: return VK_FORMAT_R8G8B8_USCALED; case 0x5E: return VK_FORMAT_R8G8B8_SSCALED; case 0x5F: return VK_FORMAT_R16G16B16_USCALED; case 0x60: return VK_FORMAT_R16G16B16_SSCALED;
    case 0x63: return VK_FORMAT_R8G8B8A8_USCALED; case 0x64: return VK_FORMAT_R8G8B8A8_SSCALED; case 0x65: return VK_FORMAT_R16G16B16A16_USCALED; case 0x66: return VK_FORMAT_R16G16B16A16_SSCALED;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32; case 0x6A: return VK_FORMAT_A2B10G10R10_SINT_PACK32; case 0x6B: return VK_FORMAT_A2B10G10R10_USCALED_PACK32; case 0x6C: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    default: return vkf_from_nvn(f, NULL, NULL);
    }
}
static VkPrimitiveTopology topo(int p, int *ok) {
    *ok = 1;
    switch (p) {
    case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST; case 2: case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;   // a line loop drawn open (open)
    case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; case 5: case 8: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;   // a quad strip has the triangle strip's vertex order
    case 6: case 9: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;   // a convex polygon as a fan
    case 0xA: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; case 0xB: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case 0xC: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; case 0xD: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY; case 0xE: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    case 7: *ok = 2; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // quads: two triangles each, through g_quad_ib (vkx_draw)
    default: *ok = 0; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}
// Quads, which Vulkan has not: each quad i drawn as the triangles 4i+{0,1,2} and 4i+{0,2,3}, the order the Mac replay uses, from a fixed index buffer of QUAD_MAX quads; the draw's
// first vertex is the indexed draw's vertex offset. Unity's blits are quads (Pokémon Shining Pearl: half of all draws, its bloom, the scene's composite and the upscale to the window:
// without them its characters were never on screen and the picture stayed 1280x720 in a corner). Open: indexed quads (the guest's indices would need expanding) and indirect quads
enum { QUAD_MAX = 65536 };
static VkBuffer g_quad_ib; static VkDeviceMemory g_quad_m;
static int quad_ib(void) {
    if (g_quad_ib) return 1;
    uint32_t *ix = malloc(sizeof *ix * 6 * QUAD_MAX); if (!ix) return 0;
    for (uint32_t q = 0; q < QUAD_MAX; q++) { uint32_t *t = ix + 6 * q, v = 4 * q; t[0] = v; t[1] = v + 1; t[2] = v + 2; t[3] = v; t[4] = v + 2; t[5] = v + 3; }
    buffer_with(ix, sizeof *ix * 6 * QUAD_MAX, &g_quad_ib, &g_quad_m); free(ix);
    return g_quad_ib != VK_NULL_HANDLE;
}
// Everything a draw needs set, then the draw (inside the render pass vk_replay.c began). ncolor / rt_h: the render pass's colour attachments and height
static void set_state(VkCommandBuffer c, int ncolor, uint32_t rt_w, uint32_t rt_h, VkPrimitiveTopology tp, int samples) {
    VkViewport vp[16]; int nvp = D.nvp ? D.nvp : 1;
    for (int i = 0; i < nvp; i++) {
        float x = D.nvp ? D.vp[i][0] : 0, y = D.nvp ? D.vp[i][1] : 0, w = D.nvp ? D.vp[i][2] : (float)rt_w, h = D.nvp ? D.vp[i][3] : (float)rt_h;
        float n = D.dn, f = D.df;   // SetDepthRange(s), NVN's default 0..1 until then: the viewport records' near / far are never filled (0..0 put every fragment at depth 0; Pokémon Shining Pearl's characters were not drawn)
        if (w <= 0) w = 1;
        // NVN upper-left origin (D3D, Metal): NDC +1 is the target's first row, Vulkan puts -1 there: a negative height flips it. Lower-left (GL) already agrees with Vulkan
        vp[i] = D.origin_ll ? (VkViewport){x, y, w, h, n, f} : (VkViewport){x, y + h, w, -h, n, f};
    }
    VkRect2D sc[16];
    for (int i = 0; i < nvp; i++) {
        if (D.has_sc && D.sc[2] > 0 && D.sc[3] > 0) {
            int sy = D.origin_ll ? (int)rt_h - D.sc[1] - D.sc[3] : D.sc[1];
            sc[i] = (VkRect2D){{D.sc[0] < 0 ? 0 : D.sc[0], sy < 0 ? 0 : sy}, {(uint32_t)D.sc[2], (uint32_t)D.sc[3]}};
        } else sc[i] = (VkRect2D){{0, 0}, {rt_w, rt_h}};
    }
    vkCmdSetViewportWithCount(c, (uint32_t)nvp, vp); vkCmdSetScissorWithCount(c, (uint32_t)nvp, sc);
    vkCmdSetRasterizerDiscardEnable(c, VK_FALSE);
    p_rs(c, (VkSampleCountFlagBits)(samples > 1 ? samples : 1));
    VkSampleMask sm = 0xffffffff; p_sm(c, (VkSampleCountFlagBits)(samples > 1 ? samples : 1), &sm);
    p_a2c(c, D.ms.a2c ? VK_TRUE : VK_FALSE); if (g_feat.alphaToOne) p_a21(c, VK_FALSE);
    p_poly(c, D.poly.mode == 0 ? VK_POLYGON_MODE_POINT : D.poly.mode == 1 && g_feat.fillModeNonSolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL);
    vkCmdSetLineWidth(c, 1.0f);
    static int nocull = -1; if (nocull < 0) nocull = getenv("DM_VK_NOCULL") != NULL;   // diagnostic: no face culling
    vkCmdSetCullMode(c, nocull ? VK_CULL_MODE_NONE : D.poly.cull == 1 ? VK_CULL_MODE_FRONT_BIT : D.poly.cull == 2 ? VK_CULL_MODE_BACK_BIT : D.poly.cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE);
    // Facing: with the upper-left origin the flipped viewport puts NVN's picture on the same memory rows Vulkan does, so the winding agrees as given; with GL's
    // lower-left origin NVN judges winding in y-up window coordinates while Vulkan's framebuffer runs y down: it turns over (the title screen's final quad, culled
    // as a back face, came out black when the upper-left case was the one turned)
    int ccw = D.poly.front == 1; if (D.origin_ll) ccw = !ccw;
    vkCmdSetFrontFace(c, ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    VkCompareOp dop = cmpop((int)D.ds.df);
    vkCmdSetDepthTestEnable(c, D.ds.dt); vkCmdSetDepthWriteEnable(c, D.ds.dw); vkCmdSetDepthCompareOp(c, dop);
    vkCmdSetDepthBoundsTestEnable(c, VK_FALSE); vkCmdSetDepthBounds(c, 0, 1);
    vkCmdSetStencilTestEnable(c, D.ds.st);
    vkCmdSetStencilOp(c, VK_STENCIL_FACE_FRONT_BIT, sop((int)D.ds.fail_f), sop((int)D.ds.zpass_f), sop((int)D.ds.zfail_f), cmpop((int)D.ds.sf_f));
    vkCmdSetStencilOp(c, VK_STENCIL_FACE_BACK_BIT, sop((int)D.ds.fail_b), sop((int)D.ds.zpass_b), sop((int)D.ds.zfail_b), cmpop((int)D.ds.sf_b));
    for (int f = 0; f < 2; f++) { VkStencilFaceFlags fl = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        vkCmdSetStencilCompareMask(c, fl, (uint32_t)D.st_vmask[f]); vkCmdSetStencilWriteMask(c, fl, (uint32_t)D.st_wmask[f]); vkCmdSetStencilReference(c, fl, (uint32_t)D.st_ref[f]); }
    int bias = D.poly.offset_en != 0; vkCmdSetDepthBiasEnable(c, bias); vkCmdSetDepthBias(c, D.pou, g_feat.depthBiasClamp ? D.poc : 0, D.pof);
    vkCmdSetPrimitiveTopology(c, tp); vkCmdSetPrimitiveRestartEnable(c, D.restart ? VK_TRUE : VK_FALSE);
    if (g_feat.depthClamp) p_dce(c, D.clamp ? VK_TRUE : VK_FALSE);
    if (g_feat.logicOp) p_loe(c, VK_FALSE);   // open: NVN logic ops
    vkCmdSetBlendConstants(c, D.bc);
    if (ncolor) {
        VkBool32 en[8]; VkColorBlendEquationEXT eq[8]; VkColorComponentFlags wm[8];
        for (int i = 0; i < ncolor; i++) {
            const blend_st *b = &D.blend[i];
            en[i] = (D.color.blend_en >> i) & 1;
            eq[i] = (VkColorBlendEquationEXT){bfac((int)b->src_rgb), bfac((int)b->dst_rgb), bop((int)b->eq_rgb), bfac((int)b->src_a), bfac((int)b->dst_a), bop((int)b->eq_a)};
            if (!b->src_rgb) eq[i] = (VkColorBlendEquationEXT){VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};   // never set: NVN's default
            wm[i] = (D.chmask.mask >> (4 * i)) & 0xF;   // r 1, g 2, b 4, a 8 as Vulkan's bits
        }
        p_cbe(c, 0, (uint32_t)ncolor, en); p_cbq(c, 0, (uint32_t)ncolor, eq); p_cwm(c, 0, (uint32_t)ncolor, wm);
    }
}
// Vertex input: the streams and attributes as bound (all of them: the shader takes what it reads), and the streams' buffers. 0 when a buffer is not in GPU-visible memory
static int set_vertex(VkCommandBuffer c) {
    VkVertexInputBindingDescription2EXT bd[16]; VkVertexInputAttributeDescription2EXT ad[16]; uint32_t nb = 0, na = 0; uint16_t used = 0;
    for (int i = 0; i < 16; i++) {
        const va_st *a = &D.attr[i]; if (!a->fmt) continue;
        VkFormat f = vfmt((int)a->fmt); if (!f) { g_st_skip[SK_FORMAT]++; continue; }
        static int8_t vok[256];   // 1 the device fetches the format, -1 it does not (logged once)
        if (a->fmt < 256 && !vok[a->fmt]) {
            VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vkd.pd, f, &fp);
            vok[a->fmt] = (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : -1;
            if (vok[a->fmt] < 0) LOG("[RED] vulkan draws: vertex format NVN %#x (vk %d) is not a vertex format on this device", (unsigned)a->fmt, (int)f);
        }
        ad[na++] = (VkVertexInputAttributeDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, .location = (uint32_t)i, .binding = a->stream, .format = f, .offset = a->offset};
        used |= (uint16_t)(1u << a->stream);
    }
    VkBuffer bufs[16]; VkDeviceSize offs[16], sizes[16], strides[16];
    for (int s = 0; s < 16; s++) {
        if (!(used >> s & 1)) continue;
        bd[nb++] = (VkVertexInputBindingDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = (uint32_t)s, .stride = D.stream[s].stride,
            .inputRate = D.stream[s].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1};
    }
    p_vin(c, nb, bd, na, ad);
    for (int s = 0; s < 16; s++) {
        if (!(used >> s & 1)) continue;
        VkDeviceAddress g; VkBuffer b; VkDeviceSize o;
        if (!D.vbuf[s].address || !vkm_lookup((const void *)(uintptr_t)D.vbuf[s].address, D.vbuf[s].size ? D.vbuf[s].size : 1, &b, &o, &g)) { g_st_skip[SK_VBUF]++; return 0; }
        // bound one at a time (the streams used need not be contiguous)
        bufs[0] = b; offs[0] = o; sizes[0] = D.vbuf[s].size ? D.vbuf[s].size : VK_WHOLE_SIZE; strides[0] = D.stream[s].stride;
        vkCmdBindVertexBuffers2(c, (uint32_t)s, 1, bufs, offs, sizes, strides);
    }
    return 1;
}
static int lookup_buf(uint64_t addr, uint64_t len, VkBuffer *b, VkDeviceSize *o) { VkDeviceAddress g; return addr && vkm_lookup((const void *)(uintptr_t)addr, len ? len : 1, b, o, &g); }
// A draw record (OP_DRAW / OP_DRAW_IDX), inside the render pass; ncolor, rt_w, rt_h, samples: the pass's colour attachments, size and sample count
void vkx_draw(VkCommandBuffer c, uint32_t op, const void *a, uint32_t size, int ncolor, uint32_t rt_w, uint32_t rt_h, int samples) {
    if (!draw_init()) return;
    dyn_init();
    p_draw td; p_draw_idx ti; int ok;
    const p_draw *d = op == OP_DRAW ? draw_rec(a, size, &td) : NULL;
    const p_draw_idx *x = op == OP_DRAW_IDX ? draw_idx_rec(a, size, &ti) : NULL;
    VkPrimitiveTopology tp = topo(d ? d->prim : x->prim, &ok);
    if (!ok || (ok == 2 && (x || d->ind || d->count > 4 * QUAD_MAX || !quad_ib()))) { g_st_skip[SK_QUADS]++; return; }
    if ((d ? d->mdi_cnt : x->mdi_cnt)) { g_st_skip[SK_INDIRECT]++; return; }   // open: indirect-count multi-draws
    if (g_trace) { uint8_t k[8]; char ks[2][17] = {"-", "-"}; for (int st = 0; st < 2; st++) if (S.prog[st] && prog_key(S.prog[st], (unsigned)(S.prog[st] == S.prog[0] ? st : 0), k)) for (int i = 0; i < 8; i++) snprintf(ks[st] + 2 * i, 3, "%02x", k[i]);
        LOG("[pass]   draw%s prim %d count %d instances %d vs %s ps %s, blend %#x mask %#x, depth test %d write %d func %d, stencil %d func %d ref %d, cull %d", x ? " indexed" : "", d ? d->prim : x->prim, d ? d->count : x->count,
            d ? d->instances : x->instances, ks[0], ks[1], D.color.blend_en, D.chmask.mask, (int)D.ds.dt, (int)D.ds.dw, (int)D.ds.df, (int)D.ds.st, (int)D.ds.sf_f, D.st_ref[0], (int)D.poly.cull);
        LOG("[pass]     viewports %d: %.1f %.1f %.1f %.1f depth %.2f..%.2f%s; scissor %s %d %d %d %d; front %s; origin %s; depth mode %s; attrs %#x", D.nvp, D.vp[0][0], D.vp[0][1], D.vp[0][2], D.vp[0][3],
            D.dn, D.df, D.has_dr ? " (depth range)" : "", D.has_sc ? "on" : "off", D.sc[0], D.sc[1], D.sc[2], D.sc[3], D.poly.front ? "ccw" : "cw",
            D.origin_ll ? "lower left" : "upper left", D.depth_zero ? "zero" : "gl", ({ unsigned m = 0; for (int i = 0; i < 16; i++) if (D.attr[i].fmt) m |= 1u << i; m; }));
        for (int i = 0; i < 16; i++) if (D.attr[i].fmt) LOG("[pass]     attr %d: NVN format %#x (vk %d) offset %u stream %u; stream stride %u divisor %u, buffer %#llx size %llu", i, (unsigned)D.attr[i].fmt, (int)vfmt((int)D.attr[i].fmt),
            (unsigned)D.attr[i].offset, (unsigned)D.attr[i].stream, D.stream[D.attr[i].stream].stride, D.stream[D.attr[i].stream].divisor, (unsigned long long)D.vbuf[D.attr[i].stream].address, (unsigned long long)D.vbuf[D.attr[i].stream].size); }
    static int slow = -1; if (slow < 0) slow = getenv("DM_VK_SLOWDRAW") != NULL;   // diagnostic: draws whose recording took over 1 ms, by part
    uint64_t sd0 = slow ? mk_now() : 0;
    if (!vkx_prepare_draw(c)) { g_st_skip[SK_PREP]++; return; }
    uint64_t sd1 = slow ? mk_now() : 0;
    set_state(c, ncolor, rt_w, rt_h, tp, samples);
    if (!set_vertex(c)) return;
    if (d) {
        if (ok == 2) { vkCmdBindIndexBuffer(c, g_quad_ib, 0, VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(c, (uint32_t)d->count / 4 * 6, (uint32_t)(d->instances ? d->instances : 1), 0, (int32_t)d->first, (uint32_t)d->base_instance); }
        else if (d->ind) { VkBuffer b; VkDeviceSize o; if (!lookup_buf(d->ind, 16, &b, &o)) { g_st_skip[SK_INDIRECT]++; return; } vkCmdDrawIndirect(c, b, o, 1, 16); }
        else vkCmdDraw(c, (uint32_t)d->count, (uint32_t)(d->instances ? d->instances : 1), (uint32_t)d->first, (uint32_t)d->base_instance);
    } else {
        static const VkIndexType it[3] = {VK_INDEX_TYPE_UINT8_KHR, VK_INDEX_TYPE_UINT16, VK_INDEX_TYPE_UINT32};
        if (x->itype == 0 && !g_vkd.index_u8) { g_st_skip[SK_U8]++; return; }   // open: 8-bit indices on a device without them
        VkBuffer b; VkDeviceSize o;
        if (!lookup_buf(x->ibuf, 1, &b, &o)) { g_st_skip[SK_IBUF]++; return; }
        vkCmdBindIndexBuffer(c, b, o, it[x->itype > 2 ? 1 : x->itype]);
        if (x->ind) { VkBuffer ib; VkDeviceSize io; if (!lookup_buf(x->ind, 20, &ib, &io)) { g_st_skip[SK_INDIRECT]++; return; } vkCmdDrawIndexedIndirect(c, ib, io, 1, 20); }
        else vkCmdDrawIndexed(c, (uint32_t)x->count, (uint32_t)(x->instances ? x->instances : 1), 0, x->base_vertex, (uint32_t)x->base_instance);
    }
    if (slow) { uint64_t sd2 = mk_now(); if (sd2 - sd0 > 1000000) LOG("[slowdraw] %.2f ms: shaders + sets %.2f, state + draw %.2f", (sd2 - sd0) / 1e6, (sd1 - sd0) / 1e6, (sd2 - sd1) / 1e6); }
    g_st_drawn++;
    if (drawlog() && !(g_st_drawn & (g_st_drawn - 1))) {
        char m[300]; int q = 0; for (int i = 0; i < 7; i++) if (g_st_skip[i]) q += snprintf(m + q, sizeof m - q, " %s %llu", SKIP[i], (unsigned long long)g_st_skip[i]);
        LOG("vulkan draws: %llu drawn; skipped:%s", (unsigned long long)g_st_drawn, q ? m : " none");
    }
}

// ---- compute dispatches ----
static uint64_t g_st_dispatched, g_st_dskip;
void vkx_dispatch(VkCommandBuffer c, const p_dispatch *p) {
    static uint64_t why[4];
    #define DSKIP(k) do { g_st_dskip++; why[k]++; if (!(g_st_dskip & (g_st_dskip - 1))) LOG("[RED] vulkan draws: %llu dispatches skipped (no program %llu, no compute shader %llu, indirect buffer %llu, set %llu)", (unsigned long long)g_st_dskip, (unsigned long long)why[0], (unsigned long long)why[1], (unsigned long long)why[2], (unsigned long long)why[3]); return; } while (0)
    if (!draw_init() || g_pool_slot < 0 || !S.prog[5]) DSKIP(0);
    vks *s = NULL; uint8_t k8[8];
    for (int i = 0; i < NST && !s && prog_key(S.prog[5], (unsigned)i, k8); i++) { vks *t = shader_get(S.prog[5], i); if (t && t->stage == 5) s = t; }
    if (!s) DSKIP(1);
    if (g_trace) LOG("[pass]   dispatch %s groups %d %d %d%s", s->key, (int)p->x, (int)p->y, (int)p->z, p->ind ? " (indirect)" : "");
    VkBuffer ib = VK_NULL_HANDLE; VkDeviceSize io = 0;
    if (p->ind) {   // DispatchComputeIndirect: the group counts are the GPU's; c0's copy of them reads as written at record time (0 here)
        VkDeviceAddress g; if (!vkm_lookup((const void *)(uintptr_t)p->ind, 12, &ib, &io, &g)) DSKIP(2);
        memset(g_dispatch, 0, sizeof g_dispatch);
    } else { g_dispatch[0] = (uint32_t)p->x; g_dispatch[1] = (uint32_t)p->y; g_dispatch[2] = (uint32_t)p->z; }
    VkDescriptorSet set = stage_set(5, s); if (!set) DSKIP(3);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl_cs, 0, 1, &set, 0, NULL);
    VkShaderStageFlagBits st = VK_SHADER_STAGE_COMPUTE_BIT; g_vkd.bind_shaders(c, 1, &st, &s->sh);
    if (p->ind) vkCmdDispatchIndirect(c, ib, io); else vkCmdDispatch(c, (uint32_t)p->x, (uint32_t)p->y, (uint32_t)p->z);
    g_st_dispatched++;
    if (drawlog() && !(g_st_dispatched & (g_st_dispatched - 1))) LOG("vulkan draws: %llu dispatches, %llu skipped", (unsigned long long)g_st_dispatched, (unsigned long long)g_st_dskip);
}
