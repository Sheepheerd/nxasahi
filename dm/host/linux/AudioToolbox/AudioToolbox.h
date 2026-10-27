// Linux build: the AudioToolbox subset hle_audio.c (AudioUnit output) and hle_movie.c (AAC through AudioConverter) use. The output AudioUnit is SDL3 audio
// (linux/audiounit_sdl.c: the render callback pulled by an SDL audio stream callback); AudioConverter fails, so AAC movie audio is silent (open: AAC through FFmpeg)
#pragma once
#include <darwin_compat.h>
typedef int32_t OSStatus;
typedef uint32_t UInt32, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, AudioUnitRenderActionFlags, AudioDeviceID, AudioFormatID, AudioFormatFlags;
typedef uint16_t UInt16;
typedef int32_t SInt32;
typedef double Float64;
typedef float Float32;
typedef unsigned char Boolean;
#define noErr 0
typedef struct { UInt32 mNumberChannels, mDataByteSize; void *mData; } AudioBuffer;
typedef struct { UInt32 mNumberBuffers; AudioBuffer mBuffers[1]; } AudioBufferList;
typedef struct { Float64 mSampleRate; AudioFormatID mFormatID; AudioFormatFlags mFormatFlags; UInt32 mBytesPerPacket, mFramesPerPacket, mBytesPerFrame, mChannelsPerFrame, mBitsPerChannel, mReserved; } AudioStreamBasicDescription;
typedef struct { int64_t mStartOffset; UInt32 mVariableFramesInPacket, mDataByteSize; } AudioStreamPacketDescription;
typedef struct { Float64 mSampleTime; uint64_t mHostTime; Float64 mRateScalar; uint64_t mWordClockTime; char mSMPTETime[24]; UInt32 mFlags, mReserved; } AudioTimeStamp;
typedef struct { Float64 mMinimum, mMaximum; } AudioValueRange;
typedef struct { uint32_t componentType, componentSubType, componentManufacturer, componentFlags, componentFlagsMask; } AudioComponentDescription;
typedef struct dm_audio_component *AudioComponent;
typedef struct dm_audio_unit *AudioUnit, *AudioComponentInstance;
typedef struct dm_audio_converter *AudioConverterRef;
typedef OSStatus (*AURenderCallback)(void *ctx, AudioUnitRenderActionFlags *f, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *bl);
typedef struct { AURenderCallback inputProc; void *inputProcRefCon; } AURenderCallbackStruct;
typedef void (*AudioUnitPropertyListenerProc)(void *ctx, AudioUnit au, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el);
typedef OSStatus (*AudioConverterComplexInputDataProc)(AudioConverterRef c, UInt32 *np, AudioBufferList *bl, AudioStreamPacketDescription **pd, void *ud);
#define DM_FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
enum {
    kAudioFormatLinearPCM = DM_FOURCC('l','p','c','m'), kAudioFormatMPEG4AAC = DM_FOURCC('a','a','c',' '),
    kLinearPCMFormatFlagIsFloat = 1, kLinearPCMFormatFlagIsSignedInteger = 4, kLinearPCMFormatFlagIsPacked = 8,
    kAudioUnitType_Output = DM_FOURCC('a','u','o','u'), kAudioUnitSubType_DefaultOutput = DM_FOURCC('d','e','f',' '), kAudioUnitSubType_RemoteIO = DM_FOURCC('r','i','o','c'),
    kAudioUnitManufacturer_Apple = DM_FOURCC('a','p','p','l'),
    kAudioUnitScope_Global = 0, kAudioUnitScope_Input = 1, kAudioUnitScope_Output = 2,
    kAudioUnitProperty_StreamFormat = 8, kAudioUnitProperty_SetRenderCallback = 23, kAudioOutputUnitProperty_CurrentDevice = 2000,
    kAudioDevicePropertyBufferFrameSize = DM_FOURCC('f','s','i','z'), kAudioDevicePropertyBufferFrameSizeRange = DM_FOURCC('f','s','z','#'),
    kAudioObjectUnknown = 0, kAudioConverterDecompressionMagicCookie = DM_FOURCC('d','m','g','c'),
};
#define DM_AT_FAIL ((OSStatus)-1)
AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d);
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out);
OSStatus AudioComponentInstanceDispose(AudioComponentInstance u);
OSStatus AudioUnitInitialize(AudioUnit u);
OSStatus AudioUnitUninitialize(AudioUnit u);
OSStatus AudioOutputUnitStart(AudioUnit u);
OSStatus AudioOutputUnitStop(AudioUnit u);
OSStatus AudioUnitGetProperty(AudioUnit u, AudioUnitPropertyID id, AudioUnitScope s, AudioUnitElement e, void *data, UInt32 *size);
OSStatus AudioUnitSetProperty(AudioUnit u, AudioUnitPropertyID id, AudioUnitScope s, AudioUnitElement e, const void *data, UInt32 size);
OSStatus AudioUnitAddPropertyListener(AudioUnit u, AudioUnitPropertyID id, AudioUnitPropertyListenerProc p, void *ctx);
OSStatus AudioUnitRemovePropertyListenerWithUserData(AudioUnit u, AudioUnitPropertyID id, AudioUnitPropertyListenerProc p, void *ctx);
static inline OSStatus AudioConverterNew(const AudioStreamBasicDescription *in, const AudioStreamBasicDescription *out, AudioConverterRef *c) { (void)in; (void)out; *c = NULL; return DM_AT_FAIL; }
static inline OSStatus AudioConverterDispose(AudioConverterRef c) { (void)c; return noErr; }
static inline OSStatus AudioConverterSetProperty(AudioConverterRef c, UInt32 id, UInt32 size, const void *data) { (void)c; (void)id; (void)size; (void)data; return DM_AT_FAIL; }
static inline OSStatus AudioConverterFillComplexBuffer(AudioConverterRef c, AudioConverterComplexInputDataProc in, void *ud, UInt32 *frames, AudioBufferList *out, AudioStreamPacketDescription *pd) { (void)c; (void)in; (void)ud; (void)out; (void)pd; *frames = 0; return DM_AT_FAIL; }
