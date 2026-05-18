# Path B — CPP HW-ISP backport plan (analysis only, no code yet)

**Goal**: bring the FP3+ to phone-camera-class still-image quality by
driving the CPP (Camera Post-Processor) hardware block on msm8953 from
Linux 6.x. The CPP is the fixed-function HW ISP that the Qualcomm
camera HAL on Android offloads colour correction, scaling, noise
reduction, sharpening, and JPEG-input prep to. Today our `qcom_camss`
driver stops at raw-Bayer V4L2 capture (CSID → ISPIF → VFE-RDI →
`/dev/video0`); everything past that is software in `cam-snap`.

If we get CPP running, we plug a HW-accelerated M2M stage between raw
capture and JPEG encode, replacing our software demosaic / CCM / gamma
/ sharpening with the real silicon path. NB: this does **not** give us
phone-quality 3A (AE / AWB / AF) — see §7.


## 1. The hardware we need to wake up

From the LineageOS `msm8953-camera.dtsi` DT node `qcom,cpp@1b04000`:

| Resource | Address / value | Source |
|---|---|---|
| CPP MICRO regs | `0x1b04000 + 0x100` | DT |
| CPP VBIF regs | `0x1b80000 + 0x200` | DT |
| CPP HW regs | `0x1b18000 + 0x18` | DT |
| CAMSS-CPP slot | `0x1858078 + 0x4` | DT |
| IRQ | GIC SPI 49 | DT |
| Power domain | `cpp_gdsc` | `gcc-msm8953.c` |
| Reset | `GCC_CAMSS_MICRO_BCR` | `gcc-msm8953.c` |
| SMMU SID | `0x1c00` on `apps_iommu` | DT (`msm_cam_smmu_cb3`) |
| Clocks needed | `CPP_CLK_SRC`, `GCC_CAMSS_CPP_AHB_CLK`, `GCC_CAMSS_CPP_AXI_CLK`, `GCC_CAMSS_CPP_CLK`, `GCC_CPP_TBU_CLK`, `GCC_CAMSS_MICRO_AHB_CLK`, `GCC_CAMSS_TOP_AHB_CLK`, `GCC_CAMSS_AHB_CLK` | `gcc-msm8953.c` |
| Firmware | `cpp_firmware_v1_*.fw` (10 versions in FairBlobs) + `cppf.b00..b06+mdt` (alt format) | FairBlobs `FP3-firmware` |

**Good news**: every clock, the `cpp_gdsc` regulator, and the
`GCC_CAMSS_MICRO_BCR` reset line are already exposed by our upstream
`gcc-msm8953.c` driver. The SMMU `apps_iommu` phandle is also wired
in our `msm8953.dtsi`. So the plumbing is in place — we just lack the
consumer.


## 2. What's actually missing: full dependency tree

`drivers/media/platform/msm/camera_v2/pproc/cpp/msm_cpp.c` is 4814
LOC. But it is **not standalone** — it sits at the bottom of a stack
of MSM-only frameworks. Headers it pulls in and what each is:

| Include | What it is | Status in upstream 6.x |
|---|---|---|
| `linux/ion.h`, `linux/msm_ion.h` | Qualcomm ION memory allocator | **Gone**. Replaced by DMA-BUF heaps. |
| `linux/clk/msm-clk.h` | Qualcomm clock helper macros | **Gone**. Use generic CCF. |
| `media/msmb_camera.h` | "MSM Camera Bus" V4L2-shaped framework | **Never upstreamed**. ~3100 LOC of `msm.c` glue, custom subdev registration. |
| `media/msmb_generic_buf_mgr.h` | MSM buffer manager | Never upstreamed. ~1500 LOC. |
| `media/msmb_pproc.h` | CPP UAPI | 255 LOC. We could keep verbatim. |
| `msm_isp_util.h` | Links CPP to V2 ISP driver | Drags in another ~10k LOC. |
| `msm_camera_io_util.h` | Qualcomm IO/clk helpers | Replace with `clk_bulk_*` + `readl/writel`. |
| `cam_smmu_api.h` | Qualcomm SMMU wrapper | Replace with `iommu_get_domain_for_dev`. |

So total downstream code involved if we naively backport: **camera_v2
is 91 756 LOC** (full count of `*.c + *.h`). Of that, *core msm bus
+ buf_mgr alone is ~6 300 LOC*. The CPP driver assumes all of that is
present.

A naive lift-and-shift is therefore not feasible. A **rewrite of the
CPP driver to use upstream-standard APIs** is the only realistic path.
That delta breaks down as:

### 2.1 Code to write fresh (~1 500–3 000 LOC)

* Glue layer: replace `msm.c` subdev registration with standard
  `v4l2_device` + `video_device` + `v4l2_m2m`.
* Buffer flow: replace `msmb_generic_buf_mgr` with `videobuf2`
  (`vb2_dma_contig` or `vb2_dma_sg`).
* DMA: replace ION imports with DMA-BUF import via `dma_buf_attach()`
  /`dma_buf_map_attachment()`.
* Clocks/power: `devm_clk_bulk_get_all()` + `regulator_get()` +
  generic `reset_control`.
* SMMU: `of_dma_configure()` + `iommu_attach_device()` against the new
  per-CPP context bank in DT.
* Bus voting (`qcom,msm-bus,*` properties): convert to `interconnect`
  framework which is in upstream.

### 2.2 Code to lift from downstream (~1 500–2 000 LOC, lightly edited)

* HW register touch sequences in `msm_cpp.c` — these directly poke the
  CPP micro / VBIF / HW registers and don't depend on the framework.
  About 40 functions: `cpp_init_hw_regs`, `cpp_load_fw`,
  `msm_cpp_reset_vbif_and_load_fw`, the IRQ handler, the
  TX/RX FIFO drain loop, etc.
* Frame-command write loop (the inner-ring code that actually
  dispatches a frame to the CPP micro via `MSM_CPP_MICRO_FIFO_TX_DATA`).
* `msm_cpp_soc.c` clock-rate / bus-vote tables (250 LOC, mostly data).
* CPP HW version table (`CPP_HW_VERSION_4_0_0` etc.) — msm8953 reports
  `4_0_0` per downstream code.

### 2.3 UAPI

* `msmb_pproc.h` can be kept verbatim — it just defines the ioctl
  numbers and the `msm_cpp_frame_info_t` struct. Userspace that
  expects this ABI (e.g. extracted Qualcomm HAL code) can talk to us
  unchanged.
* **Open design question**: do we keep the legacy MSM ioctls or layer
  V4L2 M2M (`VIDIOC_QBUF` etc.) on top? Keeping MSM ioctls = easier
  port of HAL code; V4L2 M2M = idiomatic and works with GStreamer.
  Recommendation: V4L2 M2M is the right choice for our use case,
  and we add a thin MSM-style ioctl handler only if we end up wanting
  to load proprietary HAL command builders.


## 3. Userspace strategy — three options

Even with the kernel driver done, userspace has to **build the CPP
command stream** (the `cpp_cmd_msg` `uint32_t*` array). The driver
only dispatches; the *meaning* of those words is the CPP
microcontroller's instruction set.

### 3.1 Option A — Rewrite from scratch in cam-snap

Document the CPP command word format, then issue commands ourselves
(set scaler factors, CCM coefficients, gamma LUT, denoise params).
Possible because the CPP commands are register writes through the
TX FIFO — they're not encrypted. But the **command-format
documentation does not exist publicly**: we'd need to derive it from
the binary chromatix libs + register tracing on the Android side
(see §4).

Effort: very high. The CPP supports ~30 different processing modules
(scaler, CSC, gamma, NR, sharpening, …), each with its own register
layout.

### 3.2 Option B — Lift Qualcomm HAL command-builder code

The libchromatix\_\*.so files only contain data. The actual command
builders live in `libmmcamera_isp_*.so` and `libmmcamera_cpp_*.so`
under `/vendor/lib/`. These are Android binaries (Bionic, libcutils,
…). We could either:

a. Reverse-engineer them and rewrite in C. The chromatix data + the
   command-build templates would then live entirely in our codebase.
b. Run them under **libhybris** so we can call Bionic-linked libs
   from glibc. Pmos has done this for the camera HAL on other
   Qualcomm boards.

Option (a) is a multi-week RE job. Option (b) is fragile but quick.

### 3.3 Option C — Use the CPP only for the bits we can drive ourselves

The CPP has a "passthrough" mode where it does just a colour-space
conversion + scaler without invoking the firmware-loaded modules.
We could:

* CCM via the 3×3 colour-correction matrix register block,
* gamma via the 256-entry LUT block,
* downscale via the scaler block,

…and skip the NR / sharpening / advanced bits that need proprietary
command-stream knowledge. Register layouts for these basic blocks ARE
visible in the downstream code (`msm_cpp.h` has them).

This option still gets us HW-accelerated colour pipeline at full
resolution, while leaving advanced denoise on the table. **Cleanest
"buy what we can pay for" path.**


## 4. Reverse-engineering / data path

If we want to go beyond Option C, the data we have is:

* `libchromatix_s5kgm1sp_common.so` — 56 KB .data blob (extracted).
  Contains the static CCM, gamma, NR coefficients per illuminant.
  Layout unknown without the Qualcomm header file.
* `libchromatix_s5kgm1sp_snapshot.so` and friends — per-mode tuning
  on top of common.
* `libmmcamera_isp_*.so` and `libmmcamera_cpp_*.so` — command
  builders. Not yet pulled. **We need these to know the wire format.**
* Live Android camera2 metadata via `dumpsys media.camera` — schema
  only, no values. Could be read with a small Android binary that
  uses NDK camera2.
* Live tracing with `mmap`-trace + register dump on Android — would
  show us exactly which CPP registers the HAL writes per frame.
  Requires root on the Android phone.

The **most valuable single artefact** would be a stock-Android
register trace of one capture: input → list of CPP register writes →
output. With that we can decode the command stream regardless of the
chromatix internals. Achievable in 1-2 days with `strace` /
`/sys/kernel/debug/dynamic_debug` / a kprobe stub on the downstream
kernel.


## 5. Kernel-side build-out, ordered

Stages we'd execute, each independently verifiable:

### Stage 1 — DT + clocks + power-domain bring-up (1-2 days)

* Add `cpp@1b04000` node to `sdm632-fairphone-fp3p.dts` describing
  the four register regions, IRQ, clocks (by `gcc-msm8953` indices),
  `power-domains = <&gcc CPP_GDSC>`, `resets`, `iommus`, and an
  empty new compatible string like `"qcom,msm8953-cpp"`.
* Add a `cpp_iommu` context bank under `apps_iommu` matching SID
  0x1c00, mirroring how `vfe_iommu` is set up in the upstream camss
  node.
* Write a stub platform driver that does nothing but `probe()` →
  `clk_bulk_prepare_enable` → `regulator_enable` → `reset_assert/deassert`
  → read the CPP MICRO HW_VERSION register → print → `remove()` does
  the inverse. **Exit criterion**: dmesg shows `qcom,cpp: HW version
  0x40000000` after boot.

This is low-risk and worth doing first because it validates that the
clock and regulator framework can hold CPP in a working state
independently of the rest of the camera pipeline.

### Stage 2 — Firmware loading (2-3 days)

* Ship `cpp_firmware_v1_8_0.fw` (the version downstream uses for
  msm8953) in the system as `/lib/firmware/cpp_firmware.fw`.
* Implement `request_firmware()` → boot the CPP micro:
  1. Reset HW
  2. Write firmware bytes through `MSM_CPP_MICRO_FIFO_TX_DATA`
  3. Issue `MSM_CPP_CMD_FW_LOAD` then `MSM_CPP_CMD_EXEC_JUMP`
  4. Wait for IRQ + bootloader-version response
* Lift the FW-load sequence verbatim from `cpp_load_fw()` in
  `msm_cpp.c:1934-2070`.
* **Exit criterion**: bootloader response present in RX FIFO, IRQ
  fires correctly.

### Stage 3 — Frame dispatch (3-5 days)

* V4L2 M2M video device exposed as `/dev/videoN`.
* Output queue takes RGB/NV12 input (from our raw-Bayer demosaic).
* Capture queue produces processed RGB/NV12 output.
* Convert one `vb2_buffer` into the CPP frame format using either
  passthrough scaler-only commands (Option C) or full command stream
  (Option B).
* Wait for IRQ, dequeue the result, hand it back to userspace.
* **Exit criterion**: `gst-launch-1.0 v4l2src device=/dev/videoCPP
  ! ... ! filesink` produces a correctly-scaled output frame.

### Stage 4 — Real ISP commands (decision point)

* If we picked **Option C**: write CCM 3×3 + gamma 256-LUT + scaler
  commands and call it done. ~1 week.
* If we picked **Option B**: build out the command-template database
  from the RE'd chromatix libs and HAL .so files. ~3-6 weeks.
* If we picked **Option A**: derive command format from register
  traces and ship our own builders. ~6-10 weeks.


## 6. Userspace integration

Regardless of kernel-side choice, the cam-snap pipeline becomes:

```
sensor → /dev/video0 (raw Bayer) → cam-grab unpack
       → /dev/videoCPP (M2M)      → CPP firmware processes it
       → /dev/videoCPP (capture)  → libjpeg-turbo → output.jpg
```

In ex_camera / Membrane, that's just two additional pad connections.


## 7. **Critical blind spots** — what this plan does NOT solve

These are things Path B will *not* fix on its own:

1. **AE / AWB / AF** still run on the ADSP via FastRPC in the
   Qualcomm stack. We have no FastRPC userland, and the AF VCM
   (ak7375) has an I²C resume bug we still owe. CPP alone doesn't
   change exposure or pick focus — we'd still be guessing those.
2. **Per-unit calibration (sensor OTP)**. Lens shading correction
   and per-channel pedestal vary by sensor module instance. Qualcomm
   reads these from the sensor's I²C EEPROM at boot. Our s5kgm1sp
   driver doesn't expose the EEPROM yet. Without it, even with CPP
   doing the LSC we'd be applying a generic table.
3. **Hexagon DSP image algorithms (HDR, MFNR, denoise)** that run on
   the ADSP via FastRPC. These are entirely separate from CPP and
   are what makes phones look "computational". Out of scope for Path B.
4. **CPP firmware closed-source**. We're shipping the Qualcomm
   `cpp_firmware_v1_*.fw` binary and have no debugger / no source.
   When something goes wrong on the firmware side we can only
   reset + reload.
5. **Command-stream format is undocumented for the high-level
   modules** (NR, sharpening, dynamic range). Options A/B/C above
   determine how much we work around this.
6. **No upstream interest** for an msm8953 CPP driver. Whatever we
   build is forever in our staging fork — no help from `linux-media`
   maintainers reviewing.
7. **Sensor mode**: even with CPP, the s5kgm1sp driver in our staging
   only exposes 4000×3000 (2×2-binned). For 8000×6000 full-res we'd
   need a separate sensor-driver patch independent of Path B.


## 8. Effort estimate

| Stage | Optimistic | Realistic | Pessimistic |
|---|---|---|---|
| Stage 1 (DT + bring-up) | 1 day | 2 days | 4 days |
| Stage 2 (firmware load) | 2 days | 4 days | 1 week |
| Stage 3 (M2M scaffolding) | 3 days | 1 week | 2 weeks |
| Stage 4 — Option C (CCM/gamma/scaler) | 3 days | 1 week | 2 weeks |
| Stage 4 — Option B (full HAL port) | 3 weeks | 6 weeks | 10 weeks+ |
| Stage 4 — Option A (RE from scratch) | 6 weeks | 10 weeks | 16 weeks+ |
| **Path B with Option C** | **9 days** | **~3 weeks** | **~5 weeks** |
| **Path B with Option B** | **4 weeks** | **8 weeks** | **3 months+** |

Add ~30 % buffer for kernel-side debugging without JTAG.


## 9. Risk register

* **Power/clock sequence wrong** → silicon hang on `clk_prepare_enable`.
  Mitigation: lift exact sequence from `msm_cpp_soc.c`.
* **SMMU context bank conflict with VFE** → we'd see translation
  faults in DMA. Mitigation: clean separation, distinct SID per CB.
* **CPP firmware boot fails silently** → no IRQ, no error. Mitigation:
  add a 1-second timeout + dump first 256 bytes of RX FIFO.
* **HW version mismatch** → msm8953's CPP reports 4.0.0; firmware
  blobs are versioned. Need to pair `cpp_firmware_v1_8_0.fw` with
  this HW. Mitigation: log version on boot, fail loudly.
* **CPP requires bus voting we don't implement** → interconnect
  framework absence means CPP runs at lowest bus rate, stalls on
  big frames. Mitigation: model bus paths via `interconnect` driver
  which msm8953 already has support for.
* **Project scope creep**: once CPP is up, "while we're here..."
  pulls in JPEG hardware (also on `qcom,jpeg@1b1c000`), FD, etc.
  Mitigation: stop at CPP done + commit.


## 10. Decision checkpoints

Three explicit go/no-go points to revisit:

1. **After Stage 1**: if we can't even get CPP HW_VERSION to read
   back, abandon — likely a clock/regulator detail we haven't seen.
   Wasted: ~2 days.
2. **After Stage 2**: if firmware doesn't boot, the path forks into
   "find a different firmware version" or "give up". Wasted: ~6 days.
3. **After Stage 3 passthrough capture**: if M2M works but the
   output doesn't match input (geometry / colour wrong), we're
   debugging the command stream. At this point we **know** whether
   Option C is enough. Wasted: ~3 weeks if we pivot.


## 11. Alternative paths to revisit before committing

* **GPU compute (OpenCL on Adreno 506)** is a closer cousin of Path
  B in spirit — also gets us hardware acceleration — but with a
  Mesa/freedreno/clover dependency and no proprietary firmware. ~2
  weeks of work but in *open-source* land.
* **Stay on the Cortex-A53 NEON path and optimize cam-snap further**:
  better demosaic (AHD or DCB), spatial denoise, multi-frame fusion.
  ~1-2 weeks. Gets us close to phone-class for stills, never matches
  video.

**Recommendation**: if image quality is the goal and we're willing to
spend ~3 weeks, **Path B Option C** is the cleanest return on
investment — predictable scope, real HW acceleration, no proprietary
RE. If we want phone-quality colour even when scenes are tricky,
Option B is the long path.

If we want to keep velocity on `ex_camera` / streaming / the rest of
the Nerves system, **GPU compute via OpenCL on Adreno** gives us most
of the speed-up of CPP without the deep kernel work.
