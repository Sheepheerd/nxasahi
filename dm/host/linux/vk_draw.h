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
void vkx_trace(int on);
void vkx_dispatch(VkCommandBuffer c, const p_dispatch *p);
void vkx_draw(VkCommandBuffer c, uint32_t op, const void *a, uint32_t size, int ncolor, uint32_t rt_w, uint32_t rt_h, int samples);
void vkx_bind_state(const p_bind_state *s);
void vkx_viewports(const p_viewports *v); void vkx_scissor(const p_scissor *s); void vkx_depth_range(const p_depth_range *r);
void vkx_stencil(const p_stencil *s); void vkx_poly_offset(const p_poly_offset *p); void vkx_blend_color(const p_blend_color *b);
void vkx_depth_clamp(int on); void vkx_prim_restart(int on);
void vkx_vattr(const p_bind_vattr *a); void vkx_vstream(const p_bind_vstream *s); void vkx_vbuf(const p_bind_vbuf *b);
