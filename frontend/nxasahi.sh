#!/usr/bin/env bash
# nxasahi's game library, in the repository's dev shell (Python with PyGObject, GTK 4, libadwaita and the build tools)
icd=/run/opengl-driver/share/vulkan/icd.d/asahi_icd.aarch64.json; [ -f "$icd" ] && export VK_DRIVER_FILES="$icd"   # NixOS: the Vulkan loader on the Asahi driver only (GTK renders with Vulkan too)
cd "$(dirname "$(readlink -f "$0")")/.." && exec nix develop --command python3 frontend/nxasahi.py "$@"
