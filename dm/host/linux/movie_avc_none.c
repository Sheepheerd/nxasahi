// Linux build: the H.264 decoder interface of hle_movie_avc.h without a decoder (hle_movie_avc.c is VideoToolbox). Every access unit reports an error, so the
// movie player takes its decode-failure path, as on a device whose VideoToolbox refuses the stream. Open: decoding through FFmpeg's libavcodec
#include "../dm.h"
#include "../hle_movie_avc.h"
struct mavc { int warned; };
mavc *mavc_new(void) { return calloc(1, sizeof(mavc)); }
void mavc_free(mavc *a) { free(a); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { (void)a; (void)p; (void)n; }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) { (void)a; (void)p; (void)n; }
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    (void)p; (void)n; (void)pts_us;
    if (a && !a->warned++) LOG("[RED] movie: no H.264 decoder in the Linux host yet (the movie shows no picture)");
    return -1;
}
int mavc_take(mavc *a, int flush, uint8_t *nv12, int w, int h, int64_t *pts_us) { (void)a; (void)flush; (void)nv12; (void)w; (void)h; (void)pts_us; return 0; }
int mavc_queued(const mavc *a) { (void)a; return 0; }
void mavc_set_rgba(mavc *a) { (void)a; }
void mavc_drop(mavc *a) { (void)a; }
