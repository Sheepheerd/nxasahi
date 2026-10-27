// Linux build: the Vulkan instance and device (vk_dev.h). One queue family with graphics, compute and sparse binding (Honeykrisp has one for everything);
// the features the backend relies on are required here, so a device without them is reported at startup rather than failing later
#include "dm.h"
#include "vk_dev.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

vkd_t g_vkd;
pthread_mutex_t g_vkd_qlock = PTHREAD_MUTEX_INITIALIZER;
#define VKD_OK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { LOG("[RED] vulkan: %s failed (%d)", #x, (int)r_); return 0; } } while (0)

static int has_ext(VkPhysicalDevice pd, const char *name) {
    uint32_t n = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof *e); vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    int ok = 0; for (uint32_t i = 0; i < n && !ok; i++) ok = !strcmp(e[i].extensionName, name);
    free(e); return ok;
}

int vkd_init(int with_window) {
    if (g_vkd.dev) return 1;
    const char *ext[16]; uint32_t ne = 0;
    if (with_window) {
        Uint32 n = 0; const char *const *se = SDL_Vulkan_GetInstanceExtensions(&n);
        if (!se) { LOG("[RED] vulkan: SDL has no Vulkan instance extensions (%s)", SDL_GetError()); return 0; }
        for (Uint32 i = 0; i < n && ne < 16; i++) ext[ne++] = se[i];
    }
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = DM_GAME_NAME, .pEngineName = "nxasahi", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = ne, .ppEnabledExtensionNames = ext};
    VKD_OK(vkCreateInstance(&ii, NULL, &g_vkd.inst));
    uint32_t np = 8; VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(g_vkd.inst, &np, pds);
    for (uint32_t i = 0; i < np && !g_vkd.pd; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && np > 1) continue;   // llvmpipe only when nothing else is there
        uint32_t nq = 16; VkQueueFamilyProperties qp[16]; vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qp);
        VkQueueFlags want = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_SPARSE_BINDING_BIT;
        for (uint32_t f = 0; f < nq; f++) if ((qp[f].queueFlags & want) == want) { g_vkd.pd = pds[i]; g_vkd.qf = f; g_vkd.props = p; break; }
    }
    if (!g_vkd.pd) { LOG("[RED] vulkan: no device with a graphics, compute and sparse binding queue"); return 0; }
    vkGetPhysicalDeviceMemoryProperties(g_vkd.pd, &g_vkd.mem);
    g_vkd.placed = has_ext(g_vkd.pd, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME) && has_ext(g_vkd.pd, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME);
    g_vkd.shader_object = has_ext(g_vkd.pd, VK_EXT_SHADER_OBJECT_EXTENSION_NAME);
    g_vkd.index_u8 = has_ext(g_vkd.pd, VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME);

    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT mp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
    VkPhysicalDeviceIndexTypeUint8FeaturesKHR u8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_KHR, .pNext = g_vkd.placed ? &mp : NULL};
    VkPhysicalDeviceShaderObjectFeaturesEXT so = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT, .pNext = &u8};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = g_vkd.shader_object ? (void *)&so : g_vkd.placed ? (void *)&mp : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(g_vkd.pd, &have);
    if (!f13.dynamicRendering || !f12.bufferDeviceAddress || !f12.timelineSemaphore || !have.features.sparseBinding || !have.features.sparseResidencyBuffer) {
        LOG("[RED] vulkan: %s lacks a required feature (dynamic rendering %d, buffer device address %d, timeline semaphores %d, sparse binding %d, sparse buffers %d)", g_vkd.props.deviceName,
            f13.dynamicRendering, f12.bufferDeviceAddress, f12.timelineSemaphore, have.features.sparseBinding, have.features.sparseResidencyBuffer);
        return 0;
    }
    if (g_vkd.placed && !mp.memoryMapPlaced) g_vkd.placed = 0;
    if (g_vkd.shader_object && !so.shaderObject) g_vkd.shader_object = 0;
    if (g_vkd.index_u8 && !u8.indexTypeUint8) g_vkd.index_u8 = 0;
    have.features.robustBufferAccess = VK_FALSE; f13.robustImageAccess = VK_FALSE;   // bounds checks on every access: Metal had none, and NVN has none
    // Enable what the device has: the backend checks the features it uses, and the shaders' needs differ by game
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT mp_on = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT, .memoryMapPlaced = VK_TRUE};
    VkPhysicalDeviceIndexTypeUint8FeaturesKHR u8_on = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_KHR, .pNext = g_vkd.placed ? &mp_on : NULL, .indexTypeUint8 = VK_TRUE};
    VkPhysicalDeviceShaderObjectFeaturesEXT so_on = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT, .pNext = g_vkd.index_u8 ? (void *)&u8_on : g_vkd.placed ? (void *)&mp_on : NULL, .shaderObject = VK_TRUE};
    f13.pNext = g_vkd.shader_object ? (void *)&so_on : g_vkd.placed ? (void *)&mp_on : NULL;
    const char *dext[8]; uint32_t nd = 0;
    if (g_vkd.shader_object) dext[nd++] = VK_EXT_SHADER_OBJECT_EXTENSION_NAME;
    if (g_vkd.index_u8) dext[nd++] = VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME;
    if (with_window) dext[nd++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (g_vkd.placed) { dext[nd++] = VK_KHR_MAP_MEMORY_2_EXTENSION_NAME; dext[nd++] = VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME; }
    // DM_VK_ROBUST=1 (diagnostic): buffer reads past a binding's range return 0, as Maxwell's constant buffer reads do (robustness2)
    VkPhysicalDeviceRobustness2FeaturesEXT r2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .robustBufferAccess2 = VK_TRUE};
    if (getenv("DM_VK_ROBUST") && has_ext(g_vkd.pd, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)) {
        dext[nd++] = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME; have.features.robustBufferAccess = VK_TRUE; r2.pNext = f13.pNext; f13.pNext = &r2;
        LOG("vulkan: robust buffer access 2 on (DM_VK_ROBUST)"); }
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vkd.qf, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &have, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .enabledExtensionCount = nd, .ppEnabledExtensionNames = dext};
    VKD_OK(vkCreateDevice(g_vkd.pd, &di, NULL, &g_vkd.dev));
    vkGetDeviceQueue(g_vkd.dev, g_vkd.qf, 0, &g_vkd.q);
    if (g_vkd.placed) g_vkd.map2 = (PFN_vkMapMemory2KHR)vkGetDeviceProcAddr(g_vkd.dev, "vkMapMemory2KHR");
    if (g_vkd.shader_object) { g_vkd.create_shaders = (PFN_vkCreateShadersEXT)vkGetDeviceProcAddr(g_vkd.dev, "vkCreateShadersEXT");
        g_vkd.destroy_shader = (PFN_vkDestroyShaderEXT)vkGetDeviceProcAddr(g_vkd.dev, "vkDestroyShaderEXT"); g_vkd.bind_shaders = (PFN_vkCmdBindShadersEXT)vkGetDeviceProcAddr(g_vkd.dev, "vkCmdBindShadersEXT");
        g_vkd.shader_binary = (PFN_vkGetShaderBinaryDataEXT)vkGetDeviceProcAddr(g_vkd.dev, "vkGetShaderBinaryDataEXT"); }
    g_vkd.window = with_window;
    LOG("vulkan: %s (driver %u.%u.%u, API %u.%u), queue family %u%s%s", g_vkd.props.deviceName, VK_VERSION_MAJOR(g_vkd.props.driverVersion), VK_VERSION_MINOR(g_vkd.props.driverVersion),
        VK_VERSION_PATCH(g_vkd.props.driverVersion), VK_API_VERSION_MAJOR(g_vkd.props.apiVersion), VK_API_VERSION_MINOR(g_vkd.props.apiVersion), g_vkd.qf,
        g_vkd.placed ? ", placed memory maps" : ", [RED] no placed memory maps (guest memory is not GPU-visible)", with_window ? ", for a window" : ", headless");
    return 1;
}
