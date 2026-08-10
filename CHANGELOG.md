# Changelog

## v0.1.0

First public release. Nerves system for the Fairphone 3 and Fairphone 3+
(Qualcomm Snapdragon 632 / MSM8953, aarch64).

### One image, both phones

The Fairphone 3's rear camera, front camera and loudspeaker are
user-replaceable, and the Fairphone 3+ upgrade kit fits different silicon in
each. Both models share a mainboard and report identical `qcom,msm-id` and
`qcom,board-id`, so nothing before boot can tell them apart — and because the
modules are sold separately, a phone can carry any mix of the two
generations.

This release ships a **single device tree** that describes the module *slots*
rather than their contents. At boot `drivers/misc/fp3_module_slot.c` powers
each slot, reads the chip's ID register and applies a device tree overlay
describing what it found; the stock sensor and codec drivers then bind
normally. There is no per-variant image, no boot menu and no reboot.

| Slot         | Fairphone 3            | Fairphone 3+            |
| ------------ | ---------------------- | ----------------------- |
| Rear camera  | Sony IMX363 (12MP)     | Samsung S5KGM1SP (48MP) |
| Rear AF      | AK7374                 | DW9800W                 |
| Front camera | Samsung S5K4H7YX (8MP) | Samsung S5K3P9SP (16MP) |
| Loudspeaker  | Awinic AW8898          | TI TAS2557              |

### Hardware support

- Display, touchscreen, backlight, power key and volume keys
- 2G/3G/LTE dual SIM via QMI (in-SoC modem, QMAPv1)
- Wi-Fi and Bluetooth (wcnss/prima via remoteproc)
- NFC
- GNSS
- Adreno 506 GPU: OpenGL ES and OpenCL 3.0 via Mesa Freedreno + Rusticl
- Camera capture through CAMSS to V4L2, with `cam-snap` / `cam-stream` /
  `cam-grab` userspace tooling
- Loudspeaker via ADSP
- Qualcomm sensor stack (ADSP, `sns.reg`)
- Battery charging and fuel gauge (PMI632)
- A/B firmware updates into the Android `userdata` partition, booted by
  lk2nd-msm8953 through extlinux

### Known gaps

- `cl_khr_fp16` is rejected by Rusticl on Adreno 506; use int8/int4 packed
  storage with fp32 dequantisation for memory-bound workloads.
- Convolution on the GPU triggers an IR3 shader hang on a5xx and falls back
  to CPU.
- The camera pipeline demosaics in software. Driving the msm8953 CPP
  hardware ISP is not implemented.
- GPS XTRA assistance data is not downloaded.
- The USB gadget IDs in `packages/citronics-initramfs/deviceinfo` are the
  generic Google ones; a product should use its own.
