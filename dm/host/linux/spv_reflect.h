// Linux build: what the Vulkan backend needs to know about a translated shader's SPIR-V (dm/gfx/tools/dmsh_spv.py) to build its descriptor set and pipeline layouts
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#define SPVR_MAX_BIND 96
typedef struct { uint32_t set, binding, count; VkDescriptorType type; const char *name; } spvr_binding;   // count 0 = a runtime array
typedef struct {
    VkShaderStageFlagBits stage;   // from the entry point's execution model; 0 = unknown
    int nbind; spvr_binding bind[SPVR_MAX_BIND];
    uint32_t push_size;            // bytes of the push constant block (0 = none)
    uint32_t local_size[3];        // compute: LocalSize
} spvr_info;
// 0 on success; -1 = not SPIR-V or a structure this reader does not know (err says which). Names point into the module (they stay valid while it does)
int spvr_reflect(const uint32_t *code, size_t words, spvr_info *out, char *err, size_t errcap);

// Fit a module to the device: what the translator declares but the shader does not use, the device may lack. Only provably unused declarations are dropped and only
// never-written buffers marked read-only, so the shader's behaviour is unchanged. Returns a malloc'd copy (*out_words), and in *changes what was done (a bit per kind)
typedef struct { int float64, interlock, vertex_stores; } spvr_device;
#define SPVR_FIT_FLOAT64 1     // OpCapability Float64 dropped: no 64-bit float type in the module
#define SPVR_FIT_INTERLOCK 2   // the fragment shader interlock extension, capabilities and execution modes dropped: no interlock begun
#define SPVR_FIT_NONWRITABLE 4 // vertex-stage storage buffers that nothing writes decorated NonWritable
#define SPVR_FIT_STORES 8      // a vertex-stage storage buffer is written, which the device cannot do (not changed: the caller must not use the shader as is)
uint32_t *spvr_fit(const uint32_t *code, size_t words, const spvr_device *dev, size_t *out_words, unsigned *changes);
