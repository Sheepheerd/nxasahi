// Linux build: the output AudioUnit hle_audio.c drives (AudioToolbox/AudioToolbox.h), on SDL3 audio. Both are pull models: SDL's stream callback asks for bytes and
// this calls the unit's render callback for that many frames, in the format the host set (16-bit PCM, 48 kHz, its channel count); SDL converts to the device.
// The device buffer is asked to be about 5 ms (240 frames), as the Mac host asks of CoreAudio. Without a device (no sound server), initializing still succeeds and
// starting fails, so the host's silent sink keeps the game's audio paced and its watchdog retries
#include <AudioToolbox/AudioToolbox.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dm.h"

struct dm_audio_component { int unused; };
struct dm_audio_unit {
    AudioStreamBasicDescription fmt; int has_fmt;
    AURenderCallbackStruct cb;
    SDL_AudioStream *stream;
    uint8_t *buf; size_t cap;
    double sample_time;
};
static struct dm_audio_component g_output;

AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d) {
    return !after && d && d->componentType == kAudioUnitType_Output ? &g_output : NULL;
}
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out) {
    *out = c ? calloc(1, sizeof **out) : NULL;
    return *out ? noErr : DM_AT_FAIL;
}
OSStatus AudioComponentInstanceDispose(AudioComponentInstance u) { if (u) { AudioUnitUninitialize(u); free(u->buf); free(u); } return noErr; }

// SDL's audio thread: render what the device asks for into the stream
static void SDLCALL feed(void *ud, SDL_AudioStream *st, int additional, int total) {
    (void)total; AudioUnit u = ud;
    if (additional <= 0 || !u->cb.inputProc) return;
    UInt32 bpf = u->fmt.mBytesPerFrame ? u->fmt.mBytesPerFrame : 4, frames = (UInt32)additional / bpf;
    if (!frames) frames = 1;
    size_t n = (size_t)frames * bpf;
    if (n > u->cap) { uint8_t *b = realloc(u->buf, n); if (!b) return; u->buf = b; u->cap = n; }
    AudioBufferList bl = {1, {{u->fmt.mChannelsPerFrame, (UInt32)n, u->buf}}};
    AudioUnitRenderActionFlags fl = 0; AudioTimeStamp ts = {.mSampleTime = u->sample_time, .mFlags = 1};
    if (u->cb.inputProc(u->cb.inputProcRefCon, &fl, &ts, 0, frames, &bl)) memset(u->buf, 0, n);
    u->sample_time += frames;
    SDL_PutAudioStreamData(st, u->buf, (int)n);
}
OSStatus AudioUnitInitialize(AudioUnit u) {
    if (!u || !u->has_fmt || u->fmt.mFormatID != kAudioFormatLinearPCM || u->fmt.mBitsPerChannel != 16) return DM_AT_FAIL;
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) {
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "240");
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { LOG("[RED] audio: SDL audio did not start (%s): no sound output", SDL_GetError()); return noErr; }
        LOG("audio: SDL %s output", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
    }
    SDL_AudioSpec spec = {SDL_AUDIO_S16, (int)u->fmt.mChannelsPerFrame, (int)u->fmt.mSampleRate};
    u->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, u);   // opened paused
    if (!u->stream) LOG("[RED] audio: no SDL output device (%s): no sound output", SDL_GetError());
    return noErr;
}
OSStatus AudioUnitUninitialize(AudioUnit u) { if (u && u->stream) { SDL_DestroyAudioStream(u->stream); u->stream = NULL; } return noErr; }
OSStatus AudioOutputUnitStart(AudioUnit u) { return u && u->stream && SDL_ResumeAudioStreamDevice(u->stream) ? noErr : DM_AT_FAIL; }
OSStatus AudioOutputUnitStop(AudioUnit u) { if (u && u->stream) SDL_PauseAudioStreamDevice(u->stream); return noErr; }
OSStatus AudioUnitGetProperty(AudioUnit u, AudioUnitPropertyID id, AudioUnitScope s, AudioUnitElement e, void *data, UInt32 *size) {
    (void)s; (void)e;
    if (u && id == kAudioUnitProperty_StreamFormat && u->has_fmt && *size >= sizeof u->fmt) { memcpy(data, &u->fmt, sizeof u->fmt); *size = sizeof u->fmt; return noErr; }
    return DM_AT_FAIL;   // the device properties (CurrentDevice, buffer frame size) are CoreAudio's: hle_audio.c's tuning of them is skipped
}
OSStatus AudioUnitSetProperty(AudioUnit u, AudioUnitPropertyID id, AudioUnitScope s, AudioUnitElement e, const void *data, UInt32 size) {
    (void)s; (void)e;
    if (!u) return DM_AT_FAIL;
    if (id == kAudioUnitProperty_StreamFormat && size >= sizeof u->fmt) { memcpy(&u->fmt, data, sizeof u->fmt); u->has_fmt = 1; return noErr; }
    if (id == kAudioUnitProperty_SetRenderCallback && size >= sizeof u->cb) { memcpy(&u->cb, data, sizeof u->cb); return noErr; }
    return DM_AT_FAIL;
}
OSStatus AudioUnitAddPropertyListener(AudioUnit u, AudioUnitPropertyID id, AudioUnitPropertyListenerProc p, void *ctx) { (void)u; (void)id; (void)p; (void)ctx; return noErr; }   // SDL follows device changes itself
OSStatus AudioUnitRemovePropertyListenerWithUserData(AudioUnit u, AudioUnitPropertyID id, AudioUnitPropertyListenerProc p, void *ctx) { (void)u; (void)id; (void)p; (void)ctx; return noErr; }
