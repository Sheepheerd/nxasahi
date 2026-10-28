// Linux build: OpenGL games (main imports EGL and fetches its GL functions with eglGetProcAddress: Balatro's LÖVE) run on the host's own OpenGL, Mesa's GL 4.6 for Apple's GPU.
// The Mac translates every GL call to Metal (dm/gfx/gl/hle_gl.m). Here the game's code runs in the host's address space and calls with the same AArch64 convention, so the GL
// functions are Mesa's own, called directly: guest pointers are host pointers, and Mesa compiles the game's GLSL itself (no shader translation or capture).
// This file answers the guest's EGL imports on a surfaceless Mesa EGL display:
//   · the Switch window (nn::vi's native window) becomes a pbuffer of the layer's size (the surfaceless platform has no window surfaces), configs are chosen with the pbuffer bit;
//   · eglSwapBuffers reads the frame back (glReadPixels of the default framebuffer, pack state saved and restored) and hands it to the window or the screenshots
//     (vkr_present_pixels), paced to the swap interval at 60 Hz;
//   · eglGetProcAddress gives Mesa's functions; the ones that can compile shaders (compiles, links, draws, dispatches, clears, mipmap generation) through a stub that runs them
//     on this thread's big host stack (hle_bigstack.c), since a game thread's stack can be small. The stub carries register arguments only, so functions with more than
//     eight integer arguments (glTexSubImage2D, glBlitFramebuffer, …) are called directly.
#include "../hle.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <time.h>

uint64_t vkr_present_pixels(const uint8_t *rgba, uint32_t w, uint32_t h);   // vk_replay.c
void dm_vi_layer_size(int *w, int *h);                                       // hle_vi.c

// ---- the host display ----
static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static EGLDisplay host_display(void) {
    pthread_mutex_lock(&g_mu);
    if (g_dpy == EGL_NO_DISPLAY) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (gpd) g_dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (g_dpy == EGL_NO_DISPLAY) LOG("[RED] gl: no surfaceless EGL display (EGL_MESA_platform_surfaceless): OpenGL games cannot draw");
        else LOG("gl: host EGL display (surfaceless Mesa)");
    }
    pthread_mutex_unlock(&g_mu);
    return g_dpy;
}

// ---- GL functions the layer itself calls ----
typedef void (*fn_rp)(int, int, int, int, unsigned, unsigned, void *);
typedef void (*fn_gi)(unsigned, int *);
typedef void (*fn_bf)(unsigned, unsigned);
typedef void (*fn_pi)(unsigned, int);
typedef const unsigned char *(*fn_gs)(unsigned);
static struct { fn_rp read_pixels; fn_gi get_integerv; fn_bf bind_framebuffer, bind_buffer; fn_pi pixel_storei; fn_gs get_string; } G;
static void gl_own(void) {
    if (G.read_pixels) return;
    G.read_pixels = (fn_rp)eglGetProcAddress("glReadPixels"); G.get_integerv = (fn_gi)eglGetProcAddress("glGetIntegerv");
    G.bind_framebuffer = (fn_bf)eglGetProcAddress("glBindFramebuffer"); G.bind_buffer = (fn_bf)eglGetProcAddress("glBindBuffer");
    G.pixel_storei = (fn_pi)eglGetProcAddress("glPixelStorei"); G.get_string = (fn_gs)eglGetProcAddress("glGetString");
}
enum { GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401, GL_READ_FRAMEBUFFER = 0x8ca8, GL_READ_FRAMEBUFFER_BINDING = 0x8caa, GL_PIXEL_PACK_BUFFER = 0x88eb,
       GL_PIXEL_PACK_BUFFER_BINDING = 0x88ed, GL_PACK_ALIGNMENT = 0x0d05, GL_PACK_ROW_LENGTH = 0x0d02, GL_PACK_SKIP_ROWS = 0x0d03, GL_PACK_SKIP_PIXELS = 0x0d04,
       GL_VENDOR = 0x1f00, GL_RENDERER = 0x1f01, GL_VERSION = 0x1f02 };

// ---- eglGetProcAddress: Mesa's functions, the shader-compiling ones on the big stack ----
#define NBIG 512
void *g_gl_big[NBIG];   // the stubs' targets (gl_big_stubs: stub k jumps to g_gl_big[k] on the big stack)
extern const char gl_big_stubs[];
__asm__(".text\n.p2align 2\n.globl gl_big_stubs\n.hidden gl_big_stubs\ngl_big_stubs:\n.set gl_big_k, 0\n.rept 512\n"
        "    adrp x16, g_gl_big + gl_big_k * 8\n    ldr x16, [x16, :lo12:g_gl_big + gl_big_k * 8]\n    b dm_bigstack_enter\n.set gl_big_k, gl_big_k + 1\n.endr\n");
static int big_name(const char *n) {
    static const char *const pre[] = {"glDraw", "glMultiDraw", "glDispatch", "glCompileShader", "glLinkProgram", "glUseProgram", "glValidateProgram", "glProgramBinary",
                                      "glGetProgramiv", "glGetShaderiv", "glClear", "glGenerateMipmap", "glGenerateTextureMipmap", "glFinish", "glFlush", "glCreateShaderProgram"};
    for (size_t i = 0; i < sizeof pre / sizeof *pre; i++) if (!strncmp(n, pre[i], strlen(pre[i]))) return 1;
    return 0;
}
static struct { const char *name; void *stub; } g_big_names[NBIG]; static int g_nbig;
static void *big_stub(const char *name, void *fn) {   // under g_mu
    for (int i = 0; i < g_nbig; i++) if (g_big_names[i].stub && !strcmp(g_big_names[i].name, name)) return g_big_names[i].stub;
    if (g_nbig >= NBIG) { LOG("[RED] gl: more than %d big-stack GL functions, %s called directly", NBIG, name); return fn; }
    int k = g_nbig++; g_gl_big[k] = fn; __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_big_names[k].name = strdup(name); g_big_names[k].stub = (void *)(gl_big_stubs + 12 * k);
    return g_big_names[k].stub;
}
static void *egl_own(const char *name);   // the layer's EGL functions, by name (below)

// ---- EGL take-overs ----
// Registered as plain HLE (no module defines EGL: NVIDIA's driver modules are not loaded); the heavy ones (driver load, context creation, the swap's read-back) on the big stack
#define HLE_BIG(mangled, ret, cname, ...)                                                          \
    static ret cname(__VA_ARGS__) __attribute__((used));                                          \
    extern const char cname##_bs[];                                                               \
    __asm__(HLE_BIG_STUB(cname));                                                                 \
    __attribute__((used, section("__DATA,swhle"))) static const hle_ent hle_ent_##cname = {mangled, (void *)(uintptr_t)cname##_bs}; \
    static ret cname(__VA_ARGS__)

HLE_BIG("eglGetDisplay", EGLDisplay, h_eglGetDisplay, void *native) { (void)native; return host_display(); }
HLE_BIG("eglInitialize", EGLBoolean, h_eglInitialize, EGLDisplay d, EGLint *major, EGLint *minor) {
    EGLBoolean ok = eglInitialize(d, major, minor);
    LOG("gl: eglInitialize %s (EGL %d.%d, %s)", ok ? "ok" : "failed", major ? *major : 0, minor ? *minor : 0, ok ? eglQueryString(d, EGL_VENDOR) : "-");
    return ok;
}
HLE("eglTerminate", EGLBoolean, h_eglTerminate, EGLDisplay d) { (void)d; return EGL_TRUE; }   // kept: SDL terminates and initializes again when it makes the window again
HLE("eglReleaseThread", EGLBoolean, h_eglReleaseThread, void) { return eglReleaseThread(); }
HLE("eglBindAPI", EGLBoolean, h_eglBindAPI, EGLenum api) { return api ? eglBindAPI(api) : EGL_TRUE; }   // SDL binds 0 before it has chosen an API
HLE("eglQueryAPI", EGLenum, h_eglQueryAPI, void) { return eglQueryAPI(); }
HLE("eglGetError", EGLint, h_eglGetError, void) { return eglGetError(); }
HLE("eglQueryString", const char *, h_eglQueryString, EGLDisplay d, EGLint name) { return eglQueryString(d, name); }
HLE("eglWaitGL", EGLBoolean, h_eglWaitGL, void) { return eglWaitGL(); }
HLE("eglWaitNative", EGLBoolean, h_eglWaitNative, EGLint engine) { (void)engine; return EGL_TRUE; }
// The attributes as given, with the surface type asked for as a pbuffer (the window becomes one)
HLE_BIG("eglChooseConfig", EGLBoolean, h_eglChooseConfig, EGLDisplay d, const EGLint *attr, EGLConfig *cfg, EGLint n, EGLint *num) {
    EGLint a[128]; int k = 0;
    for (int i = 0; attr && attr[i] != EGL_NONE && k < 124; i += 2) {
        a[k++] = attr[i]; a[k++] = attr[i] == EGL_SURFACE_TYPE ? EGL_PBUFFER_BIT : attr[i + 1];
    }
    int st = 0; for (int i = 0; i < k; i += 2) st |= a[i] == EGL_SURFACE_TYPE;
    if (!st) { a[k++] = EGL_SURFACE_TYPE; a[k++] = EGL_PBUFFER_BIT; }   // left out, EGL takes the window bit (SDL leaves it out), which a surfaceless display has no config for
    a[k] = EGL_NONE;
    EGLBoolean ok = eglChooseConfig(d, a, cfg, n, num);
    if (!ok || (num && !*num)) {
        char t[512]; int o = 0; for (int i = 0; a[i] != EGL_NONE && o < 480; i += 2) o += snprintf(t + o, sizeof t - o, " %#x=%d", a[i], a[i + 1]);
        LOG("[RED] gl: eglChooseConfig found no config for%s (EGL error %#x)", t, eglGetError());
    }
    return ok;
}
HLE("eglGetConfigAttrib", EGLBoolean, h_eglGetConfigAttrib, EGLDisplay d, EGLConfig cfg, EGLint attr, EGLint *v) {
    EGLBoolean ok = eglGetConfigAttrib(d, cfg, attr, v);
    if (ok && attr == EGL_SURFACE_TYPE) *v |= EGL_WINDOW_BIT;   // what the game asked for: its window is the pbuffer
    return ok;
}
HLE_BIG("eglCreateContext", EGLContext, h_eglCreateContext, EGLDisplay d, EGLConfig cfg, EGLContext share, const EGLint *attr) {
    EGLContext c = eglCreateContext(d, cfg, share, attr);
    int major = 0, minor = 0; for (int i = 0; attr && attr[i] != EGL_NONE && i < 64; i += 2) { if (attr[i] == EGL_CONTEXT_MAJOR_VERSION) major = attr[i + 1]; else if (attr[i] == EGL_CONTEXT_MINOR_VERSION) minor = attr[i + 1]; }
    if (c == EGL_NO_CONTEXT) LOG("[RED] gl: eglCreateContext (OpenGL %d.%d) failed, EGL error %#x", major, minor, eglGetError());
    else LOG("gl: context for OpenGL %d.%d%s", major, minor, share ? " (shared)" : "");
    return c;
}
HLE("eglDestroyContext", EGLBoolean, h_eglDestroyContext, EGLDisplay d, EGLContext c) { return eglDestroyContext(d, c); }
static struct { EGLSurface s; int w, h; } g_win;   // the window's pbuffer
HLE_BIG("eglCreateWindowSurface", EGLSurface, h_eglCreateWindowSurface, EGLDisplay d, EGLConfig cfg, void *win, const EGLint *attr) {
    (void)win; (void)attr;
    int w, h; dm_vi_layer_size(&w, &h); if (w <= 0 || h <= 0) { w = 1280; h = 720; }
    const EGLint pa[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, cfg, pa);
    if (s == EGL_NO_SURFACE) { LOG("[RED] gl: the window's %dx%d pbuffer could not be made (EGL error %#x)", w, h, eglGetError()); return s; }
    g_win.s = s; g_win.w = w; g_win.h = h;
    LOG("gl: window surface %dx%d (a pbuffer, read back at each swap)", w, h);
    return s;
}
HLE_BIG("eglCreatePbufferSurface", EGLSurface, h_eglCreatePbufferSurface, EGLDisplay d, EGLConfig cfg, const EGLint *attr) { return eglCreatePbufferSurface(d, cfg, attr); }
HLE("eglDestroySurface", EGLBoolean, h_eglDestroySurface, EGLDisplay d, EGLSurface s) { if (s == g_win.s) g_win.s = EGL_NO_SURFACE; return eglDestroySurface(d, s); }
HLE_BIG("eglMakeCurrent", EGLBoolean, h_eglMakeCurrent, EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c) {
    EGLBoolean ok = eglMakeCurrent(d, dr, rd, c);
    static int told; if (ok && c != EGL_NO_CONTEXT && !told++) { gl_own(); LOG("gl: %s, OpenGL %s (%s)", G.get_string ? (const char *)G.get_string(GL_RENDERER) : "?", G.get_string ? (const char *)G.get_string(GL_VERSION) : "?", G.get_string ? (const char *)G.get_string(GL_VENDOR) : "?"); }
    if (!ok) LOG("[RED] gl: eglMakeCurrent failed, EGL error %#x", eglGetError());
    return ok;
}
static int g_interval = 1;
HLE("eglSwapInterval", EGLBoolean, h_eglSwapInterval, EGLDisplay d, EGLint n) { (void)d; if (n != g_interval) LOG("gl: swap interval %d", n); g_interval = n; return EGL_TRUE; }
static uint64_t g_next_swap;   // the earliest time of the next swap (paced to the swap interval at 60 Hz)
HLE_BIG("eglSwapBuffers", EGLBoolean, h_eglSwapBuffers, EGLDisplay d, EGLSurface s) {
    dm_gfx_enter();
    if (s == g_win.s && s != EGL_NO_SURFACE) {
        static uint8_t *px; static size_t cap; size_t n = (size_t)g_win.w * g_win.h * 4;
        if (cap < n) { free(px); px = malloc(n); cap = px ? n : 0; }
        gl_own();
        if (px && G.read_pixels) {
            int rfb = 0, pbo = 0, al = 4, rl = 0, sr = 0, sp = 0;
            G.get_integerv(GL_READ_FRAMEBUFFER_BINDING, &rfb); G.get_integerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo); G.get_integerv(GL_PACK_ALIGNMENT, &al);
            G.get_integerv(GL_PACK_ROW_LENGTH, &rl); G.get_integerv(GL_PACK_SKIP_ROWS, &sr); G.get_integerv(GL_PACK_SKIP_PIXELS, &sp);
            G.bind_framebuffer(GL_READ_FRAMEBUFFER, 0); G.bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
            G.pixel_storei(GL_PACK_ALIGNMENT, 4); G.pixel_storei(GL_PACK_ROW_LENGTH, 0); G.pixel_storei(GL_PACK_SKIP_ROWS, 0); G.pixel_storei(GL_PACK_SKIP_PIXELS, 0);
            G.read_pixels(0, 0, g_win.w, g_win.h, GL_RGBA, GL_UNSIGNED_BYTE, px);
            G.bind_framebuffer(GL_READ_FRAMEBUFFER, (unsigned)rfb); G.bind_buffer(GL_PIXEL_PACK_BUFFER, (unsigned)pbo);
            G.pixel_storei(GL_PACK_ALIGNMENT, al); G.pixel_storei(GL_PACK_ROW_LENGTH, rl); G.pixel_storei(GL_PACK_SKIP_ROWS, sr); G.pixel_storei(GL_PACK_SKIP_PIXELS, sp);
            vkr_present_pixels(px, (uint32_t)g_win.w, (uint32_t)g_win.h);
        }
    }
    EGLBoolean ok = eglSwapBuffers(d, s);
    if (g_interval > 0) {
        uint64_t period = 1000000000ull * (uint64_t)(g_interval > 4 ? 4 : g_interval) / 60, now = now_ns();
        if (g_next_swap > now) { uint64_t w = g_next_swap - now; struct timespec ts = {(time_t)(w / 1000000000ull), (long)(w % 1000000000ull)}; nanosleep(&ts, NULL); }
        now = now_ns(); g_next_swap = g_next_swap && now < g_next_swap + period ? g_next_swap + period : now + period;
    }
    dm_gfx_leave();
    dm_guest_frame_tick();
    return ok;
}
HLE("eglGetProcAddress", void *, h_eglGetProcAddress, const char *name) {
    if (!name) return NULL;
    if (!strncmp(name, "egl", 3)) { void *f = egl_own(name); return f ? f : (void *)eglGetProcAddress(name); }
    void *fn = (void *)eglGetProcAddress(name);
    if (!fn || !big_name(name)) return fn;
    pthread_mutex_lock(&g_mu); void *st = big_stub(name, fn); pthread_mutex_unlock(&g_mu);
    return st;
}
// The layer's own EGL functions by name, for eglGetProcAddress("egl…")
static void *egl_own(const char *name) {
    static const struct { const char *n; const void *f; } t[] = {
        {"eglGetDisplay", h_eglGetDisplay_bs}, {"eglInitialize", h_eglInitialize_bs}, {"eglTerminate", (void *)h_eglTerminate}, {"eglReleaseThread", (void *)h_eglReleaseThread},
        {"eglBindAPI", (void *)h_eglBindAPI}, {"eglQueryAPI", (void *)h_eglQueryAPI}, {"eglGetError", (void *)h_eglGetError}, {"eglQueryString", (void *)h_eglQueryString},
        {"eglWaitGL", (void *)h_eglWaitGL}, {"eglWaitNative", (void *)h_eglWaitNative}, {"eglChooseConfig", h_eglChooseConfig_bs}, {"eglGetConfigAttrib", (void *)h_eglGetConfigAttrib},
        {"eglCreateContext", h_eglCreateContext_bs}, {"eglDestroyContext", (void *)h_eglDestroyContext}, {"eglCreateWindowSurface", h_eglCreateWindowSurface_bs},
        {"eglCreatePbufferSurface", h_eglCreatePbufferSurface_bs}, {"eglDestroySurface", (void *)h_eglDestroySurface}, {"eglMakeCurrent", h_eglMakeCurrent_bs},
        {"eglSwapInterval", (void *)h_eglSwapInterval}, {"eglSwapBuffers", h_eglSwapBuffers_bs}, {"eglGetProcAddress", (void *)h_eglGetProcAddress}};
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) if (!strcmp(t[i].n, name)) return (void *)t[i].f;
    return NULL;
}
