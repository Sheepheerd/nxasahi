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
  - [ ] Step 2: shaders as SPIR-V (the chain's output before SPIRV-Cross) loaded as Vulkan pipelines
  - [ ] Step 3: draws, textures, samplers and memory pools on Vulkan
  - [ ] Step 4: performance, the pipeline cache, memory aliasing
- [ ] Milestone 3: SDL3 front end: window, keyboard and controllers (`front.h`, `dm_input.h`), audio behind the AudioUnit subset `hle_audio.c` uses
- [ ] Packing script: a `--platform linux` that skips Xcode and the IPA

## Known gaps (open)

- **Graphics:** headless only. Fences complete at once and frames are dropped.
- **Sparse NVN memory pools** use `vm_remap` to alias memory, which Linux can only do for file-backed memory. Mappings stay in the page table only (`hle_nvn.c` `vpool_map` logs it). Fix with memfd-backed pools.
- **Audio output and AAC movie audio:** the AudioToolbox stand-in fails, so there is no sound (the same as the Mac host without `DM_AUDIO=1`). **H.264 movies:** no decoder (`linux/movie_avc_none.c`). VP9/Vorbis movies use the bundled libvpx/stb_vorbis.
- **Diagnostics:** the ones that read other threads' state (watchdog backtraces, hitch dumps, Mach CPU/energy counters) report nothing.
- **Saves:** `clonefile` is a recursive copy (reflinked where the file system supports it), so commits cost time proportional to the save's size.
- **Signals:** the SIGTERM shutdown source blocks SIGTERM only in threads created after it starts.
