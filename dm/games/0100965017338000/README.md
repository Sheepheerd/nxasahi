# Super Mario Party Jamboree (`0100965017338000`)

Base game 1.0.0 (v0), SDK 17.5.4. NintendoWare Bezel Engine on NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`, and 139 NROs the game loads at run time. Brought up on Linux (Asahi) only: it boots to its title screen at 60 fps; the Mac and iOS are untested.

## What it needs

| Setting | Where | Why |
|---|---|---|
| StandardAllocator address space 56 GB → 4 GB | [`patches.py`](patches.py), `main+0x981cf0` | see [Memory](#memory) |
| SDK node area 1 GB → 62 MB | `dm/tools/sdk_patches.py`, build `1ea0b9fe…`, `sdk+0x139468` / `+0x1394fc` | `StandardAllocator::Initialize` aborts in `nninitStartup` without it |
| 9 GB block: `mem_reserve 0x240000000`, `mem_alias 0x218000000`, `mem_stack 0x10000000` | [`game.tsv`](game.tsv) | the engine maps up to +3 GB of other regions before its allocator's 4 GB |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM's 16 MB system resource selects virtual address memory |

## Memory

- **Resolved: `FATAL …/nro.tsv: more NROs than module slots (MAX_MOD 128)`** before the game ran: 139 NROs and three modules. `MAX_MOD` is 256 now ([More NROs than module slots](../../../docs/reference/services.md#more-nros-than-module-slots)).
- **Resolved: `nn::diag` abort at `sdk+0x12b1a0`, `StandardAllocator::Initialize`**, called from `main+0x981e38`. Two causes, both needed: the SDK's 1 GB node area (the cut Ace Attorney Investigations Collection's 17.5.3 build takes, at the same offsets), and the engine's CoreSystem (`main+0x981ccc`) initializing its allocator over 56 GB of address space (`mov x8, #0xe00000000` at `main+0x981cf0`, the size at +0x20 of the configuration it passes to `main+0x9819fc`).
- A 64 GB alias region, as the Switch has, cannot be given on Linux: the Asahi driver's GPU address space is about 64 GB in all, and the memory behind a 62 GB alias region ran out of addresses after 1–2 GB. The allocator gets 4 GB, as Pokémon Legends: Z-A's does ([StandardAllocators over 12 to 62 GB of address space](../../../docs/reference/memory.md#standardallocators-over-12-to-62-gb-of-address-space)).
- At the title: alias region mapped up to +7.06 GB, 2.00 GB of the 3.21 GB pool mapped.

## Files and shaders

- RomFS: 949 Bezel Engine archives of version 0x60000 (127,203 entries, 10.5 GB unpacked); the split reads that version since this game ([BNSH in Bezel Engine archives](../../../docs/reference/shaders.md#bnsh-in-bezel-engine-archives)). 25,693 shaders (25,691 translated to SPIR-V).

## Linux testing

- 2026-10-28, headless: boots through the logos to the title screen and its attract text at 60 fps (frame time p50 16.7 ms), no crash. Not played beyond the title.

## Found on this game

- [More NROs than module slots](../../../docs/reference/services.md#more-nros-than-module-slots).
- The SDK 17.5.4 build's instance of [the SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area).
- [StandardAllocators over 12 to 62 GB of address space](../../../docs/reference/memory.md#standardallocators-over-12-to-62-gb-of-address-space), and why Linux cannot give it a 64 GB alias region.
- Bezel Engine archive version 0x60000 ([BNSH in Bezel Engine archives](../../../docs/reference/shaders.md#bnsh-in-bezel-engine-archives)).
- NVN's queue memory counters (`nvnQueueGetTotal*MemoryUsed`, `nvnQueueResetMemoryUsageCounts`), read every frame: implemented as 0, the host's queues hold no memory of their own.

## Open

- The opening movie is H.264, which the Linux host does not decode yet (`[RED] movie: no H.264 decoder in the Linux host yet`).
- `SetRenderEnableConditional` is drawn unconditionally.
- Play beyond the title screen; the Mac and iOS.
