#!/usr/bin/env bash
# Build the missing Fedora dependency into a user prefix; no Nix, sudo or changes to Mesa.
set -euo pipefail
prefix=${1:-$HOME/.local}
if [[ $# -gt 1 || $prefix != /* ]]; then
    echo "Usage: $0 [absolute-install-prefix] (default: \$HOME/.local)" >&2
    exit 1
fi
for tool in git cmake ninja clang clang++ install; do
    if ! command -v "$tool" >/dev/null; then
        echo "Missing $tool; see README.md for the Fedora build dependencies." >&2
        exit 1
    fi
done

# A released Vulkan SDK version, with the commit checked even if the tag is moved.
tag=vulkan-sdk-1.4.341.0
revision=a0fba56c34a6700f1724bf9b751da5b488a3775c
work=$(mktemp -d "${TMPDIR:-/tmp}/nxasahi-spirv-cross.XXXXXX")
trap 'rm -rf "$work"' EXIT
git clone --depth 1 --branch "$tag" https://github.com/KhronosGroup/SPIRV-Cross.git "$work/src"
if [[ $(git -C "$work/src" rev-parse HEAD) != "$revision" ]]; then
    echo "SPIRV-Cross tag $tag no longer matches the pinned commit; refusing to build." >&2
    exit 1
fi
cmake -S "$work/src" -B "$work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DSPIRV_CROSS_CLI=ON -DSPIRV_CROSS_ENABLE_TESTS=OFF
cmake --build "$work/build" --target spirv-cross --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
install -Dm755 "$work/build/spirv-cross" "$prefix/bin/spirv-cross"
printf 'Installed %s/bin/spirv-cross (%s).\n' "$prefix" "$tag"
