// Compile every translated shader (dm/gfx/tools/dmsh_spv.py output) on the Vulkan device, one stage at a time through VK_EXT_shader_object, with descriptor set
// layouts and push constant ranges from its own reflection (linux/spv_reflect.c). The driver's whole compiler runs (NIR, the GPU backend): a shader that fails
// here would fail as a pipeline in the game. Each thread writes the key it is compiling to <out>/t_spv_compile.<n>.cur first, so a driver crash names its shader.
// Usage: t_spv_compile <spv dir> [threads]   Build: make -f Makefile.linux t_spv_compile SPV=<spv dir>
#define _GNU_SOURCE
#include "spv_reflect.h"
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static VkDevice g_dev; static PFN_vkCreateShadersEXT p_create; static PFN_vkDestroyShaderEXT p_destroy;
static char **g_files; static int g_n; static _Atomic int g_next, g_ok, g_fail, g_rfail; static const char *g_dir;
static _Atomic int g_stage_ok[6], g_stage_n[6], g_fit[4]; static spvr_device g_devf; static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int stage_ix(VkShaderStageFlagBits s) { switch (s) { case VK_SHADER_STAGE_VERTEX_BIT: return 0; case VK_SHADER_STAGE_FRAGMENT_BIT: return 1; case VK_SHADER_STAGE_COMPUTE_BIT: return 2;
    case VK_SHADER_STAGE_GEOMETRY_BIT: return 3; case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return 4; default: return 5; } }
static const char *const STAGE[6] = {"vertex", "fragment", "compute", "geometry", "tess control", "other"};

static uint32_t *slurp(const char *p, size_t *words) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *b = malloc((size_t)n); if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f); *words = (size_t)n / 4; return b;
}
static int one(const char *name, char *why, size_t cap) {
    char path[4096]; snprintf(path, sizeof path, "%s/%s", g_dir, name);
    size_t words; uint32_t *raw = slurp(path, &words); if (!raw) { snprintf(why, cap, "unreadable"); return -2; }
    unsigned ch; size_t fw; uint32_t *code = spvr_fit(raw, words, &g_devf, &fw, &ch); free(raw);   // fitted to the device, as the backend loads it
    if (!code) { snprintf(why, cap, "spvr_fit failed"); return -2; }
    words = fw; for (int b = 0; b < 4; b++) if (ch & (1u << b)) g_fit[b]++;
    spvr_info r; if (spvr_reflect(code, words, &r, why, cap)) { free(code); return -2; }
    // one set layout per set number up to the highest used; every binding visible to every stage, a runtime array as 1024 partially bound descriptors
    uint32_t nset = 0; for (int i = 0; i < r.nbind; i++) if (r.bind[i].set + 1 > nset) nset = r.bind[i].set + 1;
    VkDescriptorSetLayout sl[8] = {0}; if (nset > 8) { snprintf(why, cap, "%u descriptor sets", nset); free(code); return -2; }
    int rc = 0;
    for (uint32_t s = 0; s < nset && !rc; s++) {
        VkDescriptorSetLayoutBinding b[SPVR_MAX_BIND]; VkDescriptorBindingFlags bf[SPVR_MAX_BIND]; uint32_t nb = 0;
        for (int i = 0; i < r.nbind; i++) if (r.bind[i].set == s) {
            b[nb] = (VkDescriptorSetLayoutBinding){r.bind[i].binding, r.bind[i].type, r.bind[i].count ? r.bind[i].count : 1024, VK_SHADER_STAGE_ALL, NULL};
            bf[nb++] = r.bind[i].count ? 0 : VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = nb, .pBindingFlags = bf};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &fi, .bindingCount = nb, .pBindings = b};
        VkResult vr = vkCreateDescriptorSetLayout(g_dev, &li, NULL, &sl[s]);
        if (vr) { snprintf(why, cap, "set layout %u: VkResult %d", s, vr); rc = -1; }
    }
    VkPushConstantRange pc = {VK_SHADER_STAGE_ALL, 0, (r.push_size + 3) & ~3u};
    VkShaderCreateInfoEXT ci = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT, .stage = r.stage,
        .nextStage = r.stage == VK_SHADER_STAGE_VERTEX_BIT ? VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_GEOMETRY_BIT : r.stage == VK_SHADER_STAGE_GEOMETRY_BIT ? VK_SHADER_STAGE_FRAGMENT_BIT : 0,
        .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT, .codeSize = words * 4, .pCode = code, .pName = "main",
        .setLayoutCount = nset, .pSetLayouts = sl, .pushConstantRangeCount = r.push_size ? 1 : 0, .pPushConstantRanges = &pc};
    if (!rc) {
        VkShaderEXT sh = VK_NULL_HANDLE; VkResult vr = p_create(g_dev, 1, &ci, NULL, &sh);
        if (vr) { snprintf(why, cap, "vkCreateShadersEXT: VkResult %d", vr); rc = -1; }
        if (sh) p_destroy(g_dev, sh, NULL);
    }
    for (uint32_t s = 0; s < nset; s++) if (sl[s]) vkDestroyDescriptorSetLayout(g_dev, sl[s], NULL);
    int ix = stage_ix(r.stage); g_stage_n[ix]++; if (!rc) g_stage_ok[ix]++;
    free(code); return rc;
}
static void *worker(void *arg) {
    int me = (int)(intptr_t)arg; char cur[4200]; snprintf(cur, sizeof cur, "%s/t_spv_compile.%d.cur", g_dir, me);
    for (int i; (i = atomic_fetch_add(&g_next, 1)) < g_n;) {
        FILE *f = fopen(cur, "w"); if (f) { fputs(g_files[i], f); fclose(f); }
        char why[256] = ""; int r = one(g_files[i], why, sizeof why);
        if (!r) g_ok++; else { if (r == -2) g_rfail++; else g_fail++; pthread_mutex_lock(&g_mu); printf("FAIL %s: %s\n", g_files[i], why); pthread_mutex_unlock(&g_mu); }
        if (i && i % 2000 == 0) { pthread_mutex_lock(&g_mu); printf("  %d / %d\n", i, g_n); fflush(stdout); pthread_mutex_unlock(&g_mu); }
    }
    remove(cur); return NULL;
}
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: t_spv_compile <spv dir> [threads]\n"); return 2; }
    g_dir = argv[1]; int nt = argc > 2 ? atoi(argv[2]) : 8;
    DIR *d = opendir(g_dir); if (!d) { perror(g_dir); return 2; }
    int cap = 1024; g_files = malloc(cap * sizeof *g_files);
    for (struct dirent *e; (e = readdir(d));) { size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".spv") && !strstr(e->d_name, ".reader.")) { if (g_n == cap) g_files = realloc(g_files, (cap *= 2) * sizeof *g_files); g_files[g_n++] = strdup(e->d_name); } }
    closedir(d); qsort(g_files, (size_t)g_n, sizeof *g_files, by_name);
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "t_spv_compile", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance inst; if (vkCreateInstance(&ii, NULL, &inst)) { printf("FAIL: no Vulkan instance\n"); return 1; }
    uint32_t np = 8; VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(inst, &np, pds);
    VkPhysicalDevice pd = VK_NULL_HANDLE; VkPhysicalDeviceProperties pp;
    for (uint32_t i = 0; i < np; i++) { vkGetPhysicalDeviceProperties(pds[i], &pp); if (pp.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) { pd = pds[i]; break; } }
    if (!pd) { printf("FAIL: no GPU\n"); return 1; }
    // every feature the device has, plus shader objects
    VkPhysicalDeviceShaderObjectFeaturesEXT so = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &so};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    vkGetPhysicalDeviceFeatures2(pd, &f);
    if (!so.shaderObject) { printf("FAIL: %s has no shader objects\n", pp.deviceName); return 1; }
    g_devf = (spvr_device){f.features.shaderFloat64, 0, f.features.vertexPipelineStoresAndAtomics};   // no VK_EXT_fragment_shader_interlock here (not enabled)
    printf("device %s; storage images without format: read %d write %d; shaderFloat64 %d; geometry %d\n", pp.deviceName, f.features.shaderStorageImageReadWithoutFormat,
           f.features.shaderStorageImageWriteWithoutFormat, f.features.shaderFloat64, f.features.geometryShader);
    float prio = 1; VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
    const char *ext[] = {VK_EXT_SHADER_OBJECT_EXTENSION_NAME};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .enabledExtensionCount = 1, .ppEnabledExtensionNames = ext};
    VkResult vr = vkCreateDevice(pd, &di, NULL, &g_dev); if (vr) { printf("FAIL: vkCreateDevice %d\n", vr); return 1; }
    p_create = (PFN_vkCreateShadersEXT)vkGetDeviceProcAddr(g_dev, "vkCreateShadersEXT"); p_destroy = (PFN_vkDestroyShaderEXT)vkGetDeviceProcAddr(g_dev, "vkDestroyShaderEXT");
    printf("compiling %d shaders on %d threads\n", g_n, nt); fflush(stdout);
    pthread_t th[64]; if (nt > 64) nt = 64;
    for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
    for (int s = 0; s < 6; s++) if (g_stage_n[s]) printf("  %-12s %d / %d\n", STAGE[s], g_stage_ok[s], g_stage_n[s]);
    printf("  fitted to the device: Float64 dropped %d, interlock dropped %d, buffers marked NonWritable %d; writing a vertex-stage buffer the device cannot %d\n", (int)g_fit[0], (int)g_fit[1], (int)g_fit[2], (int)g_fit[3]);
    printf("%s: %d of %d compiled; %d failed to compile, %d not read\n", g_fail || g_rfail ? "FAIL" : "PASS", (int)g_ok, g_n, (int)g_fail, (int)g_rfail);
    vkDestroyDevice(g_dev, NULL); vkDestroyInstance(inst, NULL);
    return g_fail || g_rfail;
}
