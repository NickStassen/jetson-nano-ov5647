#!/bin/bash
# Install the OV5647 driver module and device tree on a Jetson Nano and add
# a new extlinux boot entry for it. Run ON THE JETSON as root:
#
#   sudo scripts/install-on-jetson.sh <ov5647.ko> <ov5647.dtb>
#
# What it does:
#   1. copies ov5647.ko to /lib/modules/$(uname -r)/extra and runs depmod
#   2. copies the DTB to /boot/ov5647/
#   3. backs up /boot/extlinux/extlinux.conf (once) and adds a LABEL "ov5647"
#      that reuses the current default entry's kernel/initrd/cmdline but
#      points FDT at the new DTB; makes it the DEFAULT. The previous entry is
#      left intact as a fallback (selectable over the serial console).
#
# Re-running is safe: files are overwritten and the ov5647 entry replaced.
set -euo pipefail

KO=${1:?path to ov5647.ko}
DTB=${2:?path to ov5647 .dtb}
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }

KVER=$(uname -r)
EXTLINUX=/boot/extlinux/extlinux.conf

echo ">> installing module"
install -D -m 644 "$KO" "/lib/modules/$KVER/extra/ov5647.ko"
depmod -a "$KVER"
echo ov5647 > /etc/modules-load.d/ov5647.conf

echo ">> installing device tree"
install -D -m 644 "$DTB" "/boot/ov5647/$(basename "$DTB")"

echo ">> updating $EXTLINUX"
[ -f "$EXTLINUX.pre-ov5647" ] || cp -a "$EXTLINUX" "$EXTLINUX.pre-ov5647"

python3 - "$EXTLINUX" "/boot/ov5647/$(basename "$DTB")" <<'PY'
import re, sys
path, fdt = sys.argv[1], sys.argv[2]
text = open(path).read()
lines = text.splitlines()

# Split into header (before first LABEL) and LABEL blocks.
header, blocks, cur = [], [], None
for ln in lines:
    m = re.match(r'^\s*LABEL\s+(\S+)', ln)
    if m:
        cur = {"name": m.group(1), "lines": [ln]}
        blocks.append(cur)
    elif cur is None:
        header.append(ln)
    else:
        cur["lines"].append(ln)

def get(block, key):
    for ln in block["lines"]:
        m = re.match(r'^\s*' + key + r'\s+(.*)$', ln)
        if m:
            return m.group(1).strip()
    return None

default = None
for i, ln in enumerate(header):
    m = re.match(r'^\s*DEFAULT\s+(\S+)', ln)
    if m:
        default = m.group(1)
        header[i] = "DEFAULT ov5647"
if default is None:
    header.insert(0, "DEFAULT ov5647")

# Drop any previous ov5647 entry, then find the entry we are cloning.
blocks = [b for b in blocks if b["name"] != "ov5647"]
base = next((b for b in blocks if b["name"] == default), blocks[0])

linux  = get(base, "LINUX")  or "/boot/Image"
initrd = get(base, "INITRD") or "/boot/initrd"
append = get(base, "APPEND") or "${cbootargs} root=/dev/mmcblk0p1 rw rootwait rootfstype=ext4"

new = [
    "LABEL ov5647",
    "      MENU LABEL %s + OV5647 camera device tree" % (get(base, "MENU LABEL") or base["name"]),
    "      LINUX " + linux,
    "      INITRD " + initrd,
    "      APPEND " + append,
    "      FDT " + fdt,
    "",
]

# Keep comment-only trailing text of the base block with the base block.
out = header + [""] + new
for b in blocks:
    out += b["lines"]
    if out and out[-1].strip():
        out.append("")
open(path, "w").write("\n".join(out).rstrip("\n") + "\n")
print("cloned entry '%s' -> LABEL ov5647 (FDT %s)" % (base["name"], fdt))
PY

echo
cat "$EXTLINUX"
echo
echo ">> done. Reboot to use the new device tree."
