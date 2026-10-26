// Linux build: the draw path of the Vulkan replay (vk_draw.c), fed by the replay's walker (vk_replay.c)
#pragma once
#include "../gfx/nvn/nvn_obj.h"
#include <vulkan/vulkan.h>
void vkx_sub_begin(int slot, VkCommandBuffer c);   // a submission begins in this ring slot (its descriptor pool is reset)
int vkx_prepare_draw(VkCommandBuffer c);          // the program's shaders and descriptor sets for the draw now, bound in c; 0 = it cannot be drawn
void vkx_bind_program(const prog_t *p);
void vkx_bind_buf(int ssbo, const p_bind_buf *b);
void vkx_bind_handles(int what, const p_bind_handles *b, const uint64_t *h);   // what: 0 textures, 1 samplers, 2 images; h = the handles as they are now
void vkx_set_pool(int tex, int base);
