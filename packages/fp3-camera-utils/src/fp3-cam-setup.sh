#!/bin/sh
# fp3-cam-setup: configure rear and/or front CAMSS pipelines on FP3+.
# Idempotent — safe to re-run. media-ctl prints "Unable to parse link"
# if a link is already enabled; ignore those.
#
# Rear:  s5kgm1sp → csiphy0 → csid0 → ispif0 → vfe0_rdi0 → /dev/video0
# Front: s5k3p9sp → csiphy2 → csid1 → ispif1 → vfe0_rdi1 → /dev/video1
#
# Modes per sensor:
#   full   — native resolution (rear 4000×3000, front 4608×3456). Used
#            by cam-snap for stills.
#   binned — sensor 2×2 binned (rear 2000×1500, front 2304×1728). Used
#            by cam-stream for live video — 4× less sensor→DRAM
#            bandwidth + better SNR.
#
# Usage:
#   fp3-cam-setup                  Set both cameras at full resolution
#                                  (Elixir manager calls this on boot).
#   fp3-cam-setup [--binned] front Set front only, optionally binned.
#   fp3-cam-setup [--binned] rear  Set rear only, optionally binned.

set +e

MEDIA=/dev/media0

mc_link() { media-ctl -d "$MEDIA" -l "$1" >/dev/null 2>&1; }
mc_fmt()  { media-ctl -d "$MEDIA" -V "$1" >/dev/null 2>&1; }

setup_rear() {
	w="$1"; h="$2"
	mc_link '"msm_csiphy0":1->"msm_csid0":0[1]'
	mc_link '"msm_csid0":1->"msm_ispif0":0[1]'
	mc_link '"msm_ispif0":1->"msm_vfe0_rdi0":0[1]'
	for p in '"s5kgm1sp 3-0010":0' \
	         '"msm_csiphy0":0' '"msm_csiphy0":1' \
	         '"msm_csid0":0'   '"msm_csid0":1' \
	         '"msm_ispif0":0'  '"msm_ispif0":1' \
	         '"msm_vfe0_rdi0":0' '"msm_vfe0_rdi0":1'; do
		mc_fmt "${p}[fmt:SGRBG10_1X10/${w}x${h}]"
	done
}

setup_front() {
	w="$1"; h="$2"
	mc_link '"msm_csiphy2":1->"msm_csid1":0[1]'
	mc_link '"msm_csid1":1->"msm_ispif1":0[1]'
	mc_link '"msm_ispif1":1->"msm_vfe0_rdi1":0[1]'
	for p in '"s5k3p9sp 4-0010":0' \
	         '"msm_csiphy2":0' '"msm_csiphy2":1' \
	         '"msm_csid1":0'   '"msm_csid1":1' \
	         '"msm_ispif1":0'  '"msm_ispif1":1' \
	         '"msm_vfe0_rdi1":0' '"msm_vfe0_rdi1":1'; do
		mc_fmt "${p}[fmt:SGRBG10_1X10/${w}x${h}]"
	done
}

binned=0
cam=""
for a in "$@"; do
	case "$a" in
		--binned) binned=1 ;;
		--full)   binned=0 ;;
		front|rear) cam="$a" ;;
		*) echo "fp3-cam-setup: unknown arg '$a'" >&2; exit 2 ;;
	esac
done

if [ "$cam" = "rear" ]; then
	if [ "$binned" = "1" ]; then setup_rear 2000 1500
	else                         setup_rear 4000 3000; fi
	echo "fp3-cam-setup: rear ready ($([ "$binned" = "1" ] && echo binned || echo full))"
elif [ "$cam" = "front" ]; then
	if [ "$binned" = "1" ]; then setup_front 2304 1728
	else                         setup_front 4608 3456; fi
	echo "fp3-cam-setup: front ready ($([ "$binned" = "1" ] && echo binned || echo full))"
else
	# No camera specified — set both at full (boot-time default).
	setup_rear  4000 3000
	setup_front 4608 3456
	echo "fp3-cam-setup: rear (/dev/video0) + front (/dev/video1) pipelines ready (full)."
fi
