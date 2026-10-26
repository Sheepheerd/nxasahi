// The memory design of the Vulkan backend (LINUX.md, step 3), checked on the device: guest memory regions (the heap, the alias region) are backed by Vulkan memory
// in chunks mapped in place at their own addresses (VK_EXT_map_memory_placed), and each region is one sparse buffer the chunks are bound into at the same offsets,
// so the CPU and the GPU see the same bytes, contiguous on both sides. Steps, as the host would take them:
//   1. reserve an address range PROT_NONE (hle_svc.c reserves the guest address space so) and commit part of it (mprotect), writing data there
//   2. back two adjacent 32 MiB chunks with device memory: copy their contents out, map the memory in place, copy back
//   3. bind both chunks into one sparse buffer; a compute shader reads the CPU's data and writes a pattern across the chunk boundary
//   4. the CPU reads the GPU's writes at the original addresses, and data written before the backing survived
// Build and run: make -f Makefile.linux t_placed
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <vulkan/vulkan.h>
#define CHUNK (32ull << 20)
#define CHECK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s: VkResult %d\n", #x, r_); return 1; } } while (0)

// t_placed.comp: reads the first words (the CPU's data) and writes a span across the chunk boundary
static const uint32_t SPV[] = {
#include "t_placed.spv.inc"
};

int main(void) {
    // 1. the reservation and some committed, written memory in it
    size_t reserve = 4 * CHUNK;
    uint8_t *rs = mmap(NULL, reserve + CHUNK, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    uint8_t *base = (uint8_t *)(((uintptr_t)rs + CHUNK - 1) & ~(CHUNK - 1));   // chunk aligned, as the host's chunks are
    if (mprotect(base, 2 * CHUNK, PROT_READ | PROT_WRITE)) { printf("FAIL commit\n"); return 1; }
    uint32_t *w = (uint32_t *)base; for (uint32_t i = 0; i < 4096; i++) w[i] = 0x9e3779b9u * (i + 1);
    w[(CHUNK + 4096) / 4] = 0xfeedface;   // data in the second chunk, away from what the GPU writes

    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance inst; CHECK(vkCreateInstance(&ii, NULL, &inst));
    uint32_t np = 8; VkPhysicalDevice pds[8], pd = VK_NULL_HANDLE; vkEnumeratePhysicalDevices(inst, &np, pds);
    for (uint32_t i = 0; i < np; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) { pd = pds[i]; printf("device %s\n", p.deviceName); break; } }
    if (!pd) { printf("FAIL no GPU\n"); return 1; }
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT mp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT, .memoryMapPlaced = VK_TRUE};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &mp, .bufferDeviceAddress = VK_TRUE};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12, .features = {.sparseBinding = VK_TRUE, .sparseResidencyBuffer = VK_TRUE}};
    uint32_t nq = 8; VkQueueFamilyProperties qp[8]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    uint32_t qf = UINT32_MAX; for (uint32_t i = 0; i < nq; i++) if ((qp[i].queueFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_SPARSE_BINDING_BIT)) == (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_SPARSE_BINDING_BIT)) { qf = i; break; }
    if (qf == UINT32_MAX) { printf("FAIL no queue family with compute and sparse binding\n"); return 1; }
    float prio = 1; VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio};
    const char *ext[] = {VK_KHR_MAP_MEMORY_2_EXTENSION_NAME, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .enabledExtensionCount = 2, .ppEnabledExtensionNames = ext};
    VkDevice dev; CHECK(vkCreateDevice(pd, &di, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, qf, 0, &q);
    PFN_vkMapMemory2KHR map2 = (PFN_vkMapMemory2KHR)vkGetDeviceProcAddr(dev, "vkMapMemory2KHR");

    // 3a. the region's sparse buffer (created first: its memory requirements choose the memory type)
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT, .size = reserve,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buf; CHECK(vkCreateBuffer(dev, &bi, NULL, &buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mprops; vkGetPhysicalDeviceMemoryProperties(pd, &mprops);
    uint32_t mt = UINT32_MAX, want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    for (uint32_t i = 0; i < mprops.memoryTypeCount; i++) if ((mr.memoryTypeBits >> i & 1) && (mprops.memoryTypes[i].propertyFlags & want) == want) { mt = i; break; }
    if (mt == UINT32_MAX) { printf("FAIL no host-visible coherent cached memory type for the sparse buffer\n"); return 1; }
    printf("sparse buffer: alignment %#llx, memory type %u\n", (unsigned long long)mr.alignment, mt);

    // 2. back two chunks in place
    VkDeviceMemory mem[2]; uint8_t *save = malloc(CHUNK);
    for (int c = 0; c < 2; c++) {
        uint8_t *at = base + c * CHUNK;
        memcpy(save, at, CHUNK);
        VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = CHUNK, .memoryTypeIndex = mt};
        CHECK(vkAllocateMemory(dev, &ma, NULL, &mem[c]));
        VkMemoryMapPlacedInfoEXT pl = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = at};
        VkMemoryMapInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pl, .flags = VK_MEMORY_MAP_PLACED_BIT_EXT, .memory = mem[c], .offset = 0, .size = VK_WHOLE_SIZE};
        void *p = NULL; CHECK(map2(dev, &mi, &p));
        if (p != at) { printf("FAIL chunk %d mapped at %p, not %p\n", c, p, (void *)at); return 1; }
        memcpy(at, save, CHUNK);
    }
    printf("backed 2 chunks in place at %p\n", (void *)base);
    if (w[1] != 0x9e3779b9u * 2 || w[(CHUNK + 4096) / 4] != 0xfeedface) { printf("FAIL data lost while backing\n"); return 1; }

    // 3b. bind both chunks into the sparse buffer at their offsets
    VkSparseMemoryBind sb[2] = {{0, CHUNK, mem[0], 0, 0}, {CHUNK, CHUNK, mem[1], 0, 0}};
    VkSparseBufferMemoryBindInfo sbi = {buf, 2, sb};
    VkBindSparseInfo bsi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .bufferBindCount = 1, .pBufferBinds = &sbi};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    CHECK(vkQueueBindSparse(q, 1, &bsi, fence)); CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX)); vkResetFences(dev, 1, &fence);

    // 3c. the compute shader: 4096 words starting 2048 words before the chunk boundary
    VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb};
    VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl));
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkPipelineLayout pl; CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof SPV, .pCode = SPV};
    VkShaderModule sm; CHECK(vkCreateShaderModule(dev, &smi, NULL, &sm));
    VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"}};
    VkPipeline pipe; CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
    VkDescriptorSet ds; CHECK(vkAllocateDescriptorSets(dev, &dai, &ds));
    VkDescriptorBufferInfo dbi = {buf, 0, 2 * CHUNK};
    VkWriteDescriptorSet wd = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(dev, 1, &wd, 0, NULL);
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf}; VkCommandPool cp; CHECK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkBeginCommandBuffer(cb, &cbi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    uint32_t pc[2] = {(uint32_t)(CHUNK / 4 - 2048), 0x5a5a5a5au}; vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
    vkCmdDispatch(cb, 4096 / 64, 1, 1);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(vkQueueSubmit(q, 1, &si, fence)); CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));

    // 4. the CPU sees the GPU's writes on both sides of the boundary, at the original addresses
    int bad = 0;
    for (uint32_t i = 0; i < 4096; i++) if (w[CHUNK / 4 - 2048 + i] != ((0x9e3779b9u * (i + 1)) ^ 0x5a5a5a5au)) bad++;
    if (w[(CHUNK + 4096) / 4 + 0] != 0xfeedface && (CHUNK + 4096) / 4 >= CHUNK / 4 + 2048) bad++;
    VkBufferDeviceAddressInfo bai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buf};
    printf("region GPU address %#llx, CPU address %p\n", (unsigned long long)vkGetBufferDeviceAddress(dev, &bai), (void *)base);
    printf("%s: %d of 4096 words wrong across the chunk boundary\n", bad ? "FAIL" : "PASS", bad);
    return bad != 0;
}
