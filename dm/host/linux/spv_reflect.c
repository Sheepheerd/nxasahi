// Linux build: a small SPIR-V reader for the shaders the translator writes (spv_reflect.h). One pass collects the ids' types, decorations and constants, a second
// classifies each decorated resource variable as a Vulkan descriptor
#include "spv_reflect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OpName = 5, OpEntryPoint = 15, OpExecutionMode = 16, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24, OpTypeImage = 25, OpTypeSampler = 26,
       OpTypeSampledImage = 27, OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpVariable = 59, OpDecorate = 71,
       OpMemberDecorate = 72, OpTypeAccelerationStructureKHR = 5341 };
enum { DecBlock = 2, DecBufferBlock = 3, DecArrayStride = 6, DecBinding = 33, DecDescriptorSet = 34, DecOffset = 35 };
enum { SCUniformConstant = 0, SCUniform = 2, SCPushConstant = 9, SCStorageBuffer = 12 };
enum { DimBuffer = 5, DimSubpassData = 6 };

typedef struct { uint16_t op; uint32_t a[9]; const uint32_t *ins; uint16_t nwords; int32_t set, binding; uint8_t block, bufblock; uint32_t stride; const char *name; } ent;

static uint32_t type_size(const ent *e, uint32_t id, uint32_t nid, int depth);
static uint32_t struct_size(const ent *e, uint32_t id, uint32_t nid, int depth, const uint32_t *code, size_t words) {
    // the end of the last member: its Offset decoration plus its size
    uint32_t end = 0, nmem = e[id].nwords - 2;
    for (size_t i = 5; i < words;) {
        uint32_t op = code[i] & 0xFFFF, n = code[i] >> 16; if (!n) break;
        if (op == OpMemberDecorate && code[i + 1] == id && code[i + 3] == DecOffset && code[i + 2] < nmem) {
            uint32_t m = code[i + 2], mt = e[id].ins[2 + m], sz = type_size(e, mt, nid, depth + 1);
            if (code[i + 4] + sz > end) end = code[i + 4] + sz;
        }
        i += n;
    }
    return end;
}
static const uint32_t *g_code; static size_t g_words;
static uint32_t type_size(const ent *e, uint32_t id, uint32_t nid, int depth) {
    if (id >= nid || depth > 16) return 0;
    switch (e[id].op) {
    case OpTypeInt: case OpTypeFloat: return e[id].a[0] / 8;
    case OpTypeVector: return e[id].a[1] * type_size(e, e[id].a[0], nid, depth + 1);
    case OpTypeMatrix: return e[id].a[1] * type_size(e, e[id].a[0], nid, depth + 1);
    case OpTypeArray: { uint32_t len = e[id].a[1] < nid && e[e[id].a[1]].op == OpConstant ? e[e[id].a[1]].a[1] : 0;
                        uint32_t st = e[id].stride ? e[id].stride : type_size(e, e[id].a[0], nid, depth + 1); return len * st; }
    case OpTypeStruct: return struct_size(e, id, nid, depth, g_code, g_words);
    default: return 0;
    }
}

int spvr_reflect(const uint32_t *code, size_t words, spvr_info *out, char *err, size_t errcap) {
    memset(out, 0, sizeof *out);
    if (words < 5 || code[0] != 0x07230203) { snprintf(err, errcap, "not SPIR-V"); return -1; }
    uint32_t nid = code[3];
    ent *e = calloc(nid ? nid : 1, sizeof *e); if (!e) { snprintf(err, errcap, "out of memory"); return -1; }
    for (uint32_t i = 0; i < nid; i++) e[i].set = e[i].binding = -1;
    g_code = code; g_words = words;
    uint32_t entry = 0;
    for (size_t i = 5; i < words;) {
        uint32_t op = code[i] & 0xFFFF, n = code[i] >> 16;
        if (!n || i + n > words) { snprintf(err, errcap, "bad instruction at word %zu", i); free(e); return -1; }
        const uint32_t *w = code + i;
        switch (op) {
        case OpEntryPoint: if (!entry) { entry = w[2];
            switch (w[1]) { case 0: out->stage = VK_SHADER_STAGE_VERTEX_BIT; break; case 1: out->stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT; break;
                            case 2: out->stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT; break; case 3: out->stage = VK_SHADER_STAGE_GEOMETRY_BIT; break;
                            case 4: out->stage = VK_SHADER_STAGE_FRAGMENT_BIT; break; case 5: out->stage = VK_SHADER_STAGE_COMPUTE_BIT; break; } } break;
        case OpExecutionMode: if (w[1] == entry && w[2] == 17 && n >= 6) { out->local_size[0] = w[3]; out->local_size[1] = w[4]; out->local_size[2] = w[5]; } break;   // LocalSize
        case OpName: if (w[1] < nid) e[w[1]].name = (const char *)(w + 2); break;
        case OpDecorate:
            if (w[1] >= nid) break;
            if (w[2] == DecBinding) e[w[1]].binding = (int32_t)w[3];
            else if (w[2] == DecDescriptorSet) e[w[1]].set = (int32_t)w[3];
            else if (w[2] == DecBlock) e[w[1]].block = 1;
            else if (w[2] == DecBufferBlock) e[w[1]].bufblock = 1;
            else if (w[2] == DecArrayStride) e[w[1]].stride = w[3];
            break;
        case OpTypeInt: case OpTypeFloat: case OpTypeVector: case OpTypeMatrix: case OpTypeImage: case OpTypeSampler: case OpTypeSampledImage:
        case OpTypeArray: case OpTypeRuntimeArray: case OpTypeStruct: case OpTypePointer: case OpTypeAccelerationStructureKHR:
            if (w[1] < nid) { e[w[1]].op = (uint16_t)op; e[w[1]].ins = w; e[w[1]].nwords = (uint16_t)n; for (uint32_t k = 0; k < 9 && k + 2 < n; k++) e[w[1]].a[k] = w[2 + k]; } break;
        case OpConstant: if (w[2] < nid) { e[w[2]].op = OpConstant; e[w[2]].a[0] = w[1]; e[w[2]].a[1] = w[3]; } break;   // a[1] = the low word of the value
        case OpVariable: if (w[2] < nid) { e[w[2]].op = OpVariable; e[w[2]].a[0] = w[1]; e[w[2]].a[1] = w[3]; } break;   // a[0] = pointer type, a[1] = storage class
        }
        i += n;
    }
    int rc = 0;
    for (uint32_t id = 0; id < nid && !rc; id++) {
        if (e[id].op != OpVariable) continue;
        uint32_t sc = e[id].a[1], pt = e[id].a[0];
        if (pt >= nid || e[pt].op != OpTypePointer) continue;
        uint32_t t = e[pt].a[1];
        if (sc == SCPushConstant) { uint32_t s = type_size(e, t, nid, 0); if (s > out->push_size) out->push_size = s; continue; }
        if (e[id].binding < 0 || (sc != SCUniformConstant && sc != SCUniform && sc != SCStorageBuffer)) continue;
        uint32_t count = 1;
        for (int d = 0; d < 4 && t < nid && (e[t].op == OpTypeArray || e[t].op == OpTypeRuntimeArray); d++) {
            if (e[t].op == OpTypeRuntimeArray) count = 0;
            else if (count) { uint32_t c = e[t].a[1]; count *= c < nid && e[c].op == OpConstant ? e[c].a[1] : 1; }
            t = e[t].a[0];
        }
        if (t >= nid) continue;
        VkDescriptorType dt;
        if (sc == SCStorageBuffer || (sc == SCUniform && e[t].bufblock)) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (sc == SCUniform) dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else if (e[t].op == OpTypeSampler) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (e[t].op == OpTypeSampledImage) dt = e[t].a[0] < nid && e[e[t].a[0]].a[1] == DimBuffer ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (e[t].op == OpTypeImage) {   // a[1] = Dim, a[5] = Sampled (1 sampled, 2 storage)
            if (e[t].a[1] == DimSubpassData) dt = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            else if (e[t].a[1] == DimBuffer) dt = e[t].a[5] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            else dt = e[t].a[5] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        } else if (e[t].op == OpTypeAccelerationStructureKHR) dt = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        else { snprintf(err, errcap, "resource %%%u (%s): type op %u not known", id, e[id].name ? e[id].name : "?", e[t].op); rc = -1; break; }
        if (out->nbind == SPVR_MAX_BIND) { snprintf(err, errcap, "more than %d resources", SPVR_MAX_BIND); rc = -1; break; }
        out->bind[out->nbind++] = (spvr_binding){(uint32_t)(e[id].set < 0 ? 0 : e[id].set), (uint32_t)e[id].binding, count, dt, e[id].name};
    }
    free(e);
    return rc;
}

// ---- spvr_fit ----
enum { OpExtension = 10, OpCapability = 17, OpCopyMemory = 63, OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpPtrAccessChain = 67,
       OpInBoundsPtrAccessChain = 70, OpFunctionCall = 57, OpCopyObject = 83, OpBitcast = 124, OpSelect = 169, OpPhi = 245, OpAtomicFirst = 227, OpAtomicLast = 242, OpBeginInvocationInterlockEXT = 5364, OpEndInvocationInterlockEXT = 5365 };
enum { CapFloat64 = 10, CapSampleInterlock = 5363, CapShadingRateInterlock = 5372, CapPixelInterlock = 5378, DecNonWritable = 24 };
uint32_t *spvr_fit(const uint32_t *code, size_t words, const spvr_device *dev, size_t *out_words, unsigned *changes) {
    *changes = 0; *out_words = 0;
    if (words < 5 || code[0] != 0x07230203) return NULL;
    uint32_t nid = code[3];
    // what the module has and uses
    int f64type = 0, interlock_used = 0, model = -1;
    uint8_t *ptr_sc = calloc(nid, 1), *bufblock = calloc(nid, 1), *nonwr = calloc(nid, 1);   // pointer type → storage class + 1; struct ids decorated BufferBlock; NonWritable ids
    uint32_t *ptr_t = calloc(nid, sizeof *ptr_t), *arr = calloc(nid, sizeof *arr), *root = calloc(nid, sizeof *root);   // pointer → pointee; array → element; pointer value → its storage buffer variable
    uint8_t *isbuf = calloc(nid, 1), *written = calloc(nid, 1);
    if (!ptr_sc || !bufblock || !nonwr || !ptr_t || !arr || !root || !isbuf || !written) goto fail;
    for (int pass = 0; pass < 2; pass++)   // twice: an OpPhi can name a pointer defined further down (a loop's back edge)
    for (size_t i = 5; i < words;) {
        const uint32_t *w = code + i; uint32_t op = w[0] & 0xFFFF, n = w[0] >> 16; if (!n || i + n > words) goto fail;
        if (op == OpEntryPoint && model < 0) model = (int)w[1];
        else if (op == OpTypeFloat && w[2] == 64) f64type = 1;
        else if (op == OpBeginInvocationInterlockEXT || op == OpEndInvocationInterlockEXT) interlock_used = 1;
        else if (op == OpDecorate && w[1] < nid && w[2] == DecBufferBlock) bufblock[w[1]] = 1;
        else if (op == OpDecorate && w[1] < nid && w[2] == DecNonWritable) nonwr[w[1]] = 1;
        else if (op == OpTypePointer && w[1] < nid) { ptr_sc[w[1]] = (uint8_t)(w[2] + 1); ptr_t[w[1]] = w[3]; }
        else if ((op == OpTypeArray || op == OpTypeRuntimeArray) && w[1] < nid) arr[w[1]] = w[2];
        else if (op == OpVariable && w[2] < nid && w[1] < nid) {
            uint32_t t = ptr_t[w[1]]; for (int k = 0; k < 4 && t < nid && arr[t]; k++) t = arr[t];
            if (w[3] == SCStorageBuffer || (w[3] == SCUniform && t < nid && bufblock[t])) { isbuf[w[2]] = 1; root[w[2]] = w[2]; }
        } else if ((op == OpAccessChain || op == OpInBoundsAccessChain || op == OpPtrAccessChain || op == OpInBoundsPtrAccessChain) && w[2] < nid && w[3] < nid && root[w[3]]) root[w[2]] = root[w[3]];
        else if (op == OpStore && w[1] < nid && root[w[1]]) written[root[w[1]]] = 1;
        else if (op == OpCopyMemory && w[1] < nid && root[w[1]]) written[root[w[1]]] = 1;
        else if (op == OpAtomicFirst + 1) { if (w[1] < nid && root[w[1]]) written[root[w[1]]] = 1; }   // OpAtomicStore: pointer first
        else if (op > OpAtomicFirst + 1 && op <= OpAtomicLast && n > 3 && w[3] < nid && root[w[3]]) written[root[w[3]]] = 1;   // read-modify-write atomics: result type, result, pointer (OpAtomicLoad, 227, only reads)
        else if ((op == OpCopyObject || op == OpBitcast) && n > 3 && w[2] < nid && w[3] < nid && root[w[3]]) root[w[2]] = root[w[3]];   // the same pointer under another id
        else if (op == OpSelect && n > 5 && w[2] < nid) { for (uint32_t k = 4; k < 6; k++) if (w[k] < nid && root[w[k]]) { root[w[2]] = root[w[k]]; written[root[w[k]]] = 1; } }   // two buffers may meet: assume written
        else if (op == OpPhi && w[2] < nid) { for (uint32_t k = 3; k + 1 < n; k += 2) if (w[k] < nid && root[w[k]]) { root[w[2]] = root[w[k]]; written[root[w[k]]] = 1; } }
        else if (op == OpFunctionCall) for (uint32_t k = 4; k < n; k++) if (w[k] < nid && root[w[k]]) written[root[w[k]]] = 1;   // a pointer handed to a function: assume written
        i += n;
    }
    int drop_f64 = !dev->float64 && !f64type, drop_il = !dev->interlock && !interlock_used;
    int vertex_stage = model == 0 || model == 1 || model == 2 || model == 3;   // vertex, tessellation, geometry
    uint32_t nmark = 0; int stores = 0;
    if (!dev->vertex_stores && vertex_stage)
        for (uint32_t id = 0; id < nid; id++) if (isbuf[id] && !nonwr[id]) { if (written[id]) stores = 1; else nmark++; }
    uint32_t *o = malloc((words + 4 * nmark) * sizeof *o); if (!o) goto fail;
    size_t k = 0; int marked = 0;
    memcpy(o, code, 5 * sizeof *o); k = 5;
    for (size_t i = 5; i < words;) {
        const uint32_t *w = code + i; uint32_t op = w[0] & 0xFFFF, n = w[0] >> 16, drop = 0;
        if (op == OpCapability && drop_f64 && w[1] == CapFloat64) { drop = 1; *changes |= SPVR_FIT_FLOAT64; }
        if (op == OpCapability && drop_il && (w[1] == CapPixelInterlock || w[1] == CapSampleInterlock || w[1] == CapShadingRateInterlock)) { drop = 1; *changes |= SPVR_FIT_INTERLOCK; }
        if (op == OpExtension && drop_il && !strcmp((const char *)(w + 1), "SPV_EXT_fragment_shader_interlock")) { drop = 1; *changes |= SPVR_FIT_INTERLOCK; }
        if (op == OpExecutionMode && drop_il && w[2] >= 5366 && w[2] <= 5371) { drop = 1; *changes |= SPVR_FIT_INTERLOCK; }   // Pixel / Sample / ShadingRate Interlock Ordered / Unordered
        if (op == OpDecorate && !marked && nmark) {   // the decorations go before the first one
            for (uint32_t id = 0; id < nid; id++) if (isbuf[id] && !nonwr[id] && !written[id]) { o[k++] = (3u << 16) | OpDecorate; o[k++] = id; o[k++] = DecNonWritable; }
            marked = 1; *changes |= SPVR_FIT_NONWRITABLE;
        }
        if (!drop) { memcpy(o + k, w, n * sizeof *o); k += n; }
        i += n;
    }
    if (stores) *changes |= SPVR_FIT_STORES;
    free(ptr_sc); free(bufblock); free(nonwr); free(ptr_t); free(arr); free(root); free(isbuf); free(written);
    *out_words = k; return o;
fail:
    free(ptr_sc); free(bufblock); free(nonwr); free(ptr_t); free(arr); free(root); free(isbuf); free(written);
    return NULL;
}
