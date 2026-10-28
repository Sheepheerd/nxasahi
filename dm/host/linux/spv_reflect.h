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
typedef struct { int float64, interlock, vertex_stores, nan_preserve; } spvr_device;   // nan_preserve: shaderSignedZeroInfNanPreserveFloat32
#define SPVR_FIT_FLOAT64 1     // OpCapability Float64 dropped: no 64-bit float type in the module
#define SPVR_FIT_INTERLOCK 2   // the fragment shader interlock extension, capabilities and execution modes dropped: no interlock begun
#define SPVR_FIT_NONWRITABLE 4 // vertex-stage storage buffers that nothing writes decorated NonWritable
#define SPVR_FIT_STORES 8      // a vertex-stage storage buffer is written, which the device cannot do (not changed: the caller must not use the shader as is)
#define SPVR_FIT_RELAXED 16    // RelaxedPrecision decorations dropped: Mesa computes such values in 16 bits (Metal did not); the translator's precision hints do not
#define SPVR_FIT_NMINMAX 32  // FMin / FMax / FClamp made NMin / NMax / NClamp: a NaN operand gives the other one, as Maxwell's FMNMX and saturation do
#define SPVR_FIT_NANPRESERVE 64 // SignedZeroInfNanPreserve on every entry point: without it Mesa may fold isnan() to false and reorder away infinities and NaN, which Maxwell keeps
#define SPVR_FIT_VERSION 5     // what spvr_fit and spvr_nan change; in the disk cache's directory name (vk_draw.c), so binaries made before a change are not taken
                               // describe Maxwell, which computes everything in 32 bits
uint32_t *spvr_fit(const uint32_t *code, size_t words, const spvr_device *dev, size_t *out_words, unsigned *changes);

// GL's clip depth (NVN_DEPTH_MODE_NEAR_IS_MINUS_W, NVN's default) in a module for Vulkan's: before each return of the entry point, gl_Position.z = (z + w) / 2, so -w..w
// becomes 0..w. For the last stage before rasterization (vertex, or geometry / tessellation evaluation). Returns a malloc'd copy, or NULL when the module writes no Position
uint32_t *spvr_gl_depth(const uint32_t *code, size_t words, size_t *out_words);

// Every vec4 output of a vertex or tessellation evaluation shader set to (0, 0, 0, 1) at the start of the entry point: on Maxwell a component the shader never writes reads as 0 in
// the next stage, where Vulkan leaves it undefined. Mario Wonder's tile and model shaders write one component of each tangent-frame output (_41.y = 1.0) and Mesa passed garbage
// for the rest, so the normals overflowed to NaN and lit surfaces came out black. Returns a malloc'd copy, or NULL when there is nothing to do
uint32_t *spvr_init_outputs(const uint32_t *code, size_t words, size_t *out_words);
// Maxwell's non-finite values (spv_reflect.c): Sin / Cos of NaN or of an infinity give NaN, and a NaN user output of any graphics stage is written as 0. A malloc'd copy, or NULL when
// there is nothing to change; changes: 1 Sin / Cos, 2 outputs
uint32_t *spvr_nan(const uint32_t *code, size_t words, size_t *out_words, unsigned *changes);
