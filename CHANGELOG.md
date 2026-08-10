# Changelog

## v0.1.2

Camera fixes for the Fairphone 3, and a release-integrity fix. Supersedes
v0.1.1, on which the Fairphone 3 could not use either camera.

### Fixed

- **The Fairphone 3 had no working cameras at all.** Its rear overlay points
  `flash-leds` at the flash controller, but `leds-qcom-flash` registers a
  V4L2 flash subdev per *LED*, using that LED's own fwnode. Nothing ever
  appeared for the controller, so the sensor's async notifier waited forever,
  the CAMSS notifier never completed, and the phone booted with **no
  `/dev/v4l-subdev*` whatsoever** — one stalled notifier takes down the whole
  media device, so the front camera died with the rear. The Fairphone 3+ was
  unaffected because only the IMX363 module declares a flash. Fixed by
  labelling the LED nodes and referencing those, plus enabling
  `CONFIG_V4L2_FLASH_LED_CLASS` — without which the driver registers a LED
  class device and no subdev at all. Kernel `6084f591155d`.
- **The capture tooling assumed a Fairphone 3+.** `fp3-cam-setup` hardcoded
  the entity name `s5kgm1sp 3-0010`, 4000x3000 and GRBG, and `cam-snap`
  hardcoded `/dev/v4l-subdev16`, 4000x3000 and `V4L2_PIX_FMT_SGRBG10P`. On a
  Fairphone 3 every `media-ctl` call silently missed and the script still
  reported success; `VIDIOC_STREAMON` then failed with `-EPIPE`, because
  CAMSS checks the video node's format against the pipeline feeding it.
  `fp3-cam-setup` now resolves the fitted module from the media graph and
  publishes what it found to `/run/fp3-cam-{rear,front}.conf`; `cam-snap`
  reads that instead of guessing, and derives the capture fourcc from the
  Bayer order.
- **The kernel config was not in the artifact checksum.** `package_files/0`
  listed `linux-6.6.defconfig`, which does not exist — the real config is
  `linux-6.19.defconfig`. Any kernel-config change therefore produced an
  identical checksum, so a published artifact could be handed out for source
  it was not built from.

### Known gaps

- The Fairphone 3 **front** camera still fails to stream: `csiphy_set_power`
  reports `clock enable failed: -16` and the pipeline never powers up. Its
  rear camera captures at the full 4032x3024, and both Fairphone 3+ cameras
  work.

## v0.1.1

First release with a published artifact. Supersedes v0.1.0, which was tagged
but never released and carried two defects fixed here.

### Fixed

- **The loudspeaker went silent a few minutes after boot, on both phones.**
  The modem firmware programs TLMM directly, remuxing GPIO 22/23 — i2c-6
  SDA/SCL, the bus the speaker amplifier sits on — from `blsp_i2c6` to plain
  GPIO driving low. With both lines held down, every register access to the
  amplifier failed as a bus error, so the AW8898 of the Fairphone 3 and the
  TAS2557 of the Fairphone 3+ died identically; it only looked like a codec
  bug. Linux never noticed, because pinctrl still believed its default state
  was applied and so refused to re-apply it. `i2c-qup` now alternates between
  the sleep and default pin states across its runtime PM cycle, which forces
  mux, bias and drive strength back to hardware before the first transfer
  after every idle period. Kernel `c3f8be7c3799`.
- **The artifact was 6.8 GB.** `BR2_ENABLE_DEBUG` is off, which takes it to
  637 MB; LLVM alone had shipped 977 MB of debug info for Rusticl's runtime
  OpenCL compiler.

### Removed

- **OpenCL / Rusticl.** Compiling kernels at runtime means shipping clang and
  LLVM on the device: `libLLVM` 62 MB, `libclang-cpp` 58 MB, `libclang` 32 MB,
  `libRusticlOpenCL` 11 MB and 14 MB of libclc SPIR-V, together 177 MB of a
  520 MB rootfs — and `rootfs.squashfs` was filling 91% of its 250 MiB
  partition. OpenCL 3.0 genuinely worked on the Adreno 506, but `cl_khr_fp16`
  is rejected on a5xx and convolution trips an IR3 shader hang, so the
  workloads it was added for ended up faster on the CPU. The a5xx compute
  patches stay in `patches/mesa3d` — GLES 3.1 compute shaders use the same
  ir3 paths — and `BR2_PACKAGE_MESA3D_{LLVM,OPENCL,RUSTICL}` brings it back.

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
