# jetson-nano-ov5647

OV5647 (Raspberry Pi Camera v1 and the many cheap 5 MP clones) CSI camera
support for the **Jetson Nano B01 developer kit** on **L4T 32.5.1 / JetPack
4.5.1** (kernel `4.9.201-tegra`).

NVIDIA never shipped an OV5647 driver for the Nano; only IMX219 (Pi Cam v2) and,
via Jetson-IO on newer releases, IMX477 work out of the box. This repo adds:

- `driver/` – an out-of-tree `ov5647.ko` built on NVIDIA's *tegracam* framework,
  so the sensor works both with plain V4L2 (`v4l2-ctl`, OpenCV) **and** through
  the ISP path (`nvarguscamerasrc`, Argus, `nvgstcapture`), with auto exposure,
  auto white balance and the rest of the Argus pipeline.
- `dts/` – device tree sources describing two OV5647 modules on the B01
  carrier's CSI ports (cam0 and cam1), plus the `tegra-camera-platform` node
  Argus needs.
- `scripts/` – build/install helpers that run natively on the Nano against the
  stock `nvidia-l4t-kernel-headers` package. No cross toolchain, no kernel
  rebuild, no reflash.

## Status

Working, tested on a p3448-0002 (eMMC) module on a p3449-0000 B01 carrier with
a generic OV5647 module on CSI port 0.

| Mode | Resolution | Argus fps (measured) | Notes |
|---|---|---|---|
| 0 | 2592 x 1944 | 15 | full frame |
| 1 | 1920 x 1080 | 30 | centre crop |
| 2 | 1296 x 972 | 30 | 2x2 binned |
| 3 | 640 x 480 | 60 | binned + skipped |

All modes are 10-bit BGGR Bayer over 2 MIPI lanes. Both V4L2 raw capture and
`nvarguscamerasrc` JPEG/NVMM output were verified in every mode with no VI/CSI
errors in `dmesg`.

## Install (on the Nano)

Prerequisites on the Nano (all present on a stock JetPack 4.5.1 image):
`nvidia-l4t-kernel-headers`, `build-essential`, `device-tree-compiler`.

1. On your PC, fetch the L4T device tree sources once and push everything to
   the Nano (only ~5 MB of dts includes are copied):

   ```sh
   scripts/fetch-l4t-sources.sh          # downloads public_sources.tbz2 into build/src
   scripts/sync-to-jetson.sh user@nano   # rsync driver/, dts/, scripts/, dts includes -> ~/ov5647-build
   ```

2. On the Nano, build the module and the device tree:

   ```sh
   cd ~/ov5647-build
   make -C driver
   # pick the file matching your module: p3448-0002 = eMMC (production) module,
   # p3448-0000 = SD card devkit module. `cat /proc/device-tree/chosen/plugin-manager/ids/*`
   # or `ls /proc/device-tree/chosen/plugin-manager/ids` tells you which you have.
   scripts/build-dtb.sh dts/tegra210-p3448-0002-p3449-0000-b00-ov5647.dts ov5647.dtb
   ```

3. Install and reboot:

   ```sh
   sudo scripts/install-on-jetson.sh driver/ov5647.ko ov5647.dtb
   sudo reboot
   ```

   The install script copies the module to `/lib/modules/$(uname -r)/extra`,
   the DTB to `/boot/ov5647/`, and adds a new `LABEL ov5647` entry to
   `/boot/extlinux/extlinux.conf` that reuses your current kernel/initrd/cmdline
   but points `FDT` at the new DTB. Your previous entry is kept as a fallback
   (selectable over the serial console during the boot menu timeout) and the
   original file is saved as `extlinux.conf.pre-ov5647`.

4. Check:

   ```sh
   dmesg | grep ov5647          # "OV5647 chip id 0x5647 detected", "detected ov5647 sensor"
   v4l2-ctl --list-devices      # vi-output, ov5647 7-0036 -> /dev/video0
   gst-launch-1.0 nvarguscamerasrc num-buffers=30 ! 'video/x-raw(memory:NVMM),width=1920,height=1080,framerate=30/1' ! nvjpegenc ! multifilesink location=/tmp/cap_%03d.jpg
   ```

## Using it

**Argus / ISP path** (auto exposure, AWB, denoise, etc.):

```sh
gst-launch-1.0 nvarguscamerasrc sensor-id=0 ! 'video/x-raw(memory:NVMM),width=1920,height=1080,framerate=30/1' ! nvvidconv ! xvimagesink
```

**Raw V4L2 path.** Because the device tree sets `use_sensor_mode_id`, the VI
picks the sensor mode from the `sensor_mode` control, not from the requested
frame size. Select the mode explicitly:

```sh
v4l2-ctl -d /dev/video0 --set-ctrl sensor_mode=1 --set-ctrl bypass_mode=0 \
  --set-ctrl frame_rate=30000000 \
  --set-fmt-video=width=1920,height=1080,pixelformat=BG10 \
  --stream-mmap --stream-count=30 --stream-to=/tmp/raw.bin
```

Also set `frame_rate` (in fps x 1e6): tegracam keeps the last value across
mode changes instead of resetting to the mode's default, so after using the
15 fps full-frame mode every other mode also runs at 15 fps until you say
otherwise. `exposure` is in microseconds, `gain` in 1/16 steps (16 = 1.0x).
Argus sets all of these itself, so this only matters on the raw path.

Frames are 16-bit little-endian per pixel with the 10-bit sample in the low
bits, Bayer order BGGR, rows padded to a 64-byte stride.

## Second camera

The device tree describes cam1 (CSI port 1, i2c bus 8) too. With nothing
attached the driver logs one line, `no OV5647 detected on this port`, and Argus
simply enumerates a single sensor. Plug a second module in and it appears as
`sensor-id=1` / `/dev/video1`.

## How it fits together

- **Driver** (`driver/ov5647.c`): structurally a copy of NVIDIA's `imx219.c`
  tegracam driver. Register sequences come from the Raspberry Pi kernel
  `ov5647.c`. HTS/VTS are written explicitly per mode (the Pi driver programs
  them through V4L2 blanking controls). Exposure is written as
  `lines << 4` into `0x3500..0x3502`, analog gain 1:1 into `0x350a/0x350b`
  (16 = 1.0x, matching `gain_factor = 16` in the DT), frame rate via VTS.
  Manual AEC/AGC (`0x3503 = 0x03`) and on-sensor AWB off (`0x5001 = 0x00`)
  so the Tegra ISP gets untouched Bayer data. MIPI clock lane is
  non-continuous by default; `modprobe ov5647 continuous_clock=1` switches it
  (then also set `discontinuous_clk = "no"` in the DT).
- **Device tree** (`dts/`): forks of the three stock files that reference the
  camera (`tegra210-p3448-*-b00.dts`, `tegra210-porg-p3448-common.dtsi`,
  `porg-platforms/tegra210-porg-camera.dtsi`) with the dual IMX219 include
  swapped for `tegra210-camera-rbpcv1-dual-ov5647.dtsi`. Everything else is
  pulled unchanged from the L4T sources at compile time, and
  `scripts/build-dtb.sh` reproduces NVIDIA's shipped
  `tegra210-p3448-0002-p3449-0000-b00.dtb` byte for byte (apart from the
  build timestamp), so the only differences on the running system are the
  camera nodes.
- **Plugin-manager gotcha**: on B01 boards the camera nodes are enabled at
  boot by NVIDIA's plugin-manager fragment, which matches nodes *by label* and
  also rewrites the `tegra-camera-platform` module names to IMX219. The OV5647
  dtsi keeps the stock labels so the fragment still applies, and
  `tegra210-porg-plugin-manager-ov5647-fixup.dtsi` (included **after** the
  plugin-manager file, since later definitions win in dtc) overrides just the
  `devname`/`badge`/lens paths so `nvargus-daemon` finds `ov5647 7-0036`.
  Getting that include order wrong yields a working `/dev/video0` but
  "No cameras available" from Argus.
- **Power**: the Nano's `CAM0_PWDN`/`CAM1_PWDN` lines (Tegra GPIO 151/152,
  `reset-gpios` in the DT) gate the Pi-style module's on-board regulator.
  Until a driver drives that pin high the sensor is invisible on I2C, which
  is exactly what a "dead" camera looks like even with a good module.

## Troubleshooting

- **Argus says "No cameras available" but `/dev/video0` works.** Check
  `cat /proc/device-tree/tegra-camera-platform/modules/module0/drivernode0/devname`;
  it must read `ov5647 7-0036`. If it still says `imx219 7-0010` the
  plugin-manager fixup dtsi was not included after the plugin-manager (see above).
  Also restart the daemon after loading the module: `sudo systemctl restart nvargus-daemon`.
- **Strong magenta/pink cast in Argus output while raw V4L2 frames look fine.**
  Look for `/var/nvidia/nvcam/settings/camera_overrides.isp`. Stock L4T does not
  ship one; camera vendors' installers (Arducam's IMX477 package, for example)
  drop a calibration for *their* sensor there and Argus applies it to every
  camera. Move it aside and restart `nvargus-daemon`. Without a tuning file the
  image is neutral but untuned (default ISP AWB/CCM), which is the expected
  starting point for a sensor NVIDIA never calibrated.
- **Camera invisible on I2C (`i2cdetect -y -r 7` shows nothing at 0x36).** That is
  normal when no driver has claimed the port: the PWDN line is hogged low and the
  module is unpowered. It does not mean the module is bad.
- **v4l2-ctl runs every mode at 15 fps.** The `frame_rate` control is sticky; pass
  `--set-ctrl frame_rate=30000000` (or 60000000) along with `sensor_mode`.
- **v4l2-ctl captures the wrong resolution.** Set `--set-ctrl sensor_mode=N`
  (see "Using it"); the frame size alone does not select the mode.

## Layout

```
driver/   ov5647.c, ov5647_mode_tbls.h, Makefile          (out-of-tree module)
dts/      forked B01 dts + porg-platforms/*ov5647*.dtsi   (device tree)
scripts/  fetch-l4t-sources.sh, sync-to-jetson.sh, build-dtb.sh, install-on-jetson.sh
```

## Credits / references

- NVIDIA L4T 32.5.1 public sources (`imx219.c`, porg device trees), GPL-2.0.
- Raspberry Pi kernel `drivers/media/i2c/ov5647.c` for the register tables, GPL-2.0.
- GiraffAI's OV5647-on-Nano write-up and the community ports on GitHub for
  confirming the tegracam approach was viable.

License: GPL-2.0 (kernel module and device tree sources), same as the code
they derive from.
