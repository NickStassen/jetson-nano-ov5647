#!/bin/bash
# Copy the driver, device tree sources, scripts and the minimal set of L4T
# device tree include trees to the Nano so everything can be built there.
#
# Usage: scripts/sync-to-jetson.sh [user@host]   (default: jetson1@192.168.1.128)
#        L4T_SRC=... to point at an existing extracted public_sources tree
#        (default: build/src, see fetch-l4t-sources.sh). Remote dir: ~/ov5647-build
set -euo pipefail

TARGET=${1:-jetson1@192.168.1.128}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${L4T_SRC:-$ROOT/build/src}
REMOTE=ov5647-build

[ -d "$SRC/hardware/nvidia" ] || { echo "L4T sources missing at $SRC; run scripts/fetch-l4t-sources.sh" >&2; exit 1; }

echo ">> syncing repo files"
rsync -a --delete "$ROOT/driver/"  "$TARGET:$REMOTE/driver/"
rsync -a --delete "$ROOT/dts/"     "$TARGET:$REMOTE/dts/"
rsync -a          "$ROOT/scripts/" "$TARGET:$REMOTE/scripts/"

echo ">> syncing L4T device tree include trees (~5 MB)"
cd "$SRC"
rsync -a --relative \
	hardware/nvidia/platform/t210/porg \
	hardware/nvidia/platform/t210/common \
	hardware/nvidia/soc/t210 \
	hardware/nvidia/soc/tegra \
	hardware/nvidia/platform/tegra/common \
	kernel/kernel-4.9/include/dt-bindings \
	kernel/nvidia/include/dt-bindings \
	"$TARGET:$REMOTE/src/"

echo ">> done. On the Nano:"
echo "   cd ~/$REMOTE && make -C driver && \\"
echo "   scripts/build-dtb.sh dts/tegra210-p3448-0002-p3449-0000-b00-ov5647.dts out.dtb && \\"
echo "   sudo scripts/install-on-jetson.sh driver/ov5647.ko out.dtb && sudo reboot"
