// Linux build: guest memory backed by Vulkan memory (vk_mem.h; the design is checked on the device by tests/t_placed.c).
// hle_svc.c names its regions (dm_gpu_region) and calls dm_gpu_commit before it opens memory in them. A chunk is backed the first time a commit touches it:
// device memory is mapped in place over the chunk (VK_EXT_map_memory_placed replaces the reservation's pages), and bound into the region's sparse buffer at the
// chunk's offset. Pages that were readable keep their bytes (copied out first, through process_vm_readv, which fails on an inaccessible page rather than faulting)
// and their access; the others are zeroed, as fresh pages are, and made inaccessible again, so hle_svc.c's own protection of the region is unchanged.
// The device exists before the game runs (front_sdl.c), so in practice no page of a chunk is open before it is backed, and no other thread can write it meanwhile.
// Chunks stay backed for the rest of the run (open: give back a chunk none of whose pages is committed)
#include "dm.h"
#include "vk_dev.h"
#include "vk_mem.h"
#include <sys/mman.h>
#include <sys/uio.h>

#define VKM_CHUNK (32ull << 20)
#define VKM_MAXREG 4
typedef struct {
    uint8_t *base; uint64_t size;
    VkBuffer buf; VkDeviceAddress gpu; uint32_t type;   // made when the first chunk is backed
    VkDeviceMemory *chunk;                               // one per VKM_CHUNK, VK_NULL_HANDLE until backed
} vkm_region;
static vkm_region g_reg[VKM_MAXREG]; static int g_nreg;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_chunks, g_bytes;

void dm_gpu_region(void *base, size_t size) {
    pthread_mutex_lock(&g_mu);
    if (g_nreg < VKM_MAXREG && size) {
        vkm_region *r = &g_reg[g_nreg++];
        r->base = base; r->size = size;
        r->chunk = calloc((size + VKM_CHUNK - 1) / VKM_CHUNK, sizeof *r->chunk);
    }
    pthread_mutex_unlock(&g_mu);
}

static int region_buffer(vkm_region *r) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT, .size = r->size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult vr = vkCreateBuffer(g_vkd.dev, &bi, NULL, &r->buf);
    if (vr) { LOG("[RED] vulkan: guest memory region %p+%#llx: no sparse buffer (%d)", (void *)r->base, (unsigned long long)r->size, (int)vr); return 0; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vkd.dev, r->buf, &mr);
    // host visible, coherent and cached: the CPU side is the game's own memory
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    r->type = UINT32_MAX;
    for (uint32_t i = 0; i < g_vkd.mem.memoryTypeCount && r->type == UINT32_MAX; i++)
        if ((mr.memoryTypeBits >> i & 1) && (g_vkd.mem.memoryTypes[i].propertyFlags & want) == want) r->type = i;
    if (r->type == UINT32_MAX || (VKM_CHUNK % mr.alignment) || ((uintptr_t)r->base % mr.alignment)) {
        LOG("[RED] vulkan: guest memory region %p+%#llx: no host-visible coherent cached memory type, or sparse alignment %#llx does not fit", (void *)r->base,
            (unsigned long long)r->size, (unsigned long long)mr.alignment);
        vkDestroyBuffer(g_vkd.dev, r->buf, NULL); r->buf = VK_NULL_HANDLE; return 0;
    }
    VkBufferDeviceAddressInfo ai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = r->buf};
    r->gpu = vkGetBufferDeviceAddress(g_vkd.dev, &ai);
    LOG("vulkan: guest memory region %p+%#llx is GPU buffer %#llx (memory type %u)", (void *)r->base, (unsigned long long)r->size, (unsigned long long)r->gpu, r->type);
    return 1;
}

static int back_chunk(vkm_region *r, uint64_t ci) {
    uint8_t *at = r->base + ci * VKM_CHUNK;
    uint64_t len = r->size - ci * VKM_CHUNK < VKM_CHUNK ? r->size - ci * VKM_CHUNK : VKM_CHUNK;
    size_t pg = (size_t)getpagesize(), npg = len / pg;
    // the pages that are open now, and their bytes
    uint8_t *open = calloc(npg, 1), *save = NULL; size_t nopen = 0;
    for (size_t p = 0; p < npg; p++) {
        static __thread uint8_t probe;
        struct iovec l = {&probe, 1}, rv = {at + p * pg, 1};
        if (process_vm_readv(getpid(), &l, 1, &rv, 1, 0) == 1) { open[p] = 1; nopen++; }
    }
    if (nopen) {
        save = malloc(nopen * pg); size_t k = 0;
        for (size_t p = 0; p < npg; p++) if (open[p]) memcpy(save + pg * k++, at + p * pg, pg);
    }
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = len, .memoryTypeIndex = r->type};
    VkDeviceMemory m; VkResult vr = vkAllocateMemory(g_vkd.dev, &ma, NULL, &m);
    if (vr) { LOG("[RED] vulkan: no device memory for guest chunk %p+%#llx (%d)", (void *)at, (unsigned long long)len, (int)vr); free(open); free(save); return 0; }
    VkMemoryMapPlacedInfoEXT pl = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = at};
    VkMemoryMapInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pl, .flags = VK_MEMORY_MAP_PLACED_BIT_EXT, .memory = m, .offset = 0, .size = VK_WHOLE_SIZE};
    void *p0 = NULL; vr = g_vkd.map2(g_vkd.dev, &mi, &p0);
    if (vr || p0 != at) die("vulkan: placing guest chunk %p+%#llx failed (%d, at %p): the region's pages are gone", (void *)at, (unsigned long long)len, (int)vr, p0);
    // the bytes back where they were; the rest zero, as fresh pages are (device memory may be reused)
    size_t k = 0;
    for (size_t p = 0; p < npg; p++) { if (open[p]) memcpy(at + p * pg, save + pg * k++, pg); else memset(at + p * pg, 0, pg); }
    for (size_t p = 0; p < npg;) {   // inaccessible again where it was, in runs
        if (open[p]) { p++; continue; }
        size_t q = p; while (q < npg && !open[q]) q++;
        if (mprotect(at + p * pg, (q - p) * pg, PROT_NONE)) die("vulkan: guest chunk %p: mprotect errno=%d", (void *)at, errno);
        p = q;
    }
    free(open); free(save);
    VkSparseMemoryBind sb = {ci * VKM_CHUNK, len, m, 0, 0};
    VkSparseBufferMemoryBindInfo sbi = {r->buf, 1, &sb};
    VkBindSparseInfo bsi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .bufferBindCount = 1, .pBufferBinds = &sbi};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence f; vkCreateFence(g_vkd.dev, &fci, NULL, &f);
    pthread_mutex_lock(&g_vkd_qlock); vr = vkQueueBindSparse(g_vkd.q, 1, &bsi, f); pthread_mutex_unlock(&g_vkd_qlock);
    if (!vr) vr = vkWaitForFences(g_vkd.dev, 1, &f, VK_TRUE, UINT64_MAX);
    vkDestroyFence(g_vkd.dev, f, NULL);
    if (vr) die("vulkan: binding guest chunk %p into its region's buffer failed (%d)", (void *)at, (int)vr);
    r->chunk[ci] = m; g_chunks++; g_bytes += len;
    if (!(g_chunks & (g_chunks - 1)) || nopen) LOG("vulkan: guest memory backed: %llu chunks, %llu MiB (chunk %p: %zu of %zu pages were open)", (unsigned long long)g_chunks,
                                                  (unsigned long long)(g_bytes >> 20), (void *)at, nopen, npg);
    return 1;
}

void dm_gpu_commit(void *addr, size_t size) {
    if (!vkd_ok() || !g_vkd.placed || !size) return;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nreg; i++) {
        vkm_region *r = &g_reg[i]; uint8_t *a = addr;
        if (a < r->base || a - r->base >= (ptrdiff_t)r->size) continue;
        if (!r->buf && !region_buffer(r)) break;
        uint64_t lo = (uint64_t)(a - r->base), hi = lo + size > r->size ? r->size : lo + size;
        for (uint64_t c = lo / VKM_CHUNK; c * VKM_CHUNK < hi; c++) if (!r->chunk[c] && !back_chunk(r, c)) break;
        break;
    }
    pthread_mutex_unlock(&g_mu);
}

int vkm_lookup(const void *addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off, VkDeviceAddress *gpu) {
    const uint8_t *a = addr;
    for (int i = 0; i < g_nreg; i++) {
        vkm_region *r = &g_reg[i];
        if (!r->buf || a < r->base || (uint64_t)(a - r->base) + len > r->size) continue;
        uint64_t lo = (uint64_t)(a - r->base), hi = lo + (len ? len : 1);
        for (uint64_t c = lo / VKM_CHUNK; c * VKM_CHUNK < hi; c++) if (!r->chunk[c]) return 0;
        *buf = r->buf; *off = lo; *gpu = r->gpu + lo; return 1;
    }
    return 0;
}
void vkm_stats(uint64_t *chunks, uint64_t *bytes) { pthread_mutex_lock(&g_mu); *chunks = g_chunks; *bytes = g_bytes; pthread_mutex_unlock(&g_mu); }
