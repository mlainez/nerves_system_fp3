# Fairphone 3 / 3+

Nerves System configuration for the **Fairphone 3** and **Fairphone
3+** (both Qualcomm Snapdragon 632 / MSM8953, aarch64).

A single firmware image works on both boards: the kernel build ships
`sdm632-fairphone-fp3.dtb` *and* `sdm632-fairphone-fp3p.dtb` in the
boot partition, and `extlinux.conf` carries one labelled menu entry
per device. The FP3+ label is the silent default; pick `Nerves
(Fairphone 3)` from lk2nd's menu (increase `timeout` in
`extlinux/extlinux.conf` if you need more than 1 s to choose) or
swap the `default`/`menu default` markers to flip it.

| Feature              | Description                                                   |
| -------------------- | ------------------------------------------------------------- |
| CPU                  | 8x Qualcomm Snapdragon 632 (Cortex-A53, aarch64)              |
| GPU                  | Qualcomm Adreno 506                                           |
| Memory               | 3 GB LPDDR3 RAM                                               |
| Storage              | eMMC, firmware lives on the Android `userdata` partition      |
| Linux kernel         | `mlainez/linux-msm8953`, branch `staging`                     |
| Bootloader           | lk2nd-msm8953 → extlinux/extlinux.conf on the rootfs          |
| IEx terminal         | On-device display (`tty1`) or serial debug port (`ttyMSM0`)   |
| WiFi                 | wcnss / prima (firmware + driver loaded via remoteproc)       |
| Bluetooth            | wcnss BT (BlueZ userspace)                                    |
| Audio                | Loudspeaker via ADSP + TAS2557 / AW8898 amplifier firmware    |
| Sensors              | ADSP sensor stack (sns.reg + `qcom_sns_reg`)                  |
| Modem                | 2G/3G/LTE dual SIM via QMI + `vintage_net_qmi`                |

## Acknowledgements

This system is a port of the
[citronics buildroot-fp2 external tree](https://github.com/Citronics/buildroot-fp2)
(despite the name, that tree carries the working Fairphone 3 board
files), shaped to match the Nerves
[porting guide](https://github.com/nerves-project/nerves/blob/main/guides/advanced/porting-guide.md).
The firmware blobs and kernel-config additions come from the citronics
port; the `/etc/init.d/Sxx` userspace bring-up has been replaced by a
collection of small, focused OTP applications.

## Using

In your Nerves application's `mix.exs`:

```elixir
defp deps do
  [
    {:nerves_system_fp3,
      git: "https://github.com/Spin42/nerves_system_fp3",
      runtime: false, targets: :fp3, nerves: [compile: true]},

    # Native daemons (one small OTP app each).
    {:ex_rmtfs, github: "mlainez/ex_rmtfs", targets: :fp3},
    {:ex_tqftpserv, github: "mlainez/ex_tqftpserv", targets: :fp3},
    {:ex_hexagonrpcd, github: "mlainez/ex_hexagonrpcd", targets: :fp3},

    # FP3-specific bring-up bits.
    {:ex_hexagonfs, github: "mlainez/ex_hexagonfs", targets: :fp3},

    # Cellular networking — same stack the FP2 system uses.
    {:vintage_net_qmi, github: "mlainez/vintage_net_qmi", targets: :fp3}
  ]
end

def application do
  [
    extra_applications: [
      :logger,
      :ex_hexagonfs,
      :ex_rmtfs,
      :ex_tqftpserv,
      :ex_hexagonrpcd
    ],
    mod: {MyApp.Application, []}
  ]
end
```

Set `MIX_TARGET=fp3` and you're off.

The `ex_*` OTP apps each handle one slice of the bring-up and log a
warning on bad hardware instead of crashing — order them in
`extra_applications` how you like, but the listing above mirrors the
buildroot port's S20→S39 ordering.

## Configuration

```elixir
# config/target.exs
config :vintage_net_qmi, ...   # your APN / service-provider config
```

`ex_hexagonfs` and `ex_hexagonrpcd` default to the FP3 layout so they
need no config on this board. The Qualcomm QRTR / remoteproc /
fastrpc / sns-reg / sysmon / pd-mapper modules are all `=y` in the
kernel config — nothing to modprobe.

## Building the firmware

The Nerves system is consumed like any other — by a Nerves
application that targets it. The shortest path from this checkout
to a flashable image:

```bash
# 1. Prerequisites (one-off, see https://hexdocs.pm/nerves/installation.html
#    for your platform; you need erlang + elixir + fwup + ssh-askpass +
#    squashfs-tools + cmake + autoconf + bc + libssl-dev).
mix archive.install hex nerves_bootstrap

# 2. Create a throw-away Nerves app next to nerves_system_fp3 (skip if
#    you already have one).
cd /var/home/marc/Projects
mix nerves.new fp3_demo --target fp3
cd fp3_demo
```

Edit `fp3_demo/mix.exs` so the system and the runtime libs are path
deps pointing at your checkouts:

```elixir
@all_targets [:fp3]

defp deps do
  [
    {:nerves, "~> 1.11", runtime: false},
    {:shoehorn, "~> 0.9"},
    {:ring_logger, "~> 0.11"},
    {:toolshed, "~> 0.4"},

    {:nerves_system_fp3,
      path: "../nerves_system_fp3",
      runtime: false, targets: :fp3, nerves: [compile: true]},

    {:ex_rmtfs,        path: "../ex_rmtfs",        targets: :fp3},
    {:ex_tqftpserv,    path: "../ex_tqftpserv",    targets: :fp3},
    {:ex_hexagonrpcd,  path: "../ex_hexagonrpcd",  targets: :fp3},
    {:ex_hexagonfs,    path: "../ex_hexagonfs",    targets: :fp3},
    {:vintage_net_qmi, path: "../vintage_net_qmi", targets: :fp3}
  ]
end

def application do
  [
    extra_applications: [
      :logger, :ex_hexagonfs, :ex_rmtfs, :ex_tqftpserv, :ex_hexagonrpcd
    ],
    mod: {FP3Demo.Application, []}
  ]
end
```

Then build:

```bash
# 3. Set the target. `:fp3` matches the `targets:` keys above.
export MIX_TARGET=fp3

# 4. Pull deps. This downloads the aarch64 Nerves toolchain
#    (~250 MB), the citronics buildroot sources and the
#    mlainez/linux-msm8953 staging kernel, then runs Buildroot. First
#    build is 30–60 min; later rebuilds are incremental.
mix deps.get
mix compile

# 5. Build the .fw firmware archive.
mix firmware

# 6. Convert it to a raw .img that fastboot can flash into userdata.
mix firmware.image
```

Useful side-channels:

- `mix firmware.unpack` — peek inside the .fw
- `mix nerves.system.shell` — drop into the system's buildroot
  build directory with a properly-set environment, so you can run
  `make menuconfig`, `make linux-menuconfig`, etc. against this
  exact tree
- `MIX_DEBUG=1 mix compile` — see every Buildroot command
- `rm -rf _build/fp3_*` then `mix deps.compile nerves_system_fp3
  --force` — nuke and rebuild just the system after a defconfig
  tweak

## Flashing for the first time

The stock Fairphone 3 bootloader refuses to chain-load a foreign
kernel — flash `lk2nd-msm8953` over `boot` first, then push your
Nerves firmware into `userdata`:

```bash
# Boot the FP3 into fastboot (Vol Down + Power), USB-attached.
fastboot flashing unlock                    # once per device
fastboot flash boot lk2nd-msm8953.img       # second-stage bootloader
fastboot flash userdata fp3_demo.img        # the file `mix firmware.image` produced
fastboot reboot
```

## Partition layout

The Fairphone 3 bootloader will not let us add new partitions to the
eMMC, so the entire Nerves layout is *nested* inside the Android
`userdata` partition (`/dev/mmcblk0p62`). The citronics initramfs
runs `kpartx -asf /dev/mmcblk0p62` very early during boot, exposing
subpartitions as `/dev/mmcblk0p62p1` … `/dev/mmcblk0p62p3`.

```
/dev/mmcblk0p62   ─── flashed via `fastboot flash userdata fp3_demo.img` ───
├─ MBR (block 0)
├─ uboot env       (Nerves firmware metadata, 8 KiB)
├─ Boot A          (ext2, 50 MiB — kernel + dtb + initramfs + extlinux)
├─ Boot B          (ext2, 50 MiB)
├─ Rootfs A        (squashfs, 250 MiB)
├─ Rootfs B        (squashfs, 250 MiB)
└─ Application     (f2fs, expands to fill the partition, mounted at /root)
```

lk2nd-msm8953 reads `/extlinux/extlinux.conf` from the active boot
subpartition and loads `Image` + `sdm632-fairphone-fp3.dtb` +
`initramfs.gz`. The kernel command line passes `rootfs=` /
`bootpart=` to the initramfs, which kpartx-maps the parent device,
mounts the rootfs subpartition read-only at `/sysroot/`, mounts the
boot subpartition at `/sysroot/boot`, and `switch_root`s into it.

`erlinit` mounts:

- `/dev/mmcblk0p62p1` → `/boot` (ext2, ro)
- `/dev/mmcblk0p62p3` → `/root` (f2fs, application data)
- `/dev/mmcblk0p32`, `p34`, `p13` → `/mnt/{vendor,persist,dsp}` (Android
  partitions used by `ex_hexagonfs`).

The rootfs also ships a `/usr/share/qcom → /run/qcom` symlink so
`hexagonrpcd`'s compatible-string auto-discovery finds the populated
HexagonFS tree.

## Loudspeaker

TAS2557 / AW8898 firmware blobs come from the `fp3-firmware`
buildroot package; ALSA tooling is built in. After boot:

```
aplay -D plughw:0,0 /usr/share/sounds/hello.wav
```

or from Elixir:

```elixir
System.cmd("aplay", ["-D", "plughw:0,0", path])
```

## Modem and Wi-Fi

The kernel auto-boots both the ADSP and the modem (MSS) remoteprocs;
no userspace nudge is needed. Once they're up, `vintage_net_qmi`
handles UIM provisioning, the data session and DHCP exactly as on
the FP2:

```elixir
VintageNetQMI.quick_configure("internet")
```

Wi-Fi is `wpa_supplicant` + VintageNet.

## Sensors

The Qualcomm sensor stack runs on the ADSP and surfaces through
`/sys/bus/iio/devices/iio:deviceN/`. Once the ADSP is alive:

```elixir
File.ls!("/sys/bus/iio/devices")
|> Enum.filter(&String.starts_with?(&1, "iio:device"))
```

## UART

`ttyMSM0` is available but requires opening the phone and soldering
wires to the motherboard pads. Swap the `-c` line in `erlinit.config`
to redirect the IEx prompt there.

## Known gaps / next steps

- Both the ADSP and modem (MSS) rely on the kernel's
  `auto_boot=true`. If a future kernel disables that, package an
  `ex_adsp_boot` / `ex_modem_boot` OTP app that writes `start` to
  the relevant `/sys/class/remoteproc/<rproc>/state`.
- GPS XTRA-download is not wired up; add it as another small `ex_*`
  OTP app if needed.
- The deviceinfo USB IDs (`0x18d1` / `0xd001`, generic Google) are
  fine for fastboot-style hosts but a real product should use its
  own assigned VID/PID. Edit `packages/citronics-initramfs/deviceinfo`.
