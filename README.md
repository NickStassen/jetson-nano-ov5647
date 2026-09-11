# jetson-nano-ov5647

OV5647 (Raspberry Pi Camera v1 / cheap 5 MP clones) CSI camera support for the
**Jetson Nano B01 developer kit** on **L4T 32.5.1 / JetPack 4.5.1** (kernel 4.9.201-tegra).

NVIDIA never shipped an OV5647 driver for the Nano; only IMX219 (Pi Cam v2) and,
via Jetson-IO on newer releases, IMX477 are supported out of the box. This repo
adds:

- `driver/` – an out-of-tree `ov5647.ko` built on the NVIDIA *tegracam* framework so
  the sensor works with both plain V4L2 (`v4l2-ctl`) and the ISP path
  (`nvarguscamerasrc`).
- `dts/` – device tree sources adding the sensor to CSI port 0 (`cam0`) of the B01
  carrier, plus the `tegra-camera-platform` node Argus needs.
- `scripts/` – build/install helpers that run natively on the Nano against the
  stock `nvidia-l4t-kernel-headers` package (no cross toolchain required).

## Status

Work in progress. See the commit log for the current state.

## Target hardware

| Item | Value |
|---|---|
| Board | Jetson Nano Developer Kit, module p3448-0000, carrier p3449-0000 rev B01 |
| L4T | R32.5.1 (`/etc/nv_tegra_release`) |
| Kernel | 4.9.201-tegra |
| Sensor | OmniVision OV5647, I2C address 0x36, 2-lane MIPI CSI-2 |
| Port | CSI port 0 (`cam0`, the connector nearest the barrel jack side) |

## Notes

The Nano's `CAM0_PWDN` / `CAM1_PWDN` lines (Tegra GPIO 151 / 152) gate the module's
on-board regulator. Until a driver claims the sensor and drives that pin high the
sensor is completely invisible on I2C, which is what a "no camera detected" board
looks like even with a perfectly good module attached.
