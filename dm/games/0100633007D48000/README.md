# Hollow Knight (`0100633007D48000`)

Base game 1.3.2.9 (v0), SDK 3.5.1. Unity 5.6.3p2 (IL2CPP) on NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. Brought up on Linux (Asahi) only: it plays from the title screen through the intro movies into King's Pass at 60 fps; the Mac and iOS are untested.

## What it needs

Nothing of its own: the profile is the guessed one (`dm/games/profile_guess.py`), its 4 GB layout and its heap of the pool − 64 MB unchanged. What it needed was general:

| Change | Where | Why |
|---|---|---|
| The Unity 5.6 `Shader` layout | `dm/gfx/tools/unityfs.py` `SHADER_5_6` | see [Files and shaders](#files-and-shaders) |
| The scissor of a lower-left-origin game | `dm/host/linux/vk_draw.c` | see [Rendering](#rendering) |
| SDK 3's `movie::Decoder` constructor and output formats | `dm/host/hle_movie.c` | see [Movies](#movies) |
| The `AudioRenderSystem` offset in SDK 3.5.1 | `dm/host/hle_audren.c` | see [Movies](#movies) |
| H.264 and AAC on Linux | `dm/host/linux/movie_avc_ffmpeg.c`, `audioconverter_ffmpeg.c` | see [Movies](#movies) |

## Files and shaders

- RomFS: 1,410 files, 3.52 GB; 847 Unity serialized files (`Data/*.assets`, `level*`, `Data/Resources`), none with type trees, and no bundles.
- **Resolved: `[RED] Data/Resources/unity default resources: Unity serialized file without type trees, and no bundle has a Shader type tree of the same hash to borrow`**, which stopped the import. All 152 `Shader` objects have hash `a70f7abc…`, a layout the split did not have; it is written out now from Unity 5.6.3f1's tree ([Unity Shader layouts written out for players without bundles](../../../docs/reference/shaders.md#unity-shader-layouts-written-out-for-players-without-bundles)). 1,430 subprograms, 1,005 shaders after deduplication, all translated to SPIR-V.

## Rendering

- **Resolved: the title screen drawn into rows 360–720 only**, the logo cut off and the menu missing. The game keeps NVN's lower-left origin and draws its 1280×720 picture (`WindowCrop 0,0 1280x720`) into a 1920×1080 window texture under a scissor of that size, which the Linux replay flipped while it did not flip the viewport ([Linux: the scissor of a lower-left-origin game](../../../docs/reference/rendering.md#linux-the-scissor-of-a-lower-left-origin-game)).

## Movies

- 14 MP4 movies in `Data/StreamingAssets` (H.264 High profile 1280×720 and AAC-LC stereo), played by Unity through `movie::Extractor` (native) and `movie::Decoder` (the host's).
- **Resolved: a crash in `android::RefBase::RefBase` under `ACodec::UninitializedState::onAllocateComponent` when a new game starts its intro movie**: SDK 3's two-argument `Decoder` constructor was not taken over. With it taken over, the movie came out as vertical stripes until SDK 3's output format numbers were mapped ([SDK 3's `movie::Decoder` constructor and its output format numbers](../../../docs/reference/movies.md#sdk-3s-moviedecoder-constructor-and-its-output-format-numbers)).
- **Resolved: frozen for good when the intro movie stops** (skipped or at its end): the main thread waited on the AudioRenderer's event, which no render had signalled, since the renderer's manual renders read the wrong object ([The `AudioRenderSystem` inside `AudioRendererImpl` (SDK 3.5.1)](../../../docs/reference/audio.md#the-audiorendersystem-inside-audiorendererimpl-sdk-351)).
- On Linux, H.264 and AAC are decoded with FFmpeg now ([Linux: H.264 and AAC through FFmpeg](../../../docs/reference/movies.md#linux-h264-and-aac-through-ffmpeg)): the intro and prologue play, about 2 ms a picture.

## Driving it

- Headless with `DM_KEYS`, from an empty save: `f5000:A` (Start Game on the title menu), then A every 4 s through the save slot and the mode choice; the prologue movie follows, any button skips it. `LRight~8` then walks into King's Pass.

## Linux testing

- 2026-10-28, headless: title screen, a new game, the prologue and intro movies (played through and skipped), and King's Pass with the HUD, at 60 fps (frame time p50 16.7 ms); no crash or red line. Not played further.

## Found on this game

- [Unity Shader layouts written out for players without bundles](../../../docs/reference/shaders.md#unity-shader-layouts-written-out-for-players-without-bundles) (Unity 5.6).
- [Linux: the scissor of a lower-left-origin game](../../../docs/reference/rendering.md#linux-the-scissor-of-a-lower-left-origin-game).
- [SDK 3's `movie::Decoder` constructor and its output format numbers](../../../docs/reference/movies.md#sdk-3s-moviedecoder-constructor-and-its-output-format-numbers).
- [The `AudioRenderSystem` inside `AudioRendererImpl` (SDK 3.5.1)](../../../docs/reference/audio.md#the-audiorendersystem-inside-audiorendererimpl-sdk-351).
- [Linux: H.264 and AAC through FFmpeg](../../../docs/reference/movies.md#linux-h264-and-aac-through-ffmpeg).

## Open

- Play beyond King's Pass; the Mac and iOS (the movie and audio renderer fixes apply there too).
