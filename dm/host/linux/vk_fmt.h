// Linux build: NVN formats as Vulkan formats (vk_fmt.c)
#pragma once
#include <vulkan/vulkan.h>
// The Vulkan format for an NVNformat; *substitute = 1 when it stands in for a layout Vulkan does not have (data or reads need converting, see nvn_fmt_mtl.c);
// *swizzle = how the components read (formats without alpha read a as 1, and so on). VK_FORMAT_UNDEFINED = no counterpart
VkFormat vkf_from_nvn(int nvn_format, int *substitute, VkComponentMapping *swizzle);
VkImageAspectFlags vkf_aspect(VkFormat f);
