# Changelog

## v0.1.3

Both cameras work on both phones, stills and live H.264, and the
streaming path is no longer flaky. Supersedes v0.1.2, on which the
Fairphone 3's front camera could not stream at all.

### Fixed — cameras

Five independent faults sat between "a camera exists" and "you can watch
it", each hiding the next.

- **The CSIPHY could not power up for the front sensor.**
  `csi{0,1,2}phytimer_clk_src` pointed at the wrong GPLL0_DIV2 mux index
  — 2, where Qualcomm's own driver programs 4. Only the 100 MHz rate is
  sourced from that parent, so the fault stayed invisible until a sensor
  asked for it: every other msm8953 sensor in tree lands on 200 MHz, and
  the Fairphone 3's 350 MHz front sensor is the first device to reach the
  100 MHz rung. Kernel `c6e7d2b9d9c5`.

- **The front camera's 1.2 V rail was never actually switched on.**
  `regulator-fixed` asserts its enable GPIO's output-enable once, at
  probe; afterwards the core only writes the pin's *data* register, while
  `_regulator_is_enabled()` reports a software flag. The modem programs
  TLMM directly and clears the output-enable on the pins of QDSS trace
  bus B, where GPIO 46 lives. The rail read as enabled while its pin had
  quietly become an undriven input. Kernel `0dcf16c4a8fc`.

- **The Fairphone 3+ front camera returned pure noise at full
  resolution.** Its driver declared half the real MIPI link frequency,
  for the 4608x3456 mode only: `OP_SYS_CLK_DIV` is 0 for full res and 1
  for binned, and the formula applied `/2` to both. CAMSS sizes the VFE
  clock from the pixel rate, so 292.8 Mpix/s selected the 100 MHz rung
  where 585.6 needs 160 — the RDI path drained at roughly 427 Mpix/s
  against 507 delivered, and every frame overflowed. Nothing in the RDI
  path validates anything, so no error appeared anywhere. Binned was
  declared correctly and always worked, which is what made it look like
  a sensor fault. Kernel `6.19/staging b91c46147a38`.

- **The H.264 encoder was never present.** `venus-core` binds the Venus
  device from its device-tree compatible and then creates child platform
  devices for the decoder and encoder that have no DT node of their own,
  so nothing autoloads `venus-enc`/`venus-dec` and they sit unbound
  forever. `fp3-cam-setup` now loads them. Building the media stack into
  the kernel does not fix this and makes it worse: a built-in
  `venus-core` probes before the rootfs is mounted and cannot load its
  firmware at all.

- **The capture tooling opened the wrong video node.** `fp3-cam-setup`
  hardcoded `/dev/video0` and `/dev/video1`, but CAMSS registers its
  video nodes alongside Venus and the numbers move between phones and
  between boots. The pipeline was configured on one RDI lane while
  frames were read from another, and `VIDIOC_STREAMON` failed with a
  silent `-EPIPE`.

### Fixed — streaming

- **Live streams lagged 2-3 seconds behind.** The frame pacer was
  disabled, so the pipeline ran at whatever the sensor and encoder
  managed — 43-45 fps into a stream the player treats as 30. The player
  falls a third of a second behind every second and the lag grows
  without bound. Pacing is against a fixed cadence; naive
  time-since-last-submit pacing rejects frames arriving marginally early
  and compounds, measuring 24.5 fps out of a 30 fps sensor. Now 30.0 fps
  at 20 ms sensor-to-socket on both phones.

- **The Fairphone 3's front stream sheared into diagonal noise.** Venus
  wants its NV12 input width a multiple of 128. That camera bins to 1440
  and lands on 1424, which the driver silently padded to 1536 while the
  writer kept producing 1424-wide rows. Every other camera bins to at
  least 1920 and caps at 1920 = 15x128, so the fault appeared on exactly
  one camera. `cam-stream` now refuses to start rather than emit a
  sheared picture.

- **Stopping a stream wedged Venus.** `cam-stream` handled SIGINT but not
  SIGTERM, which is what the library sends, so the default action killed
  it before `VIDIOC_STREAMOFF` ran and left the encoder streaming into
  freed buffers. The next run hit `wait for cpu and video core idle fail
  (-110)`, which takes CAMSS down with it and stops stills working too.
  That was long blamed on the Venus driver.

- **Teardown now insists.** SIGTERM, then SIGKILL, then wait until the
  port can actually be bound. A wedged Venus ioctl never reaches the
  signal handler, so a restart used to collide with its own corpse and
  fail on `bind: Address already in use`.

- **Failures are reported instead of hidden.** `start_stream/2` watches
  the child long enough to catch a bind failure and returns an error
  with the child's own message, rather than an `{:ok, ref}` that
  `stop_stream/1` later rejects as `:not_found`. Stalled streams are
  detected from the encoder's frame heartbeat and restarted, and both
  binaries set `PR_SET_PDEATHSIG` so an orphan cannot outlive the VM
  holding its port.

### Fixed — device identity

`/etc/boardid.config` read `-n 8` where it meant `-l 8`, and ended in
`|| true`, which is shell syntax in a file that is not shell. Every FP3
answered `"0"`: hostname `nerves-0` on every device, and because
VintageNetDirect hashes the hostname to pick its `/30`, two phones on one
host claimed the same address. It now reads the Qualcomm SoC serial,
falling back to the eMMC CID — which is what actually answers, because
the SoC serial is not populated yet when erlinit builds the hostname.

### Verified

Both phones, all four camera modules, stills and live H.264, from a
clean build at the pinned kernel SHA. Every frame was decoded and
checked for row correlation rather than trusted on byte count — noise
scores 0.58-0.80 on that measure, real images 0.98 and above.

| | Fairphone 3 | Fairphone 3+ |
| --- | --- | --- |
| Rear | 4032x3024 IMX363, 0.993 | 4000x3000 S5KGM1SP, 0.997 |
| Front | 3264x2448 S5K4H7YX, 0.983 | 4608x3456 S5K3P9SP, 0.989 |

### Known gaps

- Venus can still wedge mid-stream (`wait for cpu and video core idle
  fail`). The stall detector now catches it and restarts the stream, but
  the underlying fault is not understood.
- Colour is close to Android's on all four sensors but is not calibrated
  against a known target under known illuminants.
- Streams centre-crop rather than scale, so they see roughly 30% less
  vertical field of view than a still from the same camera.
- Auto-exposure runs out of range on the FP3+ front at full resolution
  in dim light.

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
