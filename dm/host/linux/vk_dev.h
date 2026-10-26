// Linux build: the one Vulkan instance and device the host uses (vk_dev.c): the window's presentation (vk_window.c), guest memory backing (vk_mem.c) and the backend
#pragma once
#include <pthread.h>
#include <vulkan/vulkan.h>
struct SDL_Window;
typedef struct {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qf;
    VkPhysicalDeviceProperties props; VkPhysicalDeviceMemoryProperties mem;
    int window;                   // made with the window's instance extensions and VK_KHR_swapchain
    int placed;                   // VK_EXT_map_memory_placed (guest memory backed in place, vk_mem.c)
    PFN_vkMapMemory2KHR map2;
} vkd_t;
extern vkd_t g_vkd;
// Before the game starts (front_sdl.c): with a window, after SDL's video init (its instance extensions), else headless. 0 = no usable device (logged); the host then runs without one
int vkd_init(int with_window);
static inline int vkd_ok(void) { return g_vkd.dev != VK_NULL_HANDLE; }
// The queue is shared by the present thread, sparse binds and the backend: every vkQueue* call holds this
extern pthread_mutex_t g_vkd_qlock;
