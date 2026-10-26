// Linux build: guest memory the GPU sees (vk_mem.c). The heap and the alias region (hle_svc.c) are backed by Vulkan memory in chunks mapped in place at their own
// addresses, each region one sparse buffer the chunks are bound into at the same offsets: the game and the GPU use the same bytes, as on the Switch
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
// What the GPU uses for [addr, addr + len): the region's buffer and the offset in it, and its GPU address. 0 when the range is not all in backed chunks of one region
int vkm_lookup(const void *addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off, VkDeviceAddress *gpu);
void vkm_stats(uint64_t *chunks, uint64_t *bytes);
