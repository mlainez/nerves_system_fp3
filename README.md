# Nerves System: Fairphone 3 / 3+

Nerves system for the **Fairphone 3** and **Fairphone 3+** — Qualcomm
Snapdragon 632 (MSM8953), aarch64. One firmware image runs on both.

| Feature      | Description                                                 |
| ------------ | ----------------------------------------------------------- |
| CPU          | Snapdragon 632: 4× Kryo 250 Gold + 4× Kryo 250 Silver, aarch64 |
| GPU          | Adreno 506 — OpenGL ES (Mesa Freedreno)                     |
| Memory       | 4 GB LPDDR3, 64 GB eMMC                                     |
| Storage      | eMMC; firmware lives inside the Android `userdata` partition|
| Linux        | `mlainez/linux-msm8953`, 6.19                               |
| Bootloader   | lk2nd-msm8953 → `extlinux/extlinux.conf` on the boot partition |
| Console      | On-device display (`tty1`) or serial debug pads (`ttyMSM0`) |
| Cellular     | 2G/3G/LTE dual SIM over QMI                                 |
| Wi-Fi / BT   | wcnss/prima via remoteproc, BlueZ userspace                 |
| NFC          | Yes                                                         |
| GNSS         | Yes                                                         |
| Camera       | Rear + front: stills and live H.264 (Venus) through CAMSS   |
| Audio        | Loudspeaker via ADSP                                        |
| Sensors      | Qualcomm ADSP sensor stack, exposed through IIO             |

## One image, two phones

The Fairphone 3 is built to be taken apart. Its rear camera, front camera and
loudspeaker are user-replaceable with a #00 screwdriver, and the Fairphone 3+
upgrade kit fits different silicon in each:

| Slot         | Fairphone 3            | Fairphone 3+            |
| ------------ | ---------------------- | ----------------------- |
| Rear camera  | Sony IMX363 (12MP)     | Samsung S5KGM1SP (48MP) |
| Rear AF      | AK7374                 | DW9800W                 |
| Front camera | Samsung S5K4H7YX (8MP) | Samsung S5K3P9SP (16MP) |
| Loudspeaker  | Awinic AW8898          | TI TAS2557              |

Both models share one mainboard and report identical `qcom,msm-id = <349 0>`
and `qcom,board-id = <8 0x10000>`, so nothing before boot can tell them
apart — and because the modules are sold separately, a given phone can carry
any mix of the two generations.

So this system ships **one device tree that describes the slots rather than
their contents**. `sdm632-fairphone-fp3.dts` carries only what is soldered
down: which I²C bus each module hangs off, its regulators, its reference
clock and its enable line. At boot, `drivers/misc/fp3_module_slot.c` powers
each slot up, reads the chip's ID register, and applies a device tree overlay
describing what it found. CAMSS and the sound card stay `disabled` until
then, so they only ever probe against hardware that is actually present. The
stock, unmodified sensor and codec drivers bind normally afterwards.

There is no per-variant image, no boot menu and no reboot. `dmesg` reports
what was found:

```
fp3-module-slots: rear-camera slot: Samsung S5KGM1SP (48MP, Fairphone 3+)
fp3-module-slots: front-camera slot: Samsung S5K3P9SP (16MP, Fairphone 3+)
fp3-module-slots: speaker-amp slot: TI TAS2557 (Fairphone 3+)
```

A slot whose module is missing or unrecognised logs a warning and is skipped;
the rest of the phone still boots.

## Using

In your Nerves application's `mix.exs`, add the system under a target
name of your choice (here `:fp3`):

```elixir
defp deps do
  [
    {:nerves_system_fp3,
      github: "mlainez/nerves_system_fp3",
      runtime: false, targets: :fp3, nerves: [compile: true]},

    # Qualcomm bring-up libraries, as needed (see the table below).
    {:ex_rmtfs,      github: "mlainez/ex_rmtfs",      targets: :fp3},
    {:ex_remoteproc, github: "mlainez/ex_remoteproc", targets: :fp3},
    {:ex_qbootctl,   github: "mlainez/ex_qbootctl",   targets: :fp3}
  ]
end
```

Then set `MIX_TARGET=fp3`.
[`nerves_livebook_fp3`](https://github.com/mlainez/nerves_livebook_fp3) is a
complete firmware built on this system (it uses the target name
`:nerves_system_fp3`).

The Qualcomm bring-up that a Buildroot port would do from `/etc/init.d/S*` is
split into small OTP applications instead, each handling one slice. A
missing device or daemon is logged and retried rather than crashing the
node. See each library's README for how it starts and what it needs:

| Library | What it needs from this system |
| --- | --- |
| [`ex_rmtfs`](https://github.com/mlainez/ex_rmtfs) | `rmtfs`; QRTR |
| [`ex_tqftpserv`](https://github.com/mlainez/ex_tqftpserv) | `tqftpserv`; QRTR |
| [`ex_hexagonfs`](https://github.com/mlainez/ex_hexagonfs) | ACDB/DSP blobs at `/mnt/vendor`, `/mnt/dsp` |
| [`ex_hexagonrpcd`](https://github.com/mlainez/ex_hexagonrpcd) | `hexagonrpcd`; `/dev/fastrpc-adsp` |
| [`ex_remoteproc`](https://github.com/mlainez/ex_remoteproc) | `/sys/class/remoteproc`; firmware in `/lib/firmware` |
| [`ex_qcom_smgr`](https://github.com/mlainez/ex_qcom_smgr) | `qcom_smgr`; `/sys/bus/iio/devices` |
| [`ex_audio`](https://github.com/mlainez/ex_audio) | `amixer`; ADSP up before the card binds |
| [`ex_nfc`](https://github.com/mlainez/ex_nfc) | `NETLINK_GENERIC` + the kernel `nfc` family |
| [`ex_location`](https://github.com/mlainez/ex_location) | QRTR; modem MSS running |
| [`fp3_camera`](https://github.com/mlainez/fp3_camera) | GStreamer `libcamerasrc`, `v4l2h264enc` (Venus), `tcpserversink` |
| [`fp3_modem`](https://github.com/mlainez/fp3_modem) | QRTR + the IPA data path |
| [`ex_qbootctl`](https://github.com/mlainez/ex_qbootctl) | `/usr/bin/qbootctl` |
| [`nerves_data_resize`](https://github.com/mlainez/nerves_data_resize) | `resize.f2fs`, `df`, `mount`, `umount` |

FastRPC and remoteproc are built into the kernel. QRTR, pd-mapper,
rmtfs-mem and the sensor drivers are loadable modules that udevd loads
from their modaliases; `ex_rmtfs` starts udevd and triggers the initial
coldplug, so it has to run on any firmware that uses this hardware.

## Building

You need the Nerves toolchain prerequisites for your platform (see the
[Nerves installation guide](https://hexdocs.pm/nerves/installation.html)):
Erlang, Elixir, `fwup`, `squashfs-tools`, `cmake`, `autoconf`, `bc` and
`libssl-dev`.

```bash
mix archive.install hex nerves_bootstrap

# A throw-away app to build against. nerves.new only knows the official
# targets, so generate a default project and add the system dependency
# shown under "Using" by hand.
mix nerves.new fp3_demo
cd fp3_demo

export MIX_TARGET=fp3
mix deps.get      # pulls the toolchain (~250 MB), Buildroot and the kernel
mix firmware      # first build is 30–60 min; rebuilds are incremental
mix firmware.image # raw .img for fastboot
```

Useful side channels:

- `mix nerves.system.shell` — a shell in the Buildroot build directory with
  the environment set, for `make menuconfig` / `make linux-menuconfig`
- `MIX_DEBUG=1 mix compile` — every Buildroot command
- `rm -rf _build/fp3_* && mix deps.compile nerves_system_fp3 --force` —
  rebuild the system after a defconfig change

### Working on the kernel

`nerves_defconfig` pins the kernel to an exact commit so builds are
reproducible. To build from a local checkout instead, copy `local.mk.example`
to `local.mk` and point `LINUX_OVERRIDE_SRCDIR` at your tree. `local.mk` is
gitignored, and Buildroot `-include`s it, so its absence is the normal case.
Remember that an override silently replaces the pinned source — firmware
built that way is not reproducible by anyone else.

## Flashing

The stock Fairphone 3 bootloader will not chain-load a foreign kernel, so
[lk2nd](https://github.com/msm8916-mainline/lk2nd) goes on `boot` and the
Nerves firmware into `userdata`. The stock bootloader also applies the
device-tree overlays in `dtbo` to whatever it boots, and Android's
overlays don't match lk2nd, so `dtbo` gets the dummy overlay from
[z3ntu/dtbo-fp3](https://github.com/z3ntu/dtbo-fp3/releases).

Enable **Developer options → OEM unlocking** in Android first, then:

```bash
# Boot into fastboot: hold Volume Down while powering on, USB attached.
fastboot flashing unlock              # once per device, erases the phone
fastboot set_active a
fastboot flash dtbo dtbo.img          # dummy overlay from z3ntu/dtbo-fp3
fastboot flash boot lk2nd-msm8953.img # from the lk2nd releases
fastboot flash userdata fp3_demo.img  # from `mix firmware.image`
fastboot reboot
```

After that, `mix upload` works over the network as usual. Holding
Volume Down at power-on now opens lk2nd's fastboot mode (wait until the
screen turns on before pressing it), from which `userdata` can be
reflashed.

## Partition layout

The Fairphone 3 bootloader will not let us add eMMC partitions, so the whole
Nerves layout is nested *inside* the Android `userdata` partition
(`/dev/mmcblk0p62`). The citronics initramfs runs `kpartx -asf` on it very
early, exposing `/dev/mmcblk0p62p1` … `p3`.

```
/dev/mmcblk0p62   ── flashed with `fastboot flash userdata` ──
├─ MBR (block 0)
├─ uboot env       (Nerves firmware metadata, 8 KiB)
├─ Boot A          (ext2, 100 MiB — kernel + dtb + initramfs + extlinux)
├─ Boot B          (ext2, 100 MiB)
├─ Rootfs A        (squashfs, 250 MiB)
├─ Rootfs B        (squashfs, 250 MiB)
└─ Application     (f2fs, fills the rest, mounted at /root)
```

lk2nd reads `/extlinux/extlinux.conf` from the active boot subpartition and
loads `Image`, `sdm632-fairphone-fp3.dtb` and `initramfs.gz`. The kernel
command line passes `rootfs=` and `bootpart=` to the initramfs, which maps
the subpartitions, mounts the rootfs read-only and `switch_root`s into it.

`erlinit` then mounts `/dev/mmcblk0p62p1` at `/boot`, `p3` at `/root`, and
the read-only Android partitions `p32`, `p34` and `p13` at `/mnt/vendor`,
`/mnt/persist` and `/mnt/dsp` for `ex_hexagonfs`.

## Notes on specific hardware

**Modem and Wi-Fi.** The kernel auto-boots the ADSP and modem remoteprocs; no
userspace nudge is needed. Cellular data goes through the QRTR branch
of `vintage_net_qmi` plus `Fp3Modem.PowerManager`; see the
[`fp3_modem`](https://github.com/mlainez/fp3_modem) README for a complete
VintageNet config. Wi-Fi is `wpa_supplicant` + VintageNet.

**Audio.** `aplay -D plughw:0,0 file.wav`. The amplifier firmware comes from
the `fp3-firmware` package.

**Camera.** libcamera's simple pipeline handler drives the fitted sensors
through CAMSS, its software ISP demosaics on the CPU — the msm8953 CPP
hardware ISP is not driven — and Venus encodes H.264. The
[`fp3_camera`](https://github.com/mlainez/fp3_camera) library builds
GStreamer pipelines from them. Full-resolution frames need the 256 MB CMA
area set on the kernel command line. `cam-snap`, `cam-stream` and
`cam-grab` from `fp3-camera-utils` still capture stills, H.264 and raw
Bayer without libcamera. Streams are raw H.264 over TCP, so a browser
cannot open them; use `ffplay tcp://…`.

**Sensors.** The Qualcomm stack runs on the ADSP and surfaces under
`/sys/bus/iio/devices/`.

**LEDs and vibrator.** The RGB notification LED and the two white flash
LEDs are under `/sys/class/leds` (write `brightness`; the RGB LED also
takes `multi_intensity`). The RGB LED can also play a pattern on its own:
set `trigger` to `pattern` and write `hw_pattern` as steps of
`brightness ms brightness 0`, all of the same length, such as
`0 500 0 0 255 500 255 0` for a slow blink or a ramp of values for
breathing. The PMIC steps through it from its own memory, so the pattern
runs on through suspend to idle. The vibrator is a force-feedback input
device; `rumble 300` vibrates for 300 ms.

**Screen and touch.** The panel is a DRM device (`msm`) with fbdev
emulation on `/dev/fb0`; the HX83112B touchscreen and the volume and
power buttons are input devices. Scenic's `cairo-fb` driver
(`scenic_driver_local` with `SCENIC_LOCAL_TARGET=cairo-fb`) renders to
the framebuffer and reads the touchscreen and buttons.

**UART.** `ttyMSM0` exists but needs wires soldered to pads inside the phone.
Swap the `-c` line in `rootfs_overlay/etc/erlinit.config` to move the IEx
prompt there.

## Known gaps

- **No OpenCL.** ML workloads run on the CPU through
  [`nx_arm`](https://github.com/mlainez/nx_arm). Rusticl would also put clang
  and LLVM on the device (177 MB against a 250 MiB rootfs), and on a5xx it
  rejected `cl_khr_fp16` and hung the shader compiler on convolutions.
- Camera colour is close to Android's but not calibrated against a known
  target; streams centre-crop rather than scale, so they see about 30%
  less vertical field of view than a still from the same camera.
- GPS XTRA assistance data is not downloaded.
- The USB gadget IDs in `packages/citronics-initramfs/deviceinfo` are the
  generic Google ones; a product should use its own.

## Acknowledgements

Ported from the [citronics buildroot-fp2 external
tree](https://github.com/Citronics/buildroot-fp2) — which, despite the name,
carries the working Fairphone 3 board files — reshaped to follow the Nerves
[porting guide](https://github.com/nerves-project/nerves/blob/main/guides/advanced/porting-guide.md).
The kernel is a fork of
[msm8953-mainline/linux](https://github.com/msm8953-mainline/linux).

## License

Apache-2.0 — see [LICENSE](LICENSE). A built firmware image aggregates many
differently licensed components and downloads proprietary Qualcomm and
Fairphone firmware at build time; see [NOTICE](NOTICE).
