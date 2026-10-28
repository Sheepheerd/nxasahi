// Linux build: AudioConverter (AudioToolbox/AudioToolbox.h) for the one conversion the host asks of it, AAC packets to interleaved PCM16 (hle_movie.c md_aac), on FFmpeg's AAC decoder.
// The configuration comes as AudioToolbox's magic cookie, an MPEG-4 ES descriptor (hle_movie.c md_aac_open builds it); the AudioSpecificConfig inside (tag 5) is the decoder's extradata.
// FillComplexBuffer pulls packets from the input procedure until the output is full or the procedure returns a status, and returns that status with what was decoded, as AudioToolbox does
#include "../dm.h"
#include <AudioToolbox/AudioToolbox.h>
#include <libavcodec/avcodec.h>

struct dm_audio_converter { AVCodecContext *cx; AVPacket *pkt; AVFrame *fr; int ch, rate, red; };

OSStatus AudioConverterNew(const AudioStreamBasicDescription *in, const AudioStreamBasicDescription *out, AudioConverterRef *c) {
    *c = NULL;
    if (in->mFormatID != kAudioFormatMPEG4AAC || out->mFormatID != kAudioFormatLinearPCM || out->mBitsPerChannel != 16 || out->mChannelsPerFrame != in->mChannelsPerFrame) return DM_AT_FAIL;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!codec) { LOG("[RED] movie: FFmpeg has no AAC decoder"); return DM_AT_FAIL; }
    AudioConverterRef a = calloc(1, sizeof *a); if (!a) return DM_AT_FAIL;
    a->ch = (int)in->mChannelsPerFrame; a->rate = (int)in->mSampleRate;
    a->cx = avcodec_alloc_context3(codec); a->pkt = av_packet_alloc(); a->fr = av_frame_alloc();
    if (!a->cx || !a->pkt || !a->fr) { AudioConverterDispose(a); return DM_AT_FAIL; }
    a->cx->sample_rate = a->rate; av_channel_layout_default(&a->cx->ch_layout, a->ch);
    *c = a; return noErr;
}
OSStatus AudioConverterDispose(AudioConverterRef a) {
    if (!a) return noErr;
    avcodec_free_context(&a->cx); av_packet_free(&a->pkt); av_frame_free(&a->fr); free(a); return noErr;
}
// an MPEG-4 descriptor's length: up to four bytes of 7 bits, the high bit set on all but the last
static int descr(const uint8_t *p, size_t n, size_t *i, int *tag, size_t *len) {
    if (*i >= n) return 0;
    *tag = p[(*i)++]; *len = 0;
    for (int k = 0; k < 4 && *i < n; k++) { uint8_t b = p[(*i)++]; *len = *len << 7 | (b & 0x7f); if (!(b & 0x80)) return *len <= n - *i; }
    return 0;
}
OSStatus AudioConverterSetProperty(AudioConverterRef a, UInt32 id, UInt32 size, const void *data) {
    if (!a || id != kAudioConverterDecompressionMagicCookie || avcodec_is_open(a->cx)) return DM_AT_FAIL;
    const uint8_t *p = data; size_t i = 0, len; int tag;
    // ES descriptor (3): ES ID and flags (3 bytes), then the decoder configuration (4): object type, stream type, buffer size and bit rates (13 bytes), then the specific info (5)
    if (!descr(p, size, &i, &tag, &len) || tag != 3) return DM_AT_FAIL; i += 3;
    if (!descr(p, size, &i, &tag, &len) || tag != 4) return DM_AT_FAIL; i += 13;
    if (!descr(p, size, &i, &tag, &len) || tag != 5 || !len) return DM_AT_FAIL;
    a->cx->extradata = av_mallocz(len + AV_INPUT_BUFFER_PADDING_SIZE); if (!a->cx->extradata) return DM_AT_FAIL;
    memcpy(a->cx->extradata, p + i, len); a->cx->extradata_size = (int)len;
    int e = avcodec_open2(a->cx, NULL, NULL);
    if (e < 0) { LOG("[RED] movie: FFmpeg's AAC decoder does not accept the configuration (%d)", e); return DM_AT_FAIL; }
    return noErr;
}
static inline int16_t s16(float v) { v *= 32768.0f; return (int16_t)(v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : v); }
// the frame's samples (planar or interleaved float, or PCM16) into o as interleaved PCM16 of ch channels; at most room frames
static UInt32 put(AudioConverterRef a, const AVFrame *f, int16_t *o, UInt32 room) {
    UInt32 n = (UInt32)f->nb_samples; int fch = f->ch_layout.nb_channels;
    if (n > room) { if (!a->red++) LOG("[RED] movie: AAC frame of %u samples does not fit the %u left in the output, cut", n, room); n = room; }
    for (UInt32 s = 0; s < n; s++) for (int c = 0; c < a->ch; c++) {
        int k = c < fch ? c : fch - 1; int16_t v;
        switch (f->format) {
            case AV_SAMPLE_FMT_FLTP: v = s16(((const float *)f->extended_data[k])[s]); break;
            case AV_SAMPLE_FMT_FLT: v = s16(((const float *)f->extended_data[0])[s * fch + k]); break;
            case AV_SAMPLE_FMT_S16P: v = ((const int16_t *)f->extended_data[k])[s]; break;
            case AV_SAMPLE_FMT_S16: v = ((const int16_t *)f->extended_data[0])[s * fch + k]; break;
            default: if (!a->red++) LOG("[RED] movie: AAC sample format %d not converted", f->format); return 0;
        }
        o[s * a->ch + c] = v;
    }
    return n;
}
OSStatus AudioConverterFillComplexBuffer(AudioConverterRef a, AudioConverterComplexInputDataProc in, void *ud, UInt32 *frames, AudioBufferList *out, AudioStreamPacketDescription *pd) {
    (void)pd;
    UInt32 want = *frames, got = 0; *frames = 0;
    if (!a || !avcodec_is_open(a->cx)) return DM_AT_FAIL;
    int16_t *o = out->mBuffers[0].mData;
    OSStatus st = noErr;
    while (got < want) {
        int e;
        while (got < want && (e = avcodec_receive_frame(a->cx, a->fr)) >= 0) { got += put(a, a->fr, o + (size_t)got * a->ch, want - got); av_frame_unref(a->fr); }
        if (got >= want) break;
        UInt32 np = 1; AudioBufferList bl = {1, {{(UInt32)a->ch, 0, NULL}}}; AudioStreamPacketDescription *d = NULL;
        st = in(a, &np, &bl, &d, ud);
        if (st || !np || !bl.mBuffers[0].mData) break;
        av_packet_unref(a->pkt);
        if (av_new_packet(a->pkt, (int)bl.mBuffers[0].mDataByteSize) < 0) { st = DM_AT_FAIL; break; }
        memcpy(a->pkt->data, bl.mBuffers[0].mData, bl.mBuffers[0].mDataByteSize);
        e = avcodec_send_packet(a->cx, a->pkt);
        if (e < 0 && e != AVERROR(EAGAIN)) { if (!a->red++) LOG("[RED] movie: AAC decode error (FFmpeg %d, first case)", e); }
    }
    *frames = got; out->mBuffers[0].mDataByteSize = got * 2u * (UInt32)a->ch;
    return st;
}
