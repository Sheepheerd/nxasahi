{
  description = "nxasahi: nxnative ported to Linux on Apple silicon (Asahi)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
    in {
      devShells = forAll (pkgs: {
        default = pkgs.mkShell.override { stdenv = pkgs.clangStdenv; } {
          packages = with pkgs; [
            # host build
            gnumake lld pkg-config libblocksruntime
            # rewriter, extraction and shader tools (README "Pack an IPA")
            (python3.withPackages (p: with p; [ lz4 capstone fonttools ]))
            dotnet-sdk_10 zstd spirv-cross glslang spirv-tools
            # Linux front end: window, input, audio; graphics through Vulkan (Honeykrisp)
            sdl3 vulkan-loader vulkan-headers vulkan-tools vulkan-validation-layers
            # debugging
            gdb
          ];
          # The .NET apphost (nx_extract) looks for the runtime here
          DOTNET_ROOT = "${pkgs.dotnet-sdk_10}/share/dotnet";
          LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath [ pkgs.vulkan-loader pkgs.sdl3 pkgs.libblocksruntime ];
        };
      });
    };
}
