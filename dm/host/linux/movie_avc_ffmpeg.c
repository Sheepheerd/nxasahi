// Linux build: H.264 (AVC) decoding for the movie decoder (hle_movie.c), the interface of hle_movie_avc.h on FFmpeg's libavcodec (hle_movie_avc.c is VideoToolbox, on the Mac).
// The SDK's demuxer hands over one access unit per packet with Annex B start codes; SPS / PPS come in Configure's csd-0 / csd-1 or an avcC record and may also appear in the stream.
// They are kept and put before the next access unit in Annex B form whenever they change, so the decoder meets them in the stream. libavcodec reorders by itself: pictures come out
// in presentation order and wait in a queue for mavc_take; at the end of the stream (flush) the decoder is drained, then reset for the next run. Pictures leave as NV12 (video range,
// as the SDK's decoder hands them out) or, in the native texture mode, as RGBA converted with the stream's colour matrix (libswscale)
#include "../dm.h"
#include "../hle_movie_avc.h"
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>

#define QMAX 16
struct mavc {
    uint8_t sps[512], pps[512]; int nsps, npps, dirty;
    AVCodecContext *cx; AVPacket *pkt; AVFrame *q[QMAX]; int nq;
    uint8_t *au; size_t aucap;
    struct SwsContext *sws; int red, rgba, drained;
};

mavc *mavc_new(void) { mavc *a = calloc(1, sizeof *a); if (!a) die("movie: AVC decoder allocation failed"); return a; }
void mavc_drop(mavc *a) { for (int i = 0; i < a->nq; i++) av_frame_free(&a->q[i]); a->nq = 0; if (a->cx) avcodec_flush_buffers(a->cx); a->drained = 0; }
void mavc_free(mavc *a) {
    if (!a) return;
    mavc_drop(a); avcodec_free_context(&a->cx); av_packet_free(&a->pkt); sws_freeContext(a->sws); free(a->au); free(a);
}
int mavc_queued(const mavc *a) { return a->nq; }
void mavc_set_rgba(mavc *a) { a->rgba = 1; }

static void keep_ps(mavc *a, const uint8_t *nal, size_t n) {   // a parameter set NAL unit (without start code)
    int t = nal[0] & 31; uint8_t *dst = t == 7 ? a->sps : a->pps; int *dn = t == 7 ? &a->nsps : &a->npps;
    if (n > sizeof a->sps) { if (!a->red++) LOG("[RED] movie: AVC parameter set of %zu bytes, too long", n); return; }
    if ((size_t)*dn == n && !memcmp(dst, nal, n)) return;
    memcpy(dst, nal, n); *dn = (int)n; a->dirty = 1;
}
// the NAL units of p (Annex B start codes, or else 4-byte lengths), as in hle_movie_avc.c
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
static void each_nal(const uint8_t *p, size_t n, void (*fn)(void *, const uint8_t *, size_t), void *ctx) {
    size_t s = next_sc(p, n, 0);
    if (s < n && s <= 1) {
        while (s < n) {
            size_t b = s + 3, e = next_sc(p, n, b), end = e;
            while (end > b && !p[end - 1]) end--;
            if (end > b) fn(ctx, p + b, end - b);
            s = e;
        }
    } else for (size_t i = 0; i + 4 <= n;) {
        size_t len = (size_t)p[i] << 24 | (size_t)p[i + 1] << 16 | (size_t)p[i + 2] << 8 | p[i + 3]; i += 4;
        if (!len || len > n - i) break;
        fn(ctx, p + i, len); i += len;
    }
}
static void ps_only(void *ctx, const uint8_t *nal, size_t n) { int t = nal[0] & 31; if (t == 7 || t == 8) keep_ps(ctx, nal, n); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { each_nal(p, n, ps_only, a); }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) {
    if (n < 7 || p[0] != 1) { if (!a->red++) LOG("[RED] movie: unrecognized avcC record (%zu bytes, version %d)", n, n ? p[0] : -1); return; }
    size_t i = 5;
    for (int set = 0; set < 2 && i < n; set++) {
        int cnt = set ? p[i] : p[i] & 31; i++;
        for (int k = 0; k < cnt && i + 2 <= n; k++) { size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2; if (len > n - i) return; if (len) keep_ps(a, p + i, len); i += len; }
    }
}

static int open_codec(mavc *a) {
    if (a->cx) return 1;
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!c) { if (!a->red++) LOG("[RED] movie: FFmpeg has no H.264 decoder, outputting black frames"); return 0; }
    a->cx = avcodec_alloc_context3(c); a->pkt = av_packet_alloc();
    if (!a->cx || !a->pkt) die("movie: AVC decoder allocation failed");
    a->cx->thread_count = 0; a->cx->thread_type = FF_THREAD_SLICE;   // slice threads add no frames of delay
    int e = avcodec_open2(a->cx, c, NULL);
    if (e < 0) { if (!a->red++) LOG("[RED] movie: FFmpeg cannot open its H.264 decoder (%d), outputting black frames", e); avcodec_free_context(&a->cx); return 0; }
    return 1;
}
static void put_bytes(mavc *a, size_t *len, const uint8_t *p, size_t n) {
    if (*len + n + AV_INPUT_BUFFER_PADDING_SIZE > a->aucap) { a->aucap = (*len + n + AV_INPUT_BUFFER_PADDING_SIZE) * 2; a->au = realloc(a->au, a->aucap); if (!a->au) die("movie: AVC buffer allocation failed"); }
    memcpy(a->au + *len, p, n); *len += n;
}
typedef struct { mavc *a; size_t len; } au_ctx;
static const uint8_t SC[4] = {0, 0, 0, 1};
static void au_add(void *ctx, const uint8_t *nal, size_t n) {
    au_ctx *c = ctx; int t = nal[0] & 31;
    if (t == 7 || t == 8) { keep_ps(c->a, nal, n); return; }
    if (t == 9) return;   // access unit delimiter
    put_bytes(c->a, &c->len, SC, 4); put_bytes(c->a, &c->len, nal, n);
}
static void queue(mavc *a, AVFrame *f) {
    if (a->nq == QMAX) { if (!a->red++) LOG("[RED] movie: AVC picture queue full, dropping the oldest picture"); av_frame_free(&a->q[0]); memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]); }
    a->q[a->nq++] = f;
}
static void receive(mavc *a) {
    for (;;) {
        AVFrame *f = av_frame_alloc(); if (!f) die("movie: AVC picture allocation failed");
        if (avcodec_receive_frame(a->cx, f) < 0) { av_frame_free(&f); return; }
        queue(a, f);
    }
}
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    au_ctx c = {a, 0};
    // parameter sets first (when new), then the slices: one Annex B packet
    uint8_t *tmp = NULL; size_t tl = 0;
    each_nal(p, (size_t)n, au_add, &c);
    if (!c.len) return 0;
    if (!open_codec(a)) return -1;
    if (a->drained) { avcodec_flush_buffers(a->cx); a->drained = 0; }
    if (a->dirty) {
        if (!a->nsps || !a->npps) { if (!a->red++) LOG("[RED] movie: AVC has no SPS / PPS, outputting black frames"); return -1; }
        tmp = malloc(c.len + a->nsps + a->npps + 8 + AV_INPUT_BUFFER_PADDING_SIZE); if (!tmp) die("movie: AVC buffer allocation failed");
        memcpy(tmp, SC, 4); memcpy(tmp + 4, a->sps, (size_t)a->nsps); tl = 4 + (size_t)a->nsps;
        memcpy(tmp + tl, SC, 4); memcpy(tmp + tl + 4, a->pps, (size_t)a->npps); tl += 4 + (size_t)a->npps;
        memcpy(tmp + tl, a->au, c.len); tl += c.len;
        LOG("[movie] AVC: FFmpeg decoding, profile %d level %d", a->nsps > 1 ? a->sps[1] : 0, a->nsps > 3 ? a->sps[3] : 0);
        a->dirty = 0;
    }
    av_packet_unref(a->pkt);
    a->pkt->data = tmp ? tmp : a->au; a->pkt->size = (int)(tmp ? tl : c.len); a->pkt->pts = pts_us;
    int e = avcodec_send_packet(a->cx, a->pkt);
    if (e == AVERROR(EAGAIN)) { receive(a); e = avcodec_send_packet(a->cx, a->pkt); }
    a->pkt->data = NULL; a->pkt->size = 0; free(tmp);
    receive(a);
    if (e < 0) { if (!a->red++) LOG("[RED] movie: AVC decode error (FFmpeg %d, first case)", e); return -1; }
    return 0;
}

static void take_rgba(mavc *a, const AVFrame *f, uint8_t *dst, int w, int h) {
    int cw = f->width < w ? f->width : w, chh = f->height < h ? f->height : h;
    if (cw < w || chh < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
    a->sws = sws_getCachedContext(a->sws, cw, chh, f->format, cw, chh, AV_PIX_FMT_RGBA, SWS_POINT, NULL, NULL, NULL);
    if (!a->sws) { if (!a->red++) LOG("[RED] movie: no RGBA conversion from pixel format %s", av_get_pix_fmt_name(f->format)); return; }
    int cs = f->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601, full = f->color_range == AVCOL_RANGE_JPEG;
    sws_setColorspaceDetails(a->sws, sws_getCoefficients(cs), full, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    uint8_t *d[4] = {dst}; int ds[4] = {w * 4};
    sws_scale(a->sws, (const uint8_t *const *)f->data, f->linesize, 0, chh, d, ds);
}
static void take_nv12(mavc *a, const AVFrame *f, uint8_t *dst, int w, int h) {
    int cw = f->width < w ? f->width : w, chh = f->height < h ? f->height : h;
    if (cw < w || chh < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
    uint8_t *uvo = dst + (size_t)w * h;
    if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) {
        for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w, f->data[0] + (size_t)r * f->linesize[0], (size_t)cw);
        for (int r = 0; r < (chh + 1) / 2 && r < h / 2; r++) {
            const uint8_t *u = f->data[1] + (size_t)r * f->linesize[1], *v = f->data[2] + (size_t)r * f->linesize[2]; uint8_t *o = uvo + (size_t)r * w;
            for (int x = 0; x < (cw + 1) / 2 && 2 * x + 1 < w; x++) { o[2 * x] = u[x]; o[2 * x + 1] = v[x]; }
        }
    } else if (f->format == AV_PIX_FMT_NV12) {
        for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w, f->data[0] + (size_t)r * f->linesize[0], (size_t)cw);
        size_t uvb = (size_t)((cw + 1) & ~1); if (uvb > (size_t)w) uvb = (size_t)w;
        for (int r = 0; r < (chh + 1) / 2 && r < h / 2; r++) memcpy(uvo + (size_t)r * w, f->data[1] + (size_t)r * f->linesize[1], uvb);
    } else if (!a->red++) LOG("[RED] movie: AVC pixel format %s not converted to NV12 (black frames)", av_get_pix_fmt_name(f->format));
}
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (!a->nq && flush && a->cx && !a->drained) { avcodec_send_packet(a->cx, NULL); receive(a); a->drained = 1; }   // end of stream: the pictures held for reordering
    if (!a->nq) return 0;
    AVFrame *f = a->q[0]; memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]);
    if (pts_us) *pts_us = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    if (a->rgba) take_rgba(a, f, dst, w, h); else take_nv12(a, f, dst, w, h);
    av_frame_free(&f);
    return 1;
}
