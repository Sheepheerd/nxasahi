// Linux build: the game window's Vulkan presentation (vk_window.c), behind the mtl_* present calls of nvn_obj.h (linux/headless.c) and the SDL front end (front_sdl.c)
#pragma once
#include <stdint.h>
struct SDL_Window;
int vkw_init(struct SDL_Window *w);   // on the main thread, after the window exists: instance, surface, device, swapchain, present thread; 0 on failure (logged)
int vkw_active(void);                 // a window is presenting
uint64_t vkw_present(double period_s);   // hand the next picture to the present thread; returns its ticket at once
void vkw_present_wait(uint64_t ticket);  // until that present was handed to the GPU (or replaced by a newer one)
void vkw_resized(void);               // the window's size changed: the swapchain is made again before the next present
void vkw_suspend(int on);             // minimized: presents are dropped (and counted as handed over) until restored
int vkw_display_hz(void);             // the window's display refresh rate (0 = unknown)
