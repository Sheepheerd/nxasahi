#!/usr/bin/env bash
# Native by default; --nix keeps the repository's Nix dev shell available.
set -euo pipefail
repo=$(dirname "$(dirname "$(readlink -f "$0")")")
cd "$repo"

# NixOS only: Fedora's Vulkan loader already finds its system Mesa driver.
icd=/run/opengl-driver/share/vulkan/icd.d/asahi_icd.aarch64.json
if [[ -z ${VK_DRIVER_FILES:-} && -f $icd ]]; then export VK_DRIVER_FILES="$icd"; fi
if [[ ${1:-} == --nix ]]; then
    shift
    exec nix develop --command python3 frontend/nxasahi.py "$@"
fi

export PATH="$HOME/.local/bin:$PATH"
if [[ -n ${NXASAHI_PYTHON:-} ]]; then
    python=$NXASAHI_PYTHON
elif [[ -n ${VIRTUAL_ENV:-} ]]; then
    python=$VIRTUAL_ENV/bin/python3
elif [[ -x $repo/.venv/bin/python3 ]]; then
    python=$repo/.venv/bin/python3
else
    python=python3
fi
if ! python=$(command -v "$python"); then
    echo 'nxasahi: Python not found; see README.md for native setup (or use --nix).' >&2
    exit 1
fi
# Make and the import's subprocesses must use the same Python, including its nsz module.
export PATH="$(dirname "$python"):$PATH"
export NXASAHI_LAUNCHER="${NXASAHI_LAUNCHER:-$repo/frontend/nxasahi.sh}"
exec "$python" frontend/nxasahi.py "$@"
