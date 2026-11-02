// Linux build: what the replay (vk_replay.c) shares with the draw path (vk_draw.c): its texture images and their lookups
#pragma once
#include "../gfx/nvn/nvn_obj.h"
#include <pthread.h>
#include <vulkan/vulkan.h>
#include "vk_fmt.h"
// Command buffers in flight: a submission is flushed in parts (vkr_flush), so several per frame; the draw path keeps its descriptor pools, set cache and arena per slot (vk_draw.c)
#define CB_RING 16
// The sRGB / linear twin of an 8-bit RGBA or BGRA format (UNDEFINED for any other). Images of these formats are made mutable with both in their format list, so a texture view
// of the other one (TextureViewSetFormat) is honoured: Breath of the Wild writes and reads its picture through sRGB views of RGBA8 textures, and the views in the image's own
// format skipped the sRGB encoding, so the whole picture came out too dark (the Mac's "Render-target views that change the format")
static inline VkFormat vkr_srgb_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB; case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB; case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}
// 1 when a view of the image is to be made in the twin format: the view sets a format, and it is the twin of the image's
static inline int vkr_view_twin(VkFormat img_fmt, const view_t *v, int has_v) {
    if (!has_v || !v->set_format) return 0;
    VkFormat t = vkr_srgb_twin(img_fmt);
    return t != VK_FORMAT_UNDEFINED && vkf_from_nvn(v->format, NULL, NULL) == t;
}
typedef struct vkview { uint64_t key; VkImageView view; struct vkview *next; } vkview;   // views of an image, by vkr_view's key (bit 32: in the sRGB / linear twin format)
typedef struct vtex {
    uint64_t serial; struct vtex *next;
    VkImage img; VkDeviceMemory mem; VkFormat fmt; VkImageAspectFlags aspect;
    uint32_t w, h, d, layers, levels, samples; int target, fresh;   // fresh: still VK_IMAGE_LAYOUT_UNDEFINED
    VkImageUsageFlags usage;
    int gpu_owned;            // written by the GPU (a render target, cleared, a copy destination): its content is the GPU's, never reloaded from guest memory
    int uploaded; uint64_t stamp;   // loaded from guest memory, at this dirty-tracking stamp (nvn_dirty.c)
    uint64_t clean_at; const void *span;   // the dirty clock when last found unwritten since stamp; its tracked range (nvn_dirty_span)
    uint64_t release_after;   // released: destroyed once the timeline passes this (0 = live)
    vkview *views;
    const uint8_t *kmem, *kend; nvn_texdesc kd; uint32_t khash; int refs;   // the storage it is found by (vk_replay.c's sharing), and the texture objects that refer to it
} vtex;
extern pthread_mutex_t g_tmu;   // the texture table
vtex *vkr_tex_sampled(const tex_t *o, VkCommandBuffer upload_unused);   // under g_tmu: the image, loaded from guest memory first when it must be
int vkr_treg(int id, tex_t *t, view_t *v, int *has_v);                  // a registered texture ID
int vkr_sreg(int id, samp_par *p);                                       // a registered sampler ID
uint32_t vkr_pick_mem(uint32_t bits, VkMemoryPropertyFlags want);
uint64_t vkr_fence_next(void);   // the fence value the submission in progress will signal
int vkr_slot(void);              // the command buffer ring slot of the submission in progress
VkCommandBuffer vkr_cmd(void);   // its command buffer
