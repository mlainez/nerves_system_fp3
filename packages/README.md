# Buildroot packages

Every package's `Config.in` title carries a scope tag indicating how
hardware-specific it is.

| Tag | Meaning |
|---|---|
| `[FP3+ only]` | Hardware-bound to the Fairphone 3+ — uses its specific sensors, regulators, DTS nodes, or firmware blobs. Won't compile cleanly or run on other boards. |
| `[Qualcomm generic]` | Works on any Qualcomm SoC that ships the relevant kernel subsystem (QRTR, remoteproc, ADSP). Useful on FP3+, dragonboard, msm8x16-aimed devices, etc. |
| `[Generic]` | No hardware dependency. Could move upstream to `nerves_system_br`. |

## Current packages

| Package | Tag | Notes |
|---|---|---|
| `fp3-camera-utils` | `[FP3+ only]` | cam-snap / cam-stream / cam-grab / i2cprobe — msm8953 CAMSS + s5kgm1sp/s5k3p9sp sensors. |
| `fp3-firmware` | `[FP3+ only]` | Proprietary firmware blobs (modem, ADSP, WCNSS) extracted from the FP3+ vendor partition. |
| `citronics-initramfs` | `[FP3+ only]` | Recovery initramfs — board-specific partition layout & USB DHCP setup. |
| `qrtr` | `[Qualcomm generic]` | Userspace reference for net/qrtr (IPC Router). |
| `rmtfs` | `[Qualcomm generic]` | Remote filesystem service for modem / ADSP firmware storage. |
| `tqftpserv` | `[Qualcomm generic]` | TFTP-like server that ships firmware files to remoteprocs over QRTR. |
| `hexagonrpc` | `[Qualcomm generic]` | FastRPC userspace (`hexagonrpcd`, `chrecd`, `libhexagonrpc`). |
| `reboot-mode` | `[Qualcomm generic]` | Reboots into `bootloader`/`recovery`/`edl` via `LINUX_REBOOT_CMD_RESTART2`. |
| `unudhcpd` | `[Generic]` | Tiny single-IP DHCP server. Used by the recovery initramfs. |

## Adding a package

Pick the most restrictive tag that still applies:
- If the code references `/dev/video0` or a specific sensor compatible
  string → `[FP3+ only]`.
- If it talks to a generic Qualcomm kernel subsystem → `[Qualcomm generic]`.
- If it has no hardware assumption → `[Generic]`, and consider whether
  it belongs upstream in `nerves_system_br` instead of here.

Long-term, the `[Qualcomm generic]` set is a candidate for moving into
a parent `nerves_system_qcom_msm8953` once a second board needs them.
