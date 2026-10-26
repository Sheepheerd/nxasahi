// Linux build: the front end on SDL3, in place of front_mac.m. Without DM_WINDOW the game runs headless on this thread, as the Mac host. With DM_WINDOW=1 the main
// thread owns the window and its event loop and the game runs on a thread of its own (64 MiB stack, signal stack), as on the Mac. Lifecycle as front_mac.m: closing
// the window quits; focus lost → 2, regained → 1; minimized → 3 with the GPU suspended, restored → resumed. Input is not read yet (keyboard and controllers: LINUX.md)
#include "dm.h"
#include "hle.h"
#include "front.h"
#include "vk_window.h"
#include "vk_dev.h"
#include <SDL3/SDL.h>

typedef struct { void (*fn)(void); } guest_arg;
static void *guest_tramp(void *p) {
    static char alt[65536];   // the signal alternate stack is per thread; crash reports must work on the guest thread too
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    ((guest_arg *)p)->fn(); LOG("guest thread finished"); exit(0);
}

void front_run(void (*guest)(void)) {
    // The Vulkan device exists before the game runs: guest memory is backed with device memory as the game commits it (vk_mem.c). DM_VK=0: none (headless only)
    if (!getenv("DM_WINDOW") || !atoi(getenv("DM_WINDOW"))) {
        if (!getenv("DM_VK") || atoi(getenv("DM_VK"))) vkd_init(0);
        guest(); return;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) die("SDL_Init: %s", SDL_GetError());
    if (!vkd_init(1)) die("no Vulkan device for the window (see the vulkan lines above)");
    SDL_Window *w = SDL_CreateWindow(DM_GAME_NAME, 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!w) die("SDL_CreateWindow: %s", SDL_GetError());
    if (getenv("DM_FULLSCREEN") && atoi(getenv("DM_FULLSCREEN"))) SDL_SetWindowFullscreen(w, true);
    if (!vkw_init(w)) die("no Vulkan presentation for the window (see the vulkan lines above)");
    LOG("front end: SDL %d.%d.%d window on %s, display %d Hz", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
        SDL_GetCurrentVideoDriver(), vkw_display_hz());
    static guest_arg ga; ga.fn = guest;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
    pthread_t th; if (pthread_create(&th, &a, guest_tramp, &ga)) die("pthread_create guest");
    for (SDL_Event e; SDL_WaitEvent(&e);) switch (e.type) {
        case SDL_EVENT_QUIT: case SDL_EVENT_WINDOW_CLOSE_REQUESTED: LOG("front end: window closed"); exit(0);
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: vkw_resized(); break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: dm_focus_set(2); break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: dm_focus_set(1); break;
        case SDL_EVENT_WINDOW_MINIMIZED: dm_focus_set(3); dm_audio_suspend(1); dm_gfx_suspend(1); vkw_suspend(1); break;
        case SDL_EVENT_WINDOW_RESTORED: vkw_suspend(0); vkw_resized(); dm_gfx_suspend(0); dm_audio_suspend(0); dm_focus_set(2); break;
        default: break;
    }
    die("SDL_WaitEvent: %s", SDL_GetError());
}
int front_screen_max_fps(void) { return vkw_display_hz(); }
