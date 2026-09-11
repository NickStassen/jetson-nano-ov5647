#!/bin/bash
# Download and unpack the L4T R32.5.1 public sources (only the device tree
# sources are needed for this project; the kernel module builds against the
# nvidia-l4t-kernel-headers package already on the Nano).
#
# Usage: scripts/fetch-l4t-sources.sh [dest dir]   (default: build/src)
set -euo pipefail

DEST=${1:-"$(cd "$(dirname "$0")/.." && pwd)/build/src"}
URL="https://developer.nvidia.com/embedded/L4T/r32_Release_v5.1/r32_Release_v5.1/Sources/T210/public_sources.tbz2"

mkdir -p "$DEST"
cd "$DEST"

if [ ! -f public_sources.tbz2 ]; then
	echo ">> downloading public_sources.tbz2 (~160 MB)"
	curl -fL -o public_sources.tbz2 "$URL"
fi

if [ ! -d hardware/nvidia ]; then
	echo ">> extracting kernel_src.tbz2 (hardware/ and kernel/ trees)"
	tar xf public_sources.tbz2 Linux_for_Tegra/source/public/kernel_src.tbz2
	tar xf Linux_for_Tegra/source/public/kernel_src.tbz2
fi

echo ">> L4T sources ready in $DEST"
ls hardware/nvidia/platform/t210/porg/kernel-dts | head -3
