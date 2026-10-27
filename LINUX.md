# nxasahi: nxnative on Linux (Asahi, Apple silicon)

A port of [nxnative](https://github.com/seedds/nxnative) to Linux on Apple silicon. The game code already runs natively on the M-series CPU and 16 KiB pages, so the port replaces the Apple platform layer around it: Darwin APIs, Mach-O details, the Cocoa front end and the Metal graphics backend.

Upstream is the `upstream` remote. The port keeps the shared sources almost unchanged, so merging upstream stays cheap; Linux-only code lives in `dm/host/linux/`, `dm/host/Makefile.linux` and `flake.nix`.

## Build

```sh
nix develop                                   # clang, lld, Python with lz4/capstone/fonttools, .NET 10, SPIRV-Cross, glslang, SDL3, Vulkan
cd dm/host
make -f Makefile.linux GAME=010015100B514000  # the host, for one game profile
make -f Makefile.linux linux-tests            # tests of the Linux layer
```

Modules must be rewritten for the Linux host: set `DM_THREAD_REG=tpidr_el0` when running `dm/tools/patch_nso.py` (the NSP script includes it in its rewrite cache key).

## Run a game headless

Keys in `~/.switch/prod.keys`; one game's NSPs in their own folder (here `~/Games/MarioWonder`), with the work folder `<id>-work` beside them. Inside `nix develop`, from the repository root:

```sh
W=~/Games/MarioWonder/010015100B514000-work
dotnet build dm/tools/nx_extract -c Release -o $W/tools/nx_extract
$W/tools/nx_extract/nx_extract ~/.switch/prod.keys ~/Games/MarioWonder/*.nsp - $W/data/v0      # - = no update NSP
for m in main sdk; do DM_GAME=010015100B514000 DM_THREAD_REG=tpidr_el0 python3 dm/tools/patch_nso.py $W/data/v0/ExeFS/$m $W/build/patched_v0 $m; done
# Fonts: the Mac tools read macOS fonts; DejaVu Sans stands in (English text; CJK needs another font)
D=$(nix build --no-link --print-out-paths nixpkgs#dejavu_fonts)/share/fonts/truetype
python3 dm/tools/make_ext_font.py $W/font/nintendo_ext.ttf $D/DejaVuSans-Bold.ttf && cp $D/DejaVuSans.ttf $W/font/standard.ttf
mkdir -p $W/run/save $W/run/temp
DM_ROMFS=$W/data/v0/RomFS DM_SAVE=$W/run/save DM_TEMP=$W/run/temp DM_FONT_DIR=$W/font DM_LANG=en-US \
  DM_NVN_SHMAP=dm/games/010015100B514000/shader_map.tsv dm/host/dmhost $W/build/patched_v0
```

Add `DM_WINDOW=1` for a window (Vulkan presentation; on NixOS `VK_DRIVER_FILES=/run/opengl-driver/share/vulkan/icd.d/asahi_icd.aarch64.json` keeps the loader to the Asahi driver).

## How the platform differences are handled

| Darwin | Linux | Where |
|---|---|---|
| Thread slots at `tpidrro_el0 + 8*key` (the pthread TSD) | EL0 cannot set `tpidrro_el0`; the rewriter emits `mrs tpidr_el0` and the key page holds each key's offset from glibc's thread pointer, found at startup | `nso_asm.py`, `loader.c` `tp_slot_offsets` |
| `x18` clobbered by the kernel | not clobbered; the rewrite still applies, unchanged | — |
| Mach, libkern, dispatch, CommonCrypto, QoS, `malloc_zone`, `clonefile`, `renamex_np`, xattr options | equivalents or clean failures, force-included; xattr names without a Linux namespace go in `user.` | `linux/darwin_compat.{h,c}`, header stand-ins in `linux/` |
| `uc->uc_mcontext->__ss` in signal handlers | `sigaction` installs a trampoline that hands handlers a Darwin-shaped context and writes registers back | `darwin_compat.c` `sig_trampoline` |
| `getsectiondata` registration tables | the same `section("__DATA,…")` attributes, bracketed by a linker script | `linux/sections.ld` |
| Mach-O assembly (`_sym`, `@PAGE`) | translated to ELF at build time; one `#ifdef` for the big-stack stub macro | `linux/darwin_asm.sed`, `hle.h` |

## Status

- [x] Dev shell; all host C sources build unchanged against the compat layer; the host links (headless)
- [x] Thread-slot scheme verified on the real kernel and glibc across threads (`t_tpslot`)
- [x] Darwin-shaped signal context verified: registers, SIMD, ESR/fault address, write-back of `pc` and registers (`t_sigctx`)
- [x] Rewriter tests pass with both `DM_THREAD_REG` settings
- [x] **Milestone 1: a game boots headless to its frame loop.** Super Mario Bros. Wonder 1.0.0 reaches `nnMain`, runs 3,300 frames in 60 s at 60 fps (p50 16.66 ms) and commits its saves; the only `[RED]` is upstream's conditional render. Steps: [Run a game headless](#run-a-game-headless)
- [ ] Milestone 2: Vulkan backend behind the `mtl_*` interface of `gfx/nvn/nvn_obj.h` (now `linux/headless.c`)
  - [x] Step 1: SDL3 window and Vulkan presentation (`linux/front_sdl.c`, `linux/vk_window.c`, `DM_WINDOW=1`): Wayland window, swapchain made again on resize, a present thread with FIFO; Mario Wonder presents at 60 fps on Honeykrisp (Apple M1). The picture is a placeholder (a bar that moves one step per game frame) until the draws are translated
  - [x] Step 2a: the game's shaders as SPIR-V on Linux (`dm/gfx/tools/dmsh_spv.py`: the unmodified Metal chain with the pinned translator through dotnet, stopped before SPIRV-Cross, so the chain version is unchanged). Mario Wonder 1.0.0: 12,811 of 12,811 translated, all pass `spirv-val`, and all compile on Honeykrisp one stage at a time (`VK_EXT_shader_object`, `make -f Makefile.linux t_spv_compile SPV=…`), clean under the validation layer but for the two gaps below. At load, `linux/spv_reflect.c` reflects each module (descriptor layouts, push constants) and fits it to the device (`spvr_fit`)
  - [ ] Step 2b: the shaders as pipelines in the host (with step 3)
  - [x] Step 3.1: guest memory the GPU sees. One Vulkan device made before the game runs (`linux/vk_dev.c`, headless too; `DM_VK=0` runs without). The heap and the alias region are backed by device memory in 32 MiB chunks mapped in place at their own addresses, each region one sparse buffer (`linux/vk_mem.c`, hooked by `dm_gpu_region` / `dm_gpu_commit` in `hle_svc.c`; the design is checked on the device by `make -f Makefile.linux t_placed`). Mario Wonder boots and runs at 60 fps with every chunk backed before its first page opens
  - [x] Step 3.2: the replay skeleton (`linux/vk_replay.c`): one command buffer per submission signalling a timeline semaphore (fences and `nvnSyncWait` wait for the GPU itself), textures as VkImages by serial, the record stream walked as `nvn_mtl.m` walks it, render targets and the game's colour and depth-stencil clears carried out on the GPU, the presented texture copied to the window. Screenshots: `DM_SHOT=<present>:<file.png>` or the `DM_CTL` control file's `shot <file.png>`. Mario Wonder: its own clears on the GPU, its 1920x1080 window texture shown, at 60 fps; draws are counted (`DM_VK_OPS=1`), not drawn yet
  - [x] Step 3.3: textures loaded from guest memory (`linux/vk_replay.c`): every target as a VkImage, deswizzled on the CPU (`gfx/tex/nvn_deswizzle.c`) into staging and copied by an upload command buffer that runs before the submission, reloaded when the game writes the storage (`nvn_dirty.c`), only the written rows of a linear texture; GPU-written textures are never reloaded; texture and sampler IDs registered. `DM_VK_TEXDUMP=<dir>[:n]` writes loaded textures as PNG. Mario Wonder's ASTC and BC textures checked by eye. Samplers as VkSamplers and the copy records come with step 3.4
  - [ ] Step 3.4: draws
    - [x] 3.4a: what each shader binding is, for Vulkan (`dmsh_spv.py`): the Metal chain's own `reflect()` lifted out of `dmsh_build.py`'s parsed source and run on a stand-in of its argument buffer struct made from the SPIR-V (`<key>.vk.json`); every resource then moved to a canonical binding in a set per stage, so shader objects of different stages share one layout (`<key>.vk.spv`, and `<key>.vkb` for the host). Mario Wonder: all 12,811 shaders' bindings classified (none without a slot), remapped without collisions, valid under `spirv-val`
    - [x] 3.4b: shaders and their resources in the host (`linux/vk_draw.c`, `DM_VK_SPV=<spv-dir>`): a program's stages as shader objects (`VK_EXT_shader_object`) from `<key>.vk.spv`; one set layout for every stage, the union of the bindings the game's shaders use; per draw a descriptor set per stage from the shader's `.vkb`: support buffer, c1, uniform and storage buffers (`vkm_lookup`), textures with views of the shader's dimension, samplers cached by their parameters, dummies for whatever is missing (`DM_VK_DRAWLOG=1` counts). Mario Wonder: 65,536 draws prepared in 30 s, 63 shader objects, nothing missing. Open: driver banks c0 / c2 read as zero, storage images, texel buffers, texture arrays beyond element 0, view formats and swizzles
    - [x] 3.4c: the draws: render passes over the bound targets (dynamic rendering, `vk_replay.c`), every piece of state dynamic (shader objects: no pipelines), vertex input (`vkCmdSetVertexInputEXT`, the 3-component and scaled vertex formats), direct, indexed (8-bit indices through `VK_KHR_index_type_uint8`) and indirect draws, inline uniform updates and buffer copies in GPU order, GL clip depth mapped to Vulkan's in the last pre-rasterization stage (`spvr_gl_depth`), the upper-left origin as a flipped viewport, texture swizzles (format, builder, view). **Mario Wonder's title screen renders correctly** (checked by eye against the game), at 60 fps. Debugging: `DM_VK_PASSLOG=<present>` (a frame's passes and draws), `DM_VK_RTDUMP=<present>:<dir>` (its render targets), `DM_SHOT_EVERY=<n>:<dir>`, `DM_VK_NOCULL=1`. Open: indirect-count multi-draws, quads, line loops, 3D render targets, logic ops, compute dispatches, texture copies, storage images, texel buffers, the driver banks c0 / c2, the 11 vertex shaders that store to buffers
    - [x] 3.4d: the levels: compute dispatches, texture copies and clears, 3D render targets, storage images, the driver banks c0 / c2. Images shared by storage (address and description, as `nvn_mtl.m` does: the game rebuilds its texture objects every frame, and one image per object lost what the GPU had rendered into the last one). Vertex and tessellation evaluation outputs set to (0, 0, 0, 1) before the shader runs (`spvr_init_outputs`): on Maxwell a component a shader never writes reads as 0, under Mesa it was garbage, and the NaN it turned into made lit models black. **Mario Wonder's levels and world map render correctly.** Debugging: `DM_VK_PASSSTOP=<n>` (the traced frame draws nothing after pass n), `DM_VK_SHOVERRIDE=<key>=<spv>[,…]`, `DM_VK_TEXLOG=1`, `DM_VK_ROBUST=1`, `DM_VK_NOFLUSH=1`
  - [ ] Step 4: performance, the pipeline cache, memory aliasing
    - [x] Mario Wonder at 60 fps in its first level and on the world map (was about 40): the process keeps to the large cores (`DM_CPUS=all` for every core: the scheduler had kept it on the M1's Icestorm cores); a submission goes to the GPU in parts of 128 draws between render passes so the GPU runs while the rest is replayed; the dirty check skipped while nothing was written since; descriptor sets reused within a submission when their contents repeat; GPU time per frame from timestamps (`DM_NVN_TSV=<file>` `@gpu_ms`). Per frame in the level: game 5 ms + replay 5 ms on the game's thread, GPU 11-14 ms
    - [x] No shader compiles in play: shader objects saved as driver binaries (`~/.cache/nxasahi/vkshaders/`, `DM_VK_SHCACHE=<dir>`, `0` off; keyed by shader set, descriptor layout and driver binary UUID), loaded in 0.04 ms; two low-priority threads precompile the whole set into it in the background (`DM_VK_PRECOMPILE=<threads>`, `0` off; Mario Wonder's 12,811 in about 64 s on the first run, 144 MB); vertex shaders compiled once, in the form drawn. Before: 5-15 ms per new shader, up to 1.3 s when a level's shaders came at once. The long-frame log now counts shader compiles and texture loads (`DM_VK_SLOWDRAW=1` logs draws over 1 ms)
    - [ ] Open: hitches up to about 60 ms when a burst of new textures loads (image creation and CPU deswizzle); a full barrier after every clear, copy and uniform update; every piece of state set before every draw
- [ ] Milestone 3: SDL3 front end
  - [x] Window, keyboard and controllers (`linux/front_sdl.c`), mapped as the Mac front end: W A S D left stick, J K U I → A B X Y, F G stick presses, Q E Z C → L R ZL ZR, Return / Tab → + / −, arrows d-pad, Space shake; SDL gamepads one player each, buttons by position. Mario Wonder goes from the title through user select and New Game to character select (3D models render) with scripted input (`DM_KEYS`)
  - [x] Audio: the AudioUnit subset `hle_audio.c` uses, on SDL3 audio (`linux/audiounit_sdl.c`; PipeWire, PulseAudio or ALSA through SDL): the render callback pulled by SDL's stream callback, 5 ms device buffers. On by default (`DM_AUDIO=0` for the silent sink); without a device the silent sink stands in and the watchdog retries. Mario Wonder: no underruns
- [ ] Packing script: a `--platform linux` that skips Xcode and the IPA

## Known gaps (open)

- **Shaders on the M1:** 11 of Mario Wonder's vertex shaders write a storage buffer, which Honeykrisp does not allow from the vertex stage (`vertexPipelineStoresAndAtomics`; Metal does). Every vertex shader declares 8 clip distances and 1 cull distance (the translator's `gl_PerVertex`), one over the M1's combined limit of 8; the cull distance is never written and the driver compiles them, but the validation layer reports it.
- **Shader tables per version:** the profile's shader tables cover 1.0.0 and 1.2.1 together. With only one version's NSP, make a derived profile with that version's tables (the split's output) and point the tools at it with `DM_GAMES_DIR` and `DM_GAMES_FIRST=1`.
- **Graphics:** open: texel buffers, texture arrays beyond element 0, view format changes, quads and line loops, logic ops, indirect-count multi-draws, discarded rasterization (drawn anyway).
- **Memory cost of the backing:** Honeykrisp pins device memory when it is allocated, so a chunk is resident from its first commit, where anonymous memory became resident page by page. Mario Wonder commits its 1.44 GB heap at boot: it uses about 1.95 GB instead of 0.5 GB (measured with `free`; device memory is shmem, not in the process's RSS). Chunks are not given back when a region shrinks.
- **Sparse NVN memory pools** use `vm_remap` to alias memory, which Linux can only do for file-backed memory. Mappings stay in the page table only (`hle_nvn.c` `vpool_map` logs it). Fix with memfd-backed pools.
- **AAC movie audio:** AudioConverter is not there, so AAC movie audio is silent (open: AAC through FFmpeg). **H.264 movies:** no decoder (`linux/movie_avc_none.c`). VP9/Vorbis movies use the bundled libvpx/stb_vorbis.
- **Diagnostics:** the ones that read other threads' state (watchdog backtraces, hitch dumps, Mach CPU/energy counters) report nothing.
- **Saves:** `clonefile` is a recursive copy (reflinked where the file system supports it), so commits cost time proportional to the save's size.
- **Signals:** the SIGTERM shutdown source blocks SIGTERM only in threads created after it starts.
