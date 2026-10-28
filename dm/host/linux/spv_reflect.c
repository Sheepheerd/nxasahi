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
enum { OpMemoryModel = 14, ModeSignedZeroInfNanPreserve = 4461, CapSignedZeroInfNanPreserve = 4466 };
enum { OpExtInstImport = 11, OpExtInst = 12, GlslFMin = 37, GlslFMax = 40, GlslFClamp = 43, GlslNMin = 79, GlslNMax = 80, GlslNClamp = 81 };
enum { CapFloat64 = 10, CapSampleInterlock = 5363, CapShadingRateInterlock = 5372, CapPixelInterlock = 5378, DecNonWritable = 24 };
uint32_t *spvr_fit(const uint32_t *code, size_t words, const spvr_device *dev, size_t *out_words, unsigned *changes) {
    *changes = 0; *out_words = 0;
    if (words < 5 || code[0] != 0x07230203) return NULL;
    uint32_t nid = code[3];
    // what the module has and uses
    int f64type = 0, interlock_used = 0, model = -1; uint32_t glsl = 0;   // glsl: the GLSL.std.450 import
    uint32_t entries[8]; int nentry = 0, has_nanp = 0;   // entry points; the module already declares SignedZeroInfNanPreserve
    uint8_t *ptr_sc = calloc(nid, 1), *bufblock = calloc(nid, 1), *nonwr = calloc(nid, 1);   // pointer type → storage class + 1; struct ids decorated BufferBlock; NonWritable ids
    uint32_t *ptr_t = calloc(nid, sizeof *ptr_t), *arr = calloc(nid, sizeof *arr), *root = calloc(nid, sizeof *root);   // pointer → pointee; array → element; pointer value → its storage buffer variable
    uint8_t *isbuf = calloc(nid, 1), *written = calloc(nid, 1);
    if (!ptr_sc || !bufblock || !nonwr || !ptr_t || !arr || !root || !isbuf || !written) goto fail;
    for (int pass = 0; pass < 2; pass++)   // twice: an OpPhi can name a pointer defined further down (a loop's back edge)
    for (size_t i = 5; i < words;) {
        const uint32_t *w = code + i; uint32_t op = w[0] & 0xFFFF, n = w[0] >> 16; if (!n || i + n > words) goto fail;
        if (op == OpEntryPoint && !pass && nentry < 8) entries[nentry++] = w[2];
        if (op == OpCapability && w[1] == CapSignedZeroInfNanPreserve) has_nanp = 1;
        if (op == OpEntryPoint && model < 0) model = (int)w[1];
        else if (op == OpExtInstImport && n >= 3 && !strcmp((const char *)(w + 2), "GLSL.std.450")) glsl = w[1];
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
    int nanp = dev->nan_preserve && !has_nanp && nentry;
    uint32_t *o = malloc((words + 4 * nmark + 16 + 4 * (size_t)nentry) * sizeof *o); if (!o) goto fail;
    size_t k = 0; int marked = 0, nanp_state = 0;   // nanp_state: 1 capability in, 2 extension in, 3 execution modes in
    memcpy(o, code, 5 * sizeof *o); k = 5;
    for (size_t i = 5; i < words;) {
        const uint32_t *w = code + i; uint32_t op = w[0] & 0xFFFF, n = w[0] >> 16, drop = 0;
        if (op == OpCapability && drop_f64 && w[1] == CapFloat64) { drop = 1; *changes |= SPVR_FIT_FLOAT64; }
        if ((op == OpDecorate && n >= 3 && w[2] == 0) || (op == 72 && n >= 4 && w[3] == 0)) { drop = 1; *changes |= SPVR_FIT_RELAXED; }   // RelaxedPrecision (decoration 0) on an id or a member
        if (op == OpCapability && drop_il && (w[1] == CapPixelInterlock || w[1] == CapSampleInterlock || w[1] == CapShadingRateInterlock)) { drop = 1; *changes |= SPVR_FIT_INTERLOCK; }
        if (op == OpExtension && drop_il && !strcmp((const char *)(w + 1), "SPV_EXT_fragment_shader_interlock")) { drop = 1; *changes |= SPVR_FIT_INTERLOCK; }
        if (op == OpExecutionMode && drop_il && w[2] >= 5366 && w[2] <= 5371) { drop = 1; *changes |= SPVR_FIT_INTERLOCK; }   // Pixel / Sample / ShadingRate Interlock Ordered / Unordered
        // SignedZeroInfNanPreserve, each part where the module layout wants it: the capability among the capabilities, the extension before the imports and the memory
        // model, an execution mode per entry point after the last entry point. Maxwell keeps NaN and infinities through every operation, and the translated shaders depend on
        // it (their isnan selects, NMin / NMax); without the mode Mesa may assume neither occurs and fold the checks away
        if (nanp && nanp_state == 0 && op == OpCapability) { o[k++] = (2u << 16) | OpCapability; o[k++] = CapSignedZeroInfNanPreserve; nanp_state = 1; }
        if (nanp && nanp_state == 1 && (op == OpExtInstImport || op == OpMemoryModel)) {
            static const char ext[] = "SPV_KHR_float_controls"; uint32_t nw = 1 + (uint32_t)(sizeof ext + 3) / 4;
            o[k] = (nw << 16) | OpExtension; memset(o + k + 1, 0, (nw - 1) * 4); memcpy(o + k + 1, ext, sizeof ext); k += nw; nanp_state = 2;
        }
        if (nanp && nanp_state == 2 && op != OpEntryPoint && op != OpExtInstImport && op != OpMemoryModel && op != OpExtension && op != OpCapability) {
            for (int e = 0; e < nentry; e++) { o[k++] = (4u << 16) | OpExecutionMode; o[k++] = entries[e]; o[k++] = ModeSignedZeroInfNanPreserve; o[k++] = 32; }
            nanp_state = 3; *changes |= SPVR_FIT_NANPRESERVE;
        }
        if (op == OpDecorate && !marked && nmark) {   // the decorations go before the first one
            for (uint32_t id = 0; id < nid; id++) if (isbuf[id] && !nonwr[id] && !written[id]) { o[k++] = (3u << 16) | OpDecorate; o[k++] = id; o[k++] = DecNonWritable; }
            marked = 1; *changes |= SPVR_FIT_NONWRITABLE;
        }
        if (!drop) {
            memcpy(o + k, w, n * sizeof *o);
            // Maxwell's FMNMX returns the other operand when one is NaN, and saturation gives 0 for NaN; GLSL.std.450's FMin / FMax / FClamp leave the result undefined, and
            // Honeykrisp returns the NaN. NMin / NMax / NClamp are defined as Maxwell's (NClamp of NaN is its lower bound). Pokemon Shining Pearl's soft particles take
            // min(exp2(log2(0) * 0), 1) for a particle at the scene's depth: NaN, stored in an R11G11B10 target as its largest value, which the bloom spread over the bedroom.
            // The Mac's libraries do the same with Metal's precise:: functions (docs/reference/shaders.md, min, max and clamp with a NaN operand)
            if (op == OpExtInst && n >= 5 && glsl && w[3] == glsl && (w[4] == GlslFMin || w[4] == GlslFMax || w[4] == GlslFClamp)) {
                o[k + 4] = w[4] == GlslFMin ? GlslNMin : w[4] == GlslFMax ? GlslNMax : GlslNClamp; *changes |= SPVR_FIT_NMINMAX;
            }
            k += n;
        }
        i += n;
    }
    if (stores) *changes |= SPVR_FIT_STORES;
    free(ptr_sc); free(bufblock); free(nonwr); free(ptr_t); free(arr); free(root); free(isbuf); free(written);
    *out_words = k; return o;
fail:
    free(ptr_sc); free(bufblock); free(nonwr); free(ptr_t); free(arr); free(root); free(isbuf); free(written);
    return NULL;
}

// ---- spvr_gl_depth ----
enum { OpTypeVoid2 = 19, OpFunction = 54, OpFunctionEnd = 56, OpReturn = 253, OpLoad = 61, OpCompositeExtract = 81, OpCompositeInsert = 82, OpFAdd = 129, OpFMul = 133,
       OpMemberDecorate2 = 72, DecBuiltIn = 11, BuiltInPosition = 0, SCOutput = 3 };
uint32_t *spvr_gl_depth(const uint32_t *code, size_t words, size_t *out_words) {
    if (words < 5 || code[0] != 0x07230203) return NULL;
    uint32_t bound = code[3], entry = 0, t_float = 0, t_v4 = 0, t_int = 0, t_ptr_v4 = 0, c0 = 0, chalf = 0;
    uint32_t pos_var = 0, pos_struct = 0, pos_member = UINT32_MAX, block_var = 0;
    size_t first_fn = 0;
    // what is there: the entry point, float / vec4 / int types, an Output vec4 pointer, constants 0 and 0.5, where Position is
    for (size_t i = 5; i < words;) {
        const uint32_t *w = code + i; uint32_t op = w[0] & 0xFFFF, n = w[0] >> 16; if (!n || i + n > words) return NULL;
        if (op == OpEntryPoint && !entry) entry = w[2];
        else if (op == OpTypeFloat && w[2] == 32 && !t_float) t_float = w[1];
        else if (op == OpTypeInt && w[2] == 32 && !t_int) t_int = w[1];
        else if (op == OpDecorate && w[2] == DecBuiltIn && w[3] == BuiltInPosition) pos_var = w[1];
        else if (op == OpMemberDecorate2 && w[3] == DecBuiltIn && w[4] == BuiltInPosition) { pos_struct = w[1]; pos_member = w[2]; }
        else if (op == OpFunction && !first_fn) first_fn = i;
        i += n;
    }
    for (size_t i = 5; i < words;) {   // second pass: what depends on the first
        const uint32_t *w = code + i; uint32_t op = w[0] & 0xFFFF, n = w[0] >> 16;
        if (op == OpTypeVector && w[2] == t_float && w[3] == 4 && !t_v4) t_v4 = w[1];
        else if (op == OpConstant && w[1] == t_int && w[3] == 0 && !c0) c0 = w[2];
        else if (op == OpConstant && w[1] == t_float && w[3] == 0x3f000000u && !chalf) chalf = w[2];
        i += n;
    }
    for (size_t i = 5; i < words;) {
        const uint32_t *w = code + i; uint32_t op = w[0] & 0xFFFF, n = w[0] >> 16;
        if (op == OpTypePointer && w[2] == SCOutput && w[3] == t_v4 && t_v4 && !t_ptr_v4) t_ptr_v4 = w[1];
        i += n;
    }
    if (pos_struct && !pos_var) {   // the Output variable of the block that holds Position
        uint32_t ptr_to_struct = 0;
        for (size_t i = 5; i < words;) { const uint32_t *w = code + i; uint32_t op = w[0] & 0xFFFF, n = w[0] >> 16;
            if (op == OpTypePointer && w[2] == SCOutput && w[3] == pos_struct) ptr_to_struct = w[1]; i += n; }
        for (size_t i = 5; i < words && ptr_to_struct;) { const uint32_t *w = code + i; uint32_t op = w[0] & 0xFFFF, n = w[0] >> 16;
            if (op == OpVariable && w[1] == ptr_to_struct && w[3] == SCOutput) block_var = w[2]; i += n; }
    }
    if ((!pos_var && !block_var) || !t_float || !t_v4 || !entry || !first_fn) return NULL;
    // the declarations it needs and lacks, inserted before the first function (after every type and constant)
    uint32_t decl[16]; size_t nd = 0;
    if (!t_int && block_var) { t_int = bound++; decl[nd++] = (4u << 16) | OpTypeInt; decl[nd++] = t_int; decl[nd++] = 32; decl[nd++] = 1; }
    if (!c0 && block_var) { c0 = bound++; decl[nd++] = (4u << 16) | OpConstant; decl[nd++] = t_int; decl[nd++] = c0; decl[nd++] = 0; }
    if (!chalf) { chalf = bound++; decl[nd++] = (4u << 16) | OpConstant; decl[nd++] = t_float; decl[nd++] = chalf; decl[nd++] = 0x3f000000u; }
    if (!t_ptr_v4) { t_ptr_v4 = bound++; decl[nd++] = (4u << 16) | OpTypePointer; decl[nd++] = t_ptr_v4; decl[nd++] = SCOutput; decl[nd++] = t_v4; }
    uint32_t *o = malloc((words + nd + 64 * 32) * sizeof *o); if (!o) return NULL;
    size_t k = 0, cap = words + nd + 64 * 32; int in_entry = 0, nret = 0;
    for (size_t i = 0; i < words;) {
        if (i == first_fn) { memcpy(o + k, decl, nd * sizeof *o); k += nd; }
        const uint32_t *w = code + i; uint32_t op = i < 5 ? 0 : w[0] & 0xFFFF, n = i < 5 ? 5 : w[0] >> 16;
        if (op == OpFunction) in_entry = w[2] == entry;
        if (op == OpFunctionEnd) in_entry = 0;
        if (op == OpReturn && in_entry) {
            if (k + 40 > cap) { cap *= 2; uint32_t *no = realloc(o, cap * sizeof *o); if (!no) { free(o); return NULL; } o = no; }
            uint32_t p = bound++, v = bound++, z = bound++, ww = bound++, s = bound++, hh = bound++, v2 = bound++;
            if (block_var) { o[k++] = (5u << 16) | OpAccessChain; o[k++] = t_ptr_v4; o[k++] = p; o[k++] = block_var; o[k++] = c0;   // c0: Position is member 0 (pos_member)
                if (pos_member != 0) { free(o); return NULL; } }
            else p = pos_var;
            o[k++] = (4u << 16) | OpLoad; o[k++] = t_v4; o[k++] = v; o[k++] = p;
            o[k++] = (5u << 16) | OpCompositeExtract; o[k++] = t_float; o[k++] = z; o[k++] = v; o[k++] = 2;
            o[k++] = (5u << 16) | OpCompositeExtract; o[k++] = t_float; o[k++] = ww; o[k++] = v; o[k++] = 3;
            o[k++] = (5u << 16) | OpFAdd; o[k++] = t_float; o[k++] = s; o[k++] = z; o[k++] = ww;
            o[k++] = (5u << 16) | OpFMul; o[k++] = t_float; o[k++] = hh; o[k++] = s; o[k++] = chalf;
            o[k++] = (6u << 16) | OpCompositeInsert; o[k++] = t_v4; o[k++] = v2; o[k++] = hh; o[k++] = v; o[k++] = 2;
            o[k++] = (3u << 16) | OpStore; o[k++] = p; o[k++] = v2;
            nret++;
        }
        if (k + n > cap) { cap = cap * 2 + n; uint32_t *no = realloc(o, cap * sizeof *o); if (!no) { free(o); return NULL; } o = no; }
        memcpy(o + k, w, n * sizeof *o); k += n; i += n;
    }
    if (!nret) { free(o); return NULL; }
    o[3] = bound;
    *out_words = k; return o;
}

// ---- spvr_init_outputs ----
enum { OpTypeFunction2 = 33, OpConstantComposite = 44, OpLabel = 248, OpVariable2 = 59, SCFunction = 7 };
uint32_t *spvr_init_outputs(const uint32_t *code, size_t words, size_t *out_words) {
    if (words < 5 || code[0] != 0x07230203) return NULL;
    uint32_t bound = code[3], entry = 0, model = UINT32_MAX, t_float = 0, t_v4 = 0;
    size_t first_fn = 0;
    uint8_t *builtin = calloc(bound, 1); uint32_t *ptr_sc = calloc(bound, sizeof *ptr_sc), *ptr_t = calloc(bound, sizeof *ptr_t), outs[64]; int nout = 0;
    if (!builtin || !ptr_sc || !ptr_t) goto fail;
    for (size_t i = 5; i < words;) {
        const uint32_t *w = code + i; uint32_t op = w[0] & 0xFFFF, n = w[0] >> 16; if (!n || i + n > words) goto fail;
        if (op == OpEntryPoint && !entry) { model = w[1]; entry = w[2]; }
        else if (op == OpTypeFloat && w[2] == 32 && !t_float) t_float = w[1];
        else if (op == OpTypeVector && w[2] == t_float && t_float && w[3] == 4 && !t_v4) t_v4 = w[1];
        else if (op == OpDecorate && w[2] == DecBuiltIn && w[1] < bound) builtin[w[1]] = 1;
        else if (op == OpTypePointer && w[1] < bound) { ptr_sc[w[1]] = w[2] + 1; ptr_t[w[1]] = w[3]; }
        else if (op == OpVariable2 && w[1] < bound && w[2] < bound && w[3] == SCOutput && ptr_t[w[1]] == t_v4 && t_v4 && !builtin[w[2]] && nout < 64) outs[nout++] = w[2];
        else if (op == OpFunction && !first_fn) first_fn = i;
        i += n;
    }
    if ((model != 0 && model != 2) || !nout || !first_fn || !t_v4) goto fail;   // vertex and tessellation evaluation shaders (a geometry shader's outputs are per emitted vertex)
    uint32_t c0 = bound++, c1 = bound++, cv = bound++;
    uint32_t decl[] = {(4u << 16) | OpConstant, t_float, c0, 0, (4u << 16) | OpConstant, t_float, c1, 0x3f800000u, (7u << 16) | OpConstantComposite, t_v4, cv, c0, c0, c0, c1};
    size_t nd = sizeof decl / sizeof *decl;
    uint32_t *o = malloc((words + nd + 3 * (size_t)nout) * sizeof *o); if (!o) goto fail;
    size_t k = 0; int in_entry = 0, state = 0;   // state: 1 after the entry's first OpLabel (its OpVariables follow), 2 once the stores are in
    for (size_t i = 0; i < words;) {
        if (i == first_fn) { memcpy(o + k, decl, sizeof decl); k += nd; }
        const uint32_t *w = code + i; uint32_t op = i < 5 ? 0 : w[0] & 0xFFFF, n = i < 5 ? 5 : w[0] >> 16;
        if (op == OpFunction) in_entry = w[2] == entry;
        if (in_entry && state == 1 && !(op == OpVariable2 && w[3] == SCFunction)) {
            for (int j = 0; j < nout; j++) { o[k++] = (3u << 16) | OpStore; o[k++] = outs[j]; o[k++] = cv; }
            state = 2;
        }
        memcpy(o + k, w, n * sizeof *o); k += n;
        if (in_entry && op == OpLabel && !state) state = 1;
        i += n;
    }
    o[3] = bound;
    free(builtin); free(ptr_sc); free(ptr_t);
    *out_words = k; return o;
fail:
    free(builtin); free(ptr_sc); free(ptr_t);
    return NULL;
}

// ---- spvr_nan ----
// Two places where Maxwell and this GPU part ways on non-finite values (Pokemon Shining Pearl's bedroom: its TV's flicker divides by a speed of 0, the angle becomes
// infinity - infinity, and the colour comes from cos(NaN)):
// - MUFU.SIN / MUFU.COS of NaN or of an infinity give NaN; GLSL.std.450's Sin / Cos leave it undefined, and the driver gave a large finite number, which no NaN check
//   downstream catches. Each float Sin / Cos is made select(isnan(x) || isinf(x), NaN, sin(x)).
// - A NaN written to a float render target stays NaN on Maxwell, and what reads it later (FMNMX, saturation: the bloom's threshold, the tonemapper) drops it. This GPU
//   stores it as the format's largest value (65024 in R11G11B10, 65504 in RGBA16F), which the bloom spread over the bedroom as a white glow. Fragment outputs are
//   written as select(isnan(v), 0, v): what a Maxwell reader using min / max or saturation makes of the NaN. Open: a reader that tests isnan itself sees 0 instead.
// - A NaN output of the vertex, tessellation or geometry stage reaches the fragment shader as NaN on Maxwell; here interpolation turns it into a large finite number
//   (the TV's colour arrived as one, and the fragment shader's own NaN check could not see it). Their user outputs (not the builtins) get the same select: the
//   fragment shader computes from 0 where Maxwell's would have computed NaN and had it stored as 0
enum { OpIsNan = 156, OpIsInf = 157, OpLogicalOr = 166, OpTypeBool = 20, GlslSin = 13, GlslCos = 14, ExecFragment = 4 };
uint32_t *spvr_nan(const uint32_t *code, size_t words, size_t *out_words, unsigned *changes) {
    *changes = 0; *out_words = 0;
    if (words < 5 || code[0] != 0x07230203) return NULL;
    uint32_t bound = code[3], model = UINT32_MAX, glsl = 0, t_float = 0, t_v4 = 0, t_bool = 0, t_bv4 = 0;
    size_t first_fn = 0, nsin = 0, nstore = 0;
    uint8_t *builtin = calloc(bound, 1), *out_ptr = calloc(bound, 1);   // ids decorated BuiltIn (variables, and blocks with a BuiltIn member: gl_PerVertex); pointers into a user Output variable
    uint32_t *ptr_t = calloc(bound, sizeof *ptr_t), *id_t = calloc(bound, sizeof *id_t), *arr_t = calloc(bound, sizeof *arr_t);   // pointer type → pointee; pointer id → its type; array → element
    uint32_t *o = NULL;
    if (!builtin || !out_ptr || !ptr_t || !id_t || !arr_t) goto done;
    for (int pass = 0; pass < 2; pass++)   // twice: the counts need the types and output pointers the first pass found
    for (size_t i = 5; i < words;) {
        const uint32_t *w = code + i; uint32_t op = w[0] & 0xFFFF, n = w[0] >> 16; if (!n || i + n > words) goto done;
        if (!pass) {
            if (op == OpEntryPoint && model == UINT32_MAX) model = w[1];
            else if (op == OpExtInstImport && n >= 3 && !strcmp((const char *)(w + 2), "GLSL.std.450")) glsl = w[1];
            else if (op == OpTypeFloat && w[2] == 32 && !t_float) t_float = w[1];
            else if (op == OpTypeBool && !t_bool) t_bool = w[1];
            else if (op == OpTypeVector && w[3] == 4 && t_float && w[2] == t_float && !t_v4) t_v4 = w[1];
            else if (op == OpTypeVector && w[3] == 4 && t_bool && w[2] == t_bool && !t_bv4) t_bv4 = w[1];
            else if (op == OpDecorate && w[2] == DecBuiltIn && w[1] < bound) builtin[w[1]] = 1;
            else if (op == OpMemberDecorate2 && n >= 4 && w[3] == DecBuiltIn && w[1] < bound) builtin[w[1]] = 1;   // the struct type
            else if (op == OpTypePointer && w[1] < bound) ptr_t[w[1]] = w[3];
            else if (op == OpVariable2 && w[1] < bound && w[2] < bound) {
                id_t[w[2]] = w[1]; uint32_t pt = ptr_t[w[1]];   // an arrayed output (tessellation control): its element type
                for (int a = 0; a < 2 && pt < bound && arr_t[pt]; a++) pt = arr_t[pt];
                if (w[3] == SCOutput && !builtin[w[2]] && !(pt < bound && builtin[pt])) out_ptr[w[2]] = 1;
            }
            else if ((op == OpTypeArray || op == OpTypeRuntimeArray) && w[1] < bound) arr_t[w[1]] = w[2];
            else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && w[1] < bound && w[2] < bound && w[3] < bound) { id_t[w[2]] = w[1]; if (out_ptr[w[3]]) out_ptr[w[2]] = 1; }
            else if (op == OpFunction && !first_fn) first_fn = i;
        } else if (op == OpExtInst && n >= 6 && glsl && w[3] == glsl && (w[4] == GlslSin || w[4] == GlslCos) && w[1] == t_float) nsin++;
        else if (op == OpStore && model <= ExecFragment && w[1] < bound && out_ptr[w[1]] && id_t[w[1]] < bound) { uint32_t vt = ptr_t[id_t[w[1]]]; if ((vt == t_float && t_float) || (vt == t_v4 && t_v4)) nstore++; }
        i += n;
    }
    if ((!nsin && !nstore) || !first_fn || !t_float) goto done;   // nothing to do: NULL, the caller keeps its code
    // declarations, before the first function: the bool types where missing, 0 and NaN, a vec4 of 0
    uint32_t decl[32]; size_t nd = 0;
    if (!t_bool) { t_bool = bound++; decl[nd++] = (2u << 16) | OpTypeBool; decl[nd++] = t_bool; }
    if (t_v4 && !t_bv4) { t_bv4 = bound++; decl[nd++] = (4u << 16) | OpTypeVector; decl[nd++] = t_bv4; decl[nd++] = t_bool; decl[nd++] = 4; }
    uint32_t c0 = bound++, cnan = bound++, cv0 = t_v4 ? bound++ : 0;
    decl[nd++] = (4u << 16) | OpConstant; decl[nd++] = t_float; decl[nd++] = c0; decl[nd++] = 0;
    decl[nd++] = (4u << 16) | OpConstant; decl[nd++] = t_float; decl[nd++] = cnan; decl[nd++] = 0x7fc00000u;
    if (t_v4) { decl[nd++] = (7u << 16) | OpConstantComposite; decl[nd++] = t_v4; decl[nd++] = cv0; decl[nd++] = c0; decl[nd++] = c0; decl[nd++] = c0; decl[nd++] = c0; }
    o = malloc((words + nd + 25 * nsin + 10 * nstore) * sizeof *o); if (!o) goto done;
    size_t k = 0;
    for (size_t i = 0; i < words;) {
        if (i == first_fn) { memcpy(o + k, decl, nd * sizeof *o); k += nd; }
        const uint32_t *w = code + i; uint32_t op = i < 5 ? 0 : w[0] & 0xFFFF, n = i < 5 ? 5 : w[0] >> 16;
        if (i >= 5 && op == OpExtInst && n >= 6 && glsl && w[3] == glsl && (w[4] == GlslSin || w[4] == GlslCos) && w[1] == t_float) {
            uint32_t x = w[5], r0 = bound++, isn = bound++, isi = bound++, bad = bound++;
            uint32_t seq[] = {(6u << 16) | OpExtInst, t_float, r0, glsl, w[4], x,
                              (4u << 16) | OpIsNan, t_bool, isn, x, (4u << 16) | OpIsInf, t_bool, isi, x, (5u << 16) | OpLogicalOr, t_bool, bad, isn, isi,
                              (6u << 16) | OpSelect, t_float, w[2], bad, cnan, r0};
            memcpy(o + k, seq, sizeof seq); k += sizeof seq / sizeof *seq; *changes |= 1;
        } else if (i >= 5 && op == OpStore && model <= ExecFragment && w[1] < bound && out_ptr[w[1]] && id_t[w[1]] < bound
                   && ((ptr_t[id_t[w[1]]] == t_float && t_float) || (ptr_t[id_t[w[1]]] == t_v4 && t_v4))) {
            int v4 = ptr_t[id_t[w[1]]] == t_v4; uint32_t isn = bound++, sel = bound++;
            uint32_t seq[] = {(4u << 16) | OpIsNan, v4 ? t_bv4 : t_bool, isn, w[2], (6u << 16) | OpSelect, v4 ? t_v4 : t_float, sel, isn, v4 ? cv0 : c0, w[2]};
            memcpy(o + k, seq, sizeof seq); k += sizeof seq / sizeof *seq;
            memcpy(o + k, w, n * sizeof *o); o[k + 2] = sel; k += n;   // the store itself (with any memory-access operands), of the selected value
            *changes |= 2;
        } else { memcpy(o + k, w, n * sizeof *o); k += n; }
        i += n;
    }
    o[3] = bound;
    *out_words = k;
done:
    free(builtin); free(out_ptr); free(ptr_t); free(id_t); free(arr_t);
    if (o && !*out_words) { free(o); o = NULL; }
    return o;
}
