// Linux build: the game window's Vulkan presentation (vk_window.c), behind the mtl_* present calls of nvn_obj.h (linux/headless.c) and the SDL front end (front_sdl.c)
#pragma once
#include <stdint.h>
struct SDL_Window;
int vkw_init(struct SDL_Window *w);   // on the main thread, after the window exists: instance, surface, device, swapchain, present thread; 0 on failure (logged)
int vkw_active(void);                 // a window is presenting

// Hand the next picture to the present thread, returning its ticket at once: the game's texture t (copied: it lives in game memory; NULL = the placeholder), shown once
// the replay's timeline reaches wait (the submissions before the present), cropped to crop (x, y, w, h in texels; w or h 0 = whole)
uint64_t vkw_present(const void *t, uint64_t wait, const int crop[4], double period_s);
uint64_t vkw_present_rgba(const void *rgba, uint32_t w, uint32_t h);   // a picture in host memory, RGBA8 rows bottom-up (an OpenGL frame read back), shown as vkw_present shows a texture
void vkw_present_wait(uint64_t ticket);  // until that present was handed to the GPU (or replaced by a newer one)
void vkw_resized(void);               // the window's size changed: the swapchain is made again before the next present
void vkw_suspend(int on);             // minimized: presents are dropped (and counted as handed over) until restored
int vkw_display_hz(void);             // the window's display refresh rate (0 = unknown)
