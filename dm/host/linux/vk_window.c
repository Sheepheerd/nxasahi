// Linux build: presentation through Vulkan (Honeykrisp on Apple silicon) to the SDL window.
// The game's presents (mtl_present) hand a ticket to one present thread, which shows the newest picture with FIFO (vsync) presentation; a present that a newer one
// replaced before the thread took it counts as handed over, so mtl_present_wait never waits on a frame that will not be shown.
// Step 1 of the Vulkan backend (LINUX.md): the game's draws are not translated yet, so nothing has written the window textures; the picture is a placeholder,
// a dark field with a bar that moves one step per presented game frame, which shows that the window, the swapchain and the frame pacing work
#include "dm.h"
#include "vk_window.h"
#include "vk_dev.h"
int vkx_origin_ll(void);   // vk_draw.c
#include "../gfx/nvn/nvn_obj.h"
// vk_replay.c: the image of a presented texture, made ready in c if new; and the replay's timeline
int vkr_present_image(const tex_t *o, VkCommandBuffer c, VkImage *img, uint32_t *w, uint32_t *h, VkFormat *fmt);
VkSemaphore vkr_timeline(void);
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <stdatomic.h>

#define VKW_FRAMES 2   // frames in flight
#define VK_OK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { LOG("[RED] vulkan: %s failed (%d)", #x, (int)r_); return 0; } } while (0)

static SDL_Window *g_win;
static VkSurfaceKHR g_surf;   // the device is the shared one (vk_dev.c)
#define g_inst g_vkd.inst
#define g_pd g_vkd.pd
#define g_dev g_vkd.dev
#define g_q g_vkd.q
#define g_qf g_vkd.qf
static VkSwapchainKHR g_sc; static VkFormat g_fmt; static VkExtent2D g_ext; static uint32_t g_nimg; static VkImage g_img[8]; static VkImageView g_view[8];
static VkSemaphore g_present_done[8];   // per swapchain image: the GPU finished drawing it (presentation waits on it)
static VkCommandPool g_pool; static VkCommandBuffer g_cmd[VKW_FRAMES]; static VkSemaphore g_acquired[VKW_FRAMES]; static VkFence g_done[VKW_FRAMES];
static _Atomic int g_active, g_resize, g_suspended;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_want, g_handed;   // newest ticket asked for / newest handed to the GPU (under g_mu)
typedef struct { tex_t tex; int has, cpu; uint64_t wait; int crop[4]; } src_t;   // what the newest present shows (under g_mu); cpu: the picture in g_cpu (vkw_present_rgba)
// A picture from host memory (an OpenGL game's frame read back at eglSwapBuffers, linux/gl_egl.c): the newest one under g_mu, copied by the present thread into the staging buffer of
// its frame in flight, then into one image the window blits from
static uint8_t *g_cpu; static uint32_t g_cpu_w, g_cpu_h; static size_t g_cpu_cap;
static VkBuffer g_stg[VKW_FRAMES]; static VkDeviceMemory g_stgm[VKW_FRAMES]; static void *g_stgp[VKW_FRAMES]; static size_t g_stg_sz[VKW_FRAMES];
static VkImage g_cimg; static VkDeviceMemory g_cimgm; static uint32_t g_cimg_w, g_cimg_h; static unsigned g_slot;
uint32_t vkr_pick_mem(uint32_t bits, VkMemoryPropertyFlags want);   // vk_replay.c
static src_t g_src;

static void swapchain_drop(void) {
    for (uint32_t i = 0; i < g_nimg; i++) { vkDestroyImageView(g_dev, g_view[i], NULL); vkDestroySemaphore(g_dev, g_present_done[i], NULL); }
    g_nimg = 0;
}
static int swapchain_make(void) {
    VkSurfaceCapabilitiesKHR cap; VK_OK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, g_surf, &cap));
    int w = 0, h = 0; SDL_GetWindowSizeInPixels(g_win, &w, &h);
    g_ext = cap.currentExtent.width != UINT32_MAX ? cap.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    if (!g_ext.width || !g_ext.height) return 0;   // minimized
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surf, &nf, NULL);
    VkSurfaceFormatKHR fs[32]; if (nf > 32) nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surf, &nf, fs);
    VkSurfaceFormatKHR f = fs[0];
    for (uint32_t i = 0; i < nf; i++) if (fs[i].format == VK_FORMAT_B8G8R8A8_UNORM && fs[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) f = fs[i];   // BGRA8 unorm, as the Metal layer
    g_fmt = f.format;
    uint32_t want = cap.minImageCount + 1; if (cap.maxImageCount && want > cap.maxImageCount) want = cap.maxImageCount;
    VkSwapchainKHR old = g_sc;
    VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_surf, .minImageCount = want, .imageFormat = f.format, .imageColorSpace = f.colorSpace,
        .imageExtent = g_ext, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = cap.currentTransform, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE, .oldSwapchain = old};
    VK_OK(vkCreateSwapchainKHR(g_dev, &ci, NULL, &g_sc));
    if (old) { { pthread_mutex_lock(&g_vkd_qlock); vkDeviceWaitIdle(g_dev); pthread_mutex_unlock(&g_vkd_qlock); } swapchain_drop(); vkDestroySwapchainKHR(g_dev, old, NULL); }
    g_nimg = 8; VK_OK(vkGetSwapchainImagesKHR(g_dev, g_sc, &g_nimg, g_img));
    for (uint32_t i = 0; i < g_nimg; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = g_fmt,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_OK(vkCreateImageView(g_dev, &vi, NULL, &g_view[i]));
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VK_OK(vkCreateSemaphore(g_dev, &si, NULL, &g_present_done[i]));
    }
    LOG("vulkan: swapchain %ux%u, %u images, format %d, FIFO", g_ext.width, g_ext.height, g_nimg, (int)g_fmt);
    return 1;
}

static void barrier(VkCommandBuffer c, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags s0, VkAccessFlags a0, VkPipelineStageFlags s1, VkAccessFlags a1) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = a0, .dstAccessMask = a1, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(c, s0, s1, 0, 0, NULL, 0, NULL, 1, &b);
}
// The picture: the game's presented texture, its crop scaled to fit the window with black bars (the game's aspect kept); without one, the placeholder
// (a dark field and a bar that moves one step per game frame)
static void barrier(VkCommandBuffer c, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags s0, VkAccessFlags a0, VkPipelineStageFlags s1, VkAccessFlags a1);
// The host-memory picture into g_cimg (GENERAL layout) in c: 0 when there is none or nothing could be made
static int cpu_image(VkCommandBuffer c, VkImage *img, uint32_t *w, uint32_t *h) {
    pthread_mutex_lock(&g_mu); uint32_t cw = g_cpu_w, ch = g_cpu_h; size_t n = (size_t)cw * ch * 4;
    unsigned k = g_slot;
    if (!g_cpu || !n) { pthread_mutex_unlock(&g_mu); return 0; }
    if (g_stg_sz[k] < n) {
        if (g_stg[k]) { vkUnmapMemory(g_dev, g_stgm[k]); vkDestroyBuffer(g_dev, g_stg[k], NULL); vkFreeMemory(g_dev, g_stgm[k], NULL); g_stg[k] = VK_NULL_HANDLE; g_stg_sz[k] = 0; }
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VkMemoryRequirements mr; VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        if (vkCreateBuffer(g_dev, &bi, NULL, &g_stg[k])) { pthread_mutex_unlock(&g_mu); return 0; }
        vkGetBufferMemoryRequirements(g_dev, g_stg[k], &mr); ma.allocationSize = mr.size; ma.memoryTypeIndex = vkr_pick_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(g_dev, &ma, NULL, &g_stgm[k]) || vkBindBufferMemory(g_dev, g_stg[k], g_stgm[k], 0) || vkMapMemory(g_dev, g_stgm[k], 0, n, 0, &g_stgp[k])) { pthread_mutex_unlock(&g_mu); return 0; }
        g_stg_sz[k] = n;
    }
    memcpy(g_stgp[k], g_cpu, n);
    pthread_mutex_unlock(&g_mu);
    if (!g_cimg || g_cimg_w != cw || g_cimg_h != ch) {
        if (g_cimg) { { pthread_mutex_lock(&g_vkd_qlock); vkDeviceWaitIdle(g_dev); pthread_mutex_unlock(&g_vkd_qlock); } vkDestroyImage(g_dev, g_cimg, NULL); vkFreeMemory(g_dev, g_cimgm, NULL); g_cimg = VK_NULL_HANDLE; }
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {cw, ch, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VkMemoryRequirements mr; VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        if (vkCreateImage(g_dev, &ii, NULL, &g_cimg)) return 0;
        vkGetImageMemoryRequirements(g_dev, g_cimg, &mr); ma.allocationSize = mr.size; ma.memoryTypeIndex = vkr_pick_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(g_dev, &ma, NULL, &g_cimgm) || vkBindImageMemory(g_dev, g_cimg, g_cimgm, 0)) { vkDestroyImage(g_dev, g_cimg, NULL); g_cimg = VK_NULL_HANDLE; return 0; }
        g_cimg_w = cw; g_cimg_h = ch;
        barrier(c, g_cimg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    } else barrier(c, g_cimg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {cw, ch, 1}};
    vkCmdCopyBufferToImage(c, g_stg[k], g_cimg, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    barrier(c, g_cimg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    *img = g_cimg; *w = cw; *h = ch; return 1;
}
static int record(VkCommandBuffer c, uint32_t i, uint64_t ticket, const src_t *s) {
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(c, &bi);
    VkImage src; uint32_t sw, sh; VkFormat sf;
    if (s->cpu ? cpu_image(c, &src, &sw, &sh) : s->has && vkr_present_image(&s->tex, c, &src, &sw, &sh, &sf)) {
        barrier(c, g_img[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(c, g_img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);
        barrier(c, g_img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        int32_t cx = s->crop[0], cy = s->crop[1], cw = s->crop[2], ch = s->crop[3];
        if (cw <= 0 || ch <= 0 || cx < 0 || cy < 0 || (uint32_t)(cx + cw) > sw || (uint32_t)(cy + ch) > sh) { cx = 0; cy = 0; cw = (int32_t)sw; ch = (int32_t)sh; }
        double sc = (double)g_ext.width / cw < (double)g_ext.height / ch ? (double)g_ext.width / cw : (double)g_ext.height / ch;
        int32_t dw = (int32_t)(cw * sc), dh = (int32_t)(ch * sc), dx = ((int32_t)g_ext.width - dw) / 2, dy = ((int32_t)g_ext.height - dh) / 2;
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{cx, cy, 0}, {cx + cw, cy + ch, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
        if (s->cpu || vkx_origin_ll()) { bl.dstOffsets[0].y = dy + dh; bl.dstOffsets[1].y = dy; }   // lower-left origin: the game's image is bottom-up (Pokémon: Let's Go; GL's read-back rows), shown flipped
        vkCmdBlitImage(c, src, VK_IMAGE_LAYOUT_GENERAL, g_img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        barrier(c, g_img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
        vkEndCommandBuffer(c);
        return 1;
    }
    barrier(c, g_img[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo att = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_view[i], .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.color = {{0.06f, 0.06f, 0.08f, 1}}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_ext}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att};
    vkCmdBeginRendering(c, &ri);
    uint32_t bw = g_ext.width / 40 ? g_ext.width / 40 : 1, x = (uint32_t)((ticket * 8) % (g_ext.width > bw ? g_ext.width - bw : 1));
    VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0.90f, 0.22f, 0.15f, 1}}}};
    VkClearRect cr = {{{(int32_t)x, (int32_t)(g_ext.height * 2 / 5)}, {bw, g_ext.height / 5 ? g_ext.height / 5 : 1}}, 0, 1};
    vkCmdClearAttachments(c, 1, &ca, 1, &cr);
    vkCmdEndRendering(c);
    barrier(c, g_img[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    vkEndCommandBuffer(c);
    return 0;
}
// One present: 1 = shown, 0 = skipped (minimized, or the swapchain is being made again)
static int present_one(uint64_t ticket, unsigned slot, const src_t *s) {
    if (atomic_exchange(&g_resize, 0) || !g_nimg) { { pthread_mutex_lock(&g_vkd_qlock); vkDeviceWaitIdle(g_dev); pthread_mutex_unlock(&g_vkd_qlock); } if (!swapchain_make()) return 0; }
    vkWaitForFences(g_dev, 1, &g_done[slot], VK_TRUE, UINT64_MAX);
    uint32_t i; VkResult r = vkAcquireNextImageKHR(g_dev, g_sc, UINT64_MAX, g_acquired[slot], VK_NULL_HANDLE, &i);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { atomic_store(&g_resize, 1); return 0; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("[RED] vulkan: acquire failed (%d)", (int)r); return 0; }
    vkResetFences(g_dev, 1, &g_done[slot]);
    vkResetCommandBuffer(g_cmd[slot], 0); g_slot = slot; int game = record(g_cmd[slot], i, ticket, s);
    // waits: the swapchain image, and for the game's picture the replay's timeline (the submissions before the present: their writes are visible to the copy)
    VkSemaphore wsem[2] = {g_acquired[slot], vkr_timeline()}; uint64_t wval[2] = {0, s->wait};
    VkPipelineStageFlags ws[2] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 2, .pWaitSemaphoreValues = wval};
    if (s->cpu) game = 0;   // nothing of the replay's timeline to wait for
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = game ? &ts : NULL, .waitSemaphoreCount = game ? 2 : 1, .pWaitSemaphores = wsem, .pWaitDstStageMask = ws,
        .commandBufferCount = 1, .pCommandBuffers = &g_cmd[slot], .signalSemaphoreCount = 1, .pSignalSemaphores = &g_present_done[i]};
    pthread_mutex_lock(&g_vkd_qlock); r = vkQueueSubmit(g_q, 1, &si, g_done[slot]); pthread_mutex_unlock(&g_vkd_qlock);
    if (r != VK_SUCCESS) { LOG("[RED] vulkan: submit failed (%d)", (int)r); return 0; }
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_present_done[i], .swapchainCount = 1, .pSwapchains = &g_sc, .pImageIndices = &i};
    pthread_mutex_lock(&g_vkd_qlock); r = vkQueuePresentKHR(g_q, &pi); pthread_mutex_unlock(&g_vkd_qlock);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) atomic_store(&g_resize, 1);
    else if (r != VK_SUCCESS) LOG("[RED] vulkan: present failed (%d)", (int)r);
    return 1;
}
static void *present_thread(void *arg) {
    (void)arg; unsigned slot = 0; uint64_t shown = 0;
    pthread_setname_np("dm present");
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_want == g_handed) pthread_cond_wait(&g_cv, &g_mu);
        uint64_t t = g_want; src_t s = g_src;   // the newest: presents in between are replaced
        pthread_mutex_unlock(&g_mu);
        if (!atomic_load(&g_suspended) && present_one(t, slot, &s)) { slot = (slot + 1) % VKW_FRAMES; if (!shown++) LOG("vulkan: first picture shown (game present %llu%s)", (unsigned long long)t, s.has ? ", the game's texture" : s.cpu ? ", the game's OpenGL frame" : ", the placeholder"); }
        pthread_mutex_lock(&g_mu); g_handed = t; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

int vkw_init(SDL_Window *w) {
    g_win = w;
    if (!vkd_ok() || !g_vkd.window) { LOG("[RED] vulkan: the window needs the device made for it (vkd_init(1))"); return 0; }
    if (!SDL_Vulkan_CreateSurface(w, g_inst, NULL, &g_surf)) { LOG("[RED] vulkan: no window surface (%s)", SDL_GetError()); return 0; }
    VkBool32 pres = 0; vkGetPhysicalDeviceSurfaceSupportKHR(g_pd, g_qf, g_surf, &pres);
    if (!pres) { LOG("[RED] vulkan: queue family %u cannot present to the window", g_qf); return 0; }
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_qf};
    VK_OK(vkCreateCommandPool(g_dev, &pci, NULL, &g_pool));
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = VKW_FRAMES};
    VK_OK(vkAllocateCommandBuffers(g_dev, &cai, g_cmd));
    for (int i = 0; i < VKW_FRAMES; i++) {
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VK_OK(vkCreateSemaphore(g_dev, &si, NULL, &g_acquired[i]));
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT}; VK_OK(vkCreateFence(g_dev, &fi, NULL, &g_done[i]));
    }
    if (!swapchain_make()) return 0;
    pthread_t t; if (pthread_create(&t, NULL, present_thread, NULL)) { LOG("[RED] vulkan: no present thread"); return 0; }
    pthread_detach(t);
    atomic_store(&g_active, 1);
    return 1;
}
int vkw_active(void) { return atomic_load(&g_active); }
uint64_t vkw_present(const void *t0, uint64_t wait, const int crop[4], double period_s) {
    (void)period_s;   // FIFO shows each picture for at least one refresh; the game's own pacing (hle_nvn.c) keeps the rate
    pthread_mutex_lock(&g_mu);
    g_src.has = t0 != NULL; g_src.cpu = 0; if (t0) g_src.tex = *(const tex_t *)t0; g_src.wait = wait; for (int k = 0; k < 4; k++) g_src.crop[k] = crop ? crop[k] : 0;
    uint64_t t = ++g_want; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    return t;
}
uint64_t vkw_present_rgba(const void *rgba, uint32_t w, uint32_t h) {
    size_t n = (size_t)w * h * 4;
    pthread_mutex_lock(&g_mu);
    if (g_cpu_cap < n) { free(g_cpu); g_cpu = malloc(n); g_cpu_cap = g_cpu ? n : 0; }
    if (g_cpu) { memcpy(g_cpu, rgba, n); g_cpu_w = w; g_cpu_h = h; }
    g_src.has = 0; g_src.cpu = g_cpu != NULL; g_src.wait = 0; memset(g_src.crop, 0, sizeof g_src.crop);
    uint64_t t = ++g_want; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    return t;
}
void vkw_present_wait(uint64_t ticket) {
    pthread_mutex_lock(&g_mu); while (g_handed < ticket) pthread_cond_wait(&g_cv, &g_mu); pthread_mutex_unlock(&g_mu);
}
void vkw_resized(void) { atomic_store(&g_resize, 1); }
void vkw_suspend(int on) { atomic_store(&g_suspended, on); }
int vkw_display_hz(void) {
    if (!g_win) return 0;
    const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_win));
    return m && m->refresh_rate > 0 ? (int)(m->refresh_rate + 0.5f) : 0;
}
