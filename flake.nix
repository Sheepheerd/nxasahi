{
  description = "nxasahi: nxnative ported to Linux on Apple silicon (Asahi)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [ "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});

      # Python with what the import's tools and the library front end import
      python =
        pkgs:
        pkgs.python3.withPackages (
          p: with p; [
            lz4
            capstone
            fonttools
            pyside6 # the game library (frontend/nxasahi.py: Qt 6 / QML)
            nsz # NSZ / XCZ games, decompressed before extraction (build_ipa_from_nsp.py decompress_nsz)
          ]
        );

      # What an import runs (scripts/build_ipa_from_nsp.py --platform linux): the host is compiled per game, since its profile is compiled in
      buildTools =
        pkgs: with pkgs; [
          clang
          lld
          llvmPackages.bintools # ar for libvpx
          gnumake
          pkg-config
          coreutils
          findutils
          gnused
          gnugrep
          zstd
          spirv-cross
          spirv-tools
          glslang
        ];

      # The library's Qt: the QML runtime, its controls and dialogs, and the Wayland platform plugin
      qtLibs = pkgs: with pkgs.qt6; [ qtbase qtdeclarative qtwayland qtsvg ];
      qtEnv = pkgs: {
        QT_PLUGIN_PATH = pkgs.lib.makeSearchPath "lib/qt-6/plugins" (qtLibs pkgs);
        QML_IMPORT_PATH = pkgs.lib.makeSearchPath "lib/qt-6/qml" (qtLibs pkgs);
      };

      # The shared fonts' stand-ins: the Mac tools read macOS fonts
      fontDir = pkgs: "${pkgs.dejavu_fonts}/share/fonts/truetype";

      # nx_extract (NSP / XCI extraction through LibHac), built once here instead of at the first import
      nxExtract =
        pkgs:
        pkgs.buildDotnetModule {
          pname = "nx_extract";
          version = "0.1";
          src = ./dm/tools/nx_extract;
          projectFile = "nx_extract.csproj";
          nugetDeps = ./dm/tools/nx_extract/deps.json;
          dotnet-sdk = pkgs.dotnetCorePackages.sdk_10_0;
          dotnet-runtime = pkgs.dotnetCorePackages.runtime_10_0;
          executables = [ "nx_extract" ];
        };
    in
    {
      packages = forAll (
        pkgs:
        let
          inherit (pkgs) lib;
          py = python pkgs;
          nx = nxExtract pkgs;
          runtimeLibs = [
            pkgs.vulkan-loader
            pkgs.sdl3
            pkgs.libblocksruntime
            pkgs.ffmpeg-headless # H.264 and AAC movies (dm/host/linux/movie_avc_ffmpeg.c, audioconverter_ffmpeg.c)
            pkgs.libglvnd # EGL for OpenGL games (dm/host/linux/gl_egl.c), dispatching to the system's Mesa
          ];
        in
        {
          nx_extract = nx;
          default = pkgs.stdenvNoCC.mkDerivation {
            pname = "nxasahi";
            version = "0.1";
            src = self;

            nativeBuildInputs = [
              pkgs.makeShellWrapper # --run lines: NXASAHI_STATE and VK_DRIVER_FILES are set at start
              pkgs.qt6.wrapQtAppsHook # Qt's plugin and QML paths (qtWrapperArgs) for the library
              pkgs.copyDesktopItems
            ];
            buildInputs = qtLibs pkgs;
            dontWrapQtApps = true; # its arguments go into the one wrapper below
            dontBuild = true;

            desktopItems = [
              (pkgs.makeDesktopItem {
                name = "org.nxasahi.Library";
                desktopName = "nxasahi";
                comment = "Switch game library";
                exec = "nxasahi %F";
                icon = "input-gaming";
                categories = [ "Game" ];
                mimeTypes = [
                  "application/x-nx-nsp"
                  "application/x-nx-xci"
                ];
              })
            ];

            installPhase = ''
              runHook preInstall
              mkdir -p $out/share/nxasahi $out/bin
              cp -r . $out/share/nxasahi
              rm -rf $out/share/nxasahi/.envrc $out/share/nxasahi/.direnv
              runHook postInstall
            '';

            # The library, with everything an import and a game need. The source is read-only here: an import builds each game's host
            # under NXASAHI_STATE (build_ipa_from_nsp.py linux_state), and nx_extract comes built
            preFixup = ''
              makeShellWrapper ${py}/bin/python3 $out/bin/nxasahi \
                "''${qtWrapperArgs[@]}" \
                --add-flags $out/share/nxasahi/frontend/nxasahi.py \
                --prefix PATH : ${lib.makeBinPath ([ py pkgs.dotnetCorePackages.runtime_10_0 ] ++ buildTools pkgs)} \
                --prefix PKG_CONFIG_PATH : ${lib.makeSearchPathOutput "dev" "lib/pkgconfig" [ pkgs.sdl3 pkgs.vulkan-loader pkgs.ffmpeg-headless pkgs.libglvnd ]} \
                --prefix CPATH : ${lib.makeSearchPathOutput "dev" "include" [ pkgs.libblocksruntime pkgs.vulkan-headers ]} \
                --prefix LIBRARY_PATH : ${lib.makeLibraryPath runtimeLibs} \
                --prefix LD_LIBRARY_PATH : ${lib.makeLibraryPath runtimeLibs} \
                --set DOTNET_ROOT ${pkgs.dotnetCorePackages.runtime_10_0}/share/dotnet \
                --set NXASAHI_FONT_DIR ${fontDir pkgs} \
                --set NXASAHI_NX_EXTRACT ${nx}/bin/nx_extract \
                --set NXASAHI_LAUNCHER $out/bin/nxasahi \
                --set PYTHONDONTWRITEBYTECODE 1 \
                --run 'export NXASAHI_STATE="''${NXASAHI_STATE:-''${XDG_DATA_HOME:-$HOME/.local/share}/nxasahi/build}"' \
                --run 'icd=/run/opengl-driver/share/vulkan/icd.d/asahi_icd.aarch64.json; [ -f "$icd" ] && export VK_DRIVER_FILES="$icd"'
            '';

            meta = {
              description = "Switch games running natively on Apple silicon under Asahi Linux";
              homepage = "https://github.com/Sheepheerd/nxasahi";
              platforms = systems;
              mainProgram = "nxasahi";
            };
          };
        }
      );

      apps = forAll (pkgs: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.system}.default}/bin/nxasahi";
        };
      });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell.override { stdenv = pkgs.clangStdenv; } {
          packages = with pkgs; [
            # host build
            gnumake
            lld
            pkg-config
            libblocksruntime
            (python pkgs)
            dotnet-sdk_10
            zstd
            spirv-cross
            glslang
            spirv-tools
            # Linux front end: window, input, audio; graphics through Vulkan (Honeykrisp)
            sdl3
            ffmpeg-headless # H.264 and AAC movies
            libglvnd # EGL for OpenGL games
            vulkan-loader
            vulkan-headers
            vulkan-tools
            vulkan-validation-layers
            # debugging
            gdb
          ]
          # game library front end (frontend/nxasahi.py): Qt 6 / QML through PySide6
          ++ qtLibs pkgs;
          inherit (qtEnv pkgs) QT_PLUGIN_PATH QML_IMPORT_PATH;
          # The .NET apphost (nx_extract) looks for the runtime here
          DOTNET_ROOT = "${pkgs.dotnet-sdk_10}/share/dotnet";
          NXASAHI_FONT_DIR = fontDir pkgs;
          LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath [
            pkgs.vulkan-loader
            pkgs.sdl3
            pkgs.libblocksruntime
            pkgs.ffmpeg-headless
            pkgs.libglvnd
          ];
        };
      });
    };
}
