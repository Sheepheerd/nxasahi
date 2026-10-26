// Linux build: NVN formats as Vulkan formats. The NVN knowledge stays in one place, dm/gfx/tex/nvn_fmt_mtl.c (which format, which substitute, which components);
// this maps its Metal pixel formats to the Vulkan ones of the same layout. 0 (VK_FORMAT_UNDEFINED) where Metal has none either (the table's 3-component formats)
#include "vk_fmt.h"
#include "../gfx/tex/nvn_fmt_mtl.h"

static VkFormat from_mtl(uint32_t m) {
    switch (m) {
    case 10: return VK_FORMAT_R8_UNORM; case 12: return VK_FORMAT_R8_SNORM; case 13: return VK_FORMAT_R8_UINT; case 14: return VK_FORMAT_R8_SINT;
    case 20: return VK_FORMAT_R16_UNORM; case 22: return VK_FORMAT_R16_SNORM; case 23: return VK_FORMAT_R16_UINT; case 24: return VK_FORMAT_R16_SINT; case 25: return VK_FORMAT_R16_SFLOAT;
    case 30: return VK_FORMAT_R8G8_UNORM; case 32: return VK_FORMAT_R8G8_SNORM; case 33: return VK_FORMAT_R8G8_UINT; case 34: return VK_FORMAT_R8G8_SINT;
    case 40: return VK_FORMAT_R5G6B5_UNORM_PACK16;        // MTLPixelFormatB5G6R5Unorm: blue in the low bits, as Vulkan's R5G6B5 (red in the high bits)
    case 41: return VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR;   // A1BGR5Unorm
    case 42: return VK_FORMAT_A4B4G4R4_UNORM_PACK16;       // ABGR4Unorm
    case 43: return VK_FORMAT_B5G5R5A1_UNORM_PACK16;       // BGR5A1Unorm
    case 53: return VK_FORMAT_R32_UINT; case 54: return VK_FORMAT_R32_SINT; case 55: return VK_FORMAT_R32_SFLOAT;
    case 60: return VK_FORMAT_R16G16_UNORM; case 62: return VK_FORMAT_R16G16_SNORM; case 63: return VK_FORMAT_R16G16_UINT; case 64: return VK_FORMAT_R16G16_SINT; case 65: return VK_FORMAT_R16G16_SFLOAT;
    case 70: return VK_FORMAT_R8G8B8A8_UNORM; case 71: return VK_FORMAT_R8G8B8A8_SRGB; case 72: return VK_FORMAT_R8G8B8A8_SNORM; case 73: return VK_FORMAT_R8G8B8A8_UINT; case 74: return VK_FORMAT_R8G8B8A8_SINT;
    case 80: return VK_FORMAT_B8G8R8A8_UNORM; case 81: return VK_FORMAT_B8G8R8A8_SRGB;
    case 90: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 91: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 92: return VK_FORMAT_B10G11R11_UFLOAT_PACK32; case 93: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 103: return VK_FORMAT_R32G32_UINT; case 104: return VK_FORMAT_R32G32_SINT; case 105: return VK_FORMAT_R32G32_SFLOAT;
    case 110: return VK_FORMAT_R16G16B16A16_UNORM; case 112: return VK_FORMAT_R16G16B16A16_SNORM; case 113: return VK_FORMAT_R16G16B16A16_UINT; case 114: return VK_FORMAT_R16G16B16A16_SINT;
    case 115: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 123: return VK_FORMAT_R32G32B32A32_UINT; case 124: return VK_FORMAT_R32G32B32A32_SINT; case 125: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 130: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK; case 131: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK; case 132: return VK_FORMAT_BC2_UNORM_BLOCK; case 133: return VK_FORMAT_BC2_SRGB_BLOCK;
    case 134: return VK_FORMAT_BC3_UNORM_BLOCK; case 135: return VK_FORMAT_BC3_SRGB_BLOCK; case 140: return VK_FORMAT_BC4_UNORM_BLOCK; case 141: return VK_FORMAT_BC4_SNORM_BLOCK;
    case 142: return VK_FORMAT_BC5_UNORM_BLOCK; case 143: return VK_FORMAT_BC5_SNORM_BLOCK; case 150: return VK_FORMAT_BC6H_SFLOAT_BLOCK; case 151: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case 152: return VK_FORMAT_BC7_UNORM_BLOCK; case 153: return VK_FORMAT_BC7_SRGB_BLOCK;
    case 250: return VK_FORMAT_D16_UNORM; case 252: return VK_FORMAT_D32_SFLOAT; case 253: return VK_FORMAT_S8_UINT; case 260: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    default: break;
    }
    // ASTC: Metal numbers LDR 4x4 from 204 and sRGB 4x4 from 186, in the same block order as Vulkan's (one number skipped after 6x6 on both sides of Metal's list)
    static const VkFormat ldr[] = {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x5_UNORM_BLOCK, VK_FORMAT_ASTC_6x5_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_UNORM_BLOCK,
        VK_FORMAT_UNDEFINED, VK_FORMAT_ASTC_8x5_UNORM_BLOCK, VK_FORMAT_ASTC_8x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x5_UNORM_BLOCK, VK_FORMAT_ASTC_10x6_UNORM_BLOCK,
        VK_FORMAT_ASTC_10x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_UNORM_BLOCK};
    if (m >= 204 && m < 204 + sizeof ldr / sizeof *ldr) return ldr[m - 204];
    if (m >= 186 && m < 186 + sizeof ldr / sizeof *ldr) { VkFormat f = ldr[m - 186]; return f ? (VkFormat)(f + 1) : VK_FORMAT_UNDEFINED; }   // each *_SRGB_BLOCK follows its *_UNORM_BLOCK
    return VK_FORMAT_UNDEFINED;
}

VkFormat vkf_from_nvn(int nvn_format, int *substitute, VkComponentMapping *swizzle) {
    uint32_t m = 0; uint8_t swz[4] = {NVN_SWZ_R, NVN_SWZ_G, NVN_SWZ_B, NVN_SWZ_A};
    int kind = nvn_fmt_to_mtl(nvn_format, &m, swz);
    if (substitute) *substitute = kind == 2;
    if (swizzle) {
        static const VkComponentSwizzle S[6] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
        swizzle->r = swz[0] < 6 ? S[swz[0]] : VK_COMPONENT_SWIZZLE_IDENTITY; swizzle->g = swz[1] < 6 ? S[swz[1]] : VK_COMPONENT_SWIZZLE_IDENTITY;
        swizzle->b = swz[2] < 6 ? S[swz[2]] : VK_COMPONENT_SWIZZLE_IDENTITY; swizzle->a = swz[3] < 6 ? S[swz[3]] : VK_COMPONENT_SWIZZLE_IDENTITY;
    }
    return kind ? from_mtl(m) : VK_FORMAT_UNDEFINED;
}

VkImageAspectFlags vkf_aspect(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_X8_D24_UNORM_PACK32: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
