// Linux build: what the replay (vk_replay.c) shares with the draw path (vk_draw.c): its texture images and their lookups
#pragma once
#include "../gfx/nvn/nvn_obj.h"
#include <pthread.h>
#include <vulkan/vulkan.h>
typedef struct vkview { uint32_t key; VkImageView view; struct vkview *next; } vkview;   // views of an image, by vkr_view's key
typedef struct vtex {
    uint64_t serial; struct vtex *next;
    VkImage img; VkDeviceMemory mem; VkFormat fmt; VkImageAspectFlags aspect;
    uint32_t w, h, d, layers, levels, samples; int target, fresh;   // fresh: still VK_IMAGE_LAYOUT_UNDEFINED
    VkImageUsageFlags usage;
    int gpu_owned;            // written by the GPU (a render target, cleared, a copy destination): its content is the GPU's, never reloaded from guest memory
    int uploaded; uint64_t stamp;   // loaded from guest memory, at this dirty-tracking stamp (nvn_dirty.c)
    uint64_t release_after;   // released: destroyed once the timeline passes this (0 = live)
    vkview *views;
} vtex;
extern pthread_mutex_t g_tmu;   // the texture table
vtex *vkr_tex_sampled(const tex_t *o, VkCommandBuffer upload_unused);   // under g_tmu: the image, loaded from guest memory first when it must be
int vkr_treg(int id, tex_t *t, view_t *v, int *has_v);                  // a registered texture ID
int vkr_sreg(int id, samp_par *p);                                       // a registered sampler ID
uint32_t vkr_pick_mem(uint32_t bits, VkMemoryPropertyFlags want);
uint64_t vkr_fence_next(void);   // the fence value the submission in progress will signal
int vkr_slot(void);              // the command buffer ring slot of the submission in progress
VkCommandBuffer vkr_cmd(void);   // its command buffer
