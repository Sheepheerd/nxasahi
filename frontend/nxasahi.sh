#!/usr/bin/env bash
# nxasahi's game library, in the repository's dev shell (Python with PySide6, Qt 6 and the build tools)
icd=/run/opengl-driver/share/vulkan/icd.d/asahi_icd.aarch64.json; [ -f "$icd" ] && export VK_DRIVER_FILES="$icd"   # NixOS: the Vulkan loader on the Asahi driver only
cd "$(dirname "$(readlink -f "$0")")/.." && exec nix develop --command python3 frontend/nxasahi.py "$@"
