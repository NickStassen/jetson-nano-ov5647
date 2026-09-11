#!/bin/bash
# Compile a Jetson Nano device tree the same way the L4T kernel build does
# (cpp pass, then dtc with symbols), without needing the whole kernel tree.
#
# Usage: scripts/build-dtb.sh <input.dts> <output.dtb>
#
# Needs: cpp (any gcc), dtc >= 1.4.4, and the L4T "hardware/nvidia" device
# tree sources. Point L4T_SRC at a directory containing hardware/nvidia/...
# (default: ~/ov5647-build/src, which is where sync-to-jetson.sh puts them).
set -euo pipefail

DTS=${1:?input .dts}
OUT=${2:?output .dtb}
SRC=${L4T_SRC:-$HOME/ov5647-build/src}
HW=$SRC/hardware/nvidia

[ -d "$HW" ] || { echo "L4T dts sources not found at $HW (set L4T_SRC)" >&2; exit 1; }

INC=(
	"$HW/soc/tegra/kernel-include"
	"$HW/platform/tegra/common/kernel-dts"
	"$HW/soc/t210/kernel-dts"
	"$HW/platform/t210/common/kernel-dts"
	"$HW/platform/t210/porg/kernel-dts"
	"$HW/platform/t210/porg/kernel-dts/porg-platforms"
)
CPPI=(); DTCI=()
for d in "${INC[@]}"; do CPPI+=(-I"$d"); DTCI+=(-i "$d"); done

TMP=$(mktemp --suffix=.pre.dts)
trap 'rm -f "$TMP"' EXIT

cpp -nostdinc "${CPPI[@]}" -DLINUX_VERSION=409 -undef -D__DTS__ \
	-x assembler-with-cpp -o "$TMP" "$DTS"

dtc -@ -H both -O dtb -o "$OUT" -b 0 -i "$(dirname "$DTS")" "${DTCI[@]}" \
	-Wno-unit_address_vs_reg "$TMP"

echo "built $OUT ($(stat -c %s "$OUT") bytes)"
