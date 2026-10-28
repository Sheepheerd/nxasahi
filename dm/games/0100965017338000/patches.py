# Super Mario Party Jamboree (0100965017338000): the rewriter's patches of this game's own builds (CUTS as described in dm/tools/patch_nso.py, "Registered patches")
CUTS = {
  # 1.0.0 (v0) main: with virtual address memory the Bezel engine's CoreSystem (main+0x981ccc) initializes a StandardAllocator over 56 GB of address space (main+0x981cf0, the size
  # at +0x20 of the configuration it passes to main+0x9819fc → StandardAllocator::Initialize(nullptr, size)), which the Switch's 64 GB alias region holds. The Linux GPU's address
  # space is about 64 GB in all (one sparse buffer of 63 GB at most, and every allocation backing it takes its own range), so the allocator gets 4 GB of address space, as
  # Pokémon Legends: Z-A's and Paper Mario: The Thousand-Year Door's do
  '0451231f26f181157cec81b4385b5ed1': [
    (0x981cf0, 'c801c0d2', '2800c0d2', 'CoreSystem StandardAllocator address space mov x8,#0xe00000000 → mov x8,#0x100000000 (56 GB → 4 GB)'),
  ],
}
