#!/bin/sh
# fp3-cam-setup: configure the rear and/or front CAMSS pipeline.
# Idempotent — safe to re-run. media-ctl prints "Unable to parse link"
# if a link is already enabled; ignore those.
#
# Rear:  <rear sensor>  → csiphy0 → csid0 → ispif0 → vfe0_rdi0 → /dev/video0
# Front: <front sensor> → csiphy2 → csid1 → ispif1 → vfe0_rdi1 → /dev/video1
#
# The camera modules are user-replaceable and the Fairphone 3 and 3+ fit
# different silicon in the same slots, so neither the subdev entity name
# nor the geometry nor the Bayer order can be hardcoded: the rear slot is
# an IMX363 at 4032x3024 RGGB on one phone and an S5KGM1SP at 4000x3000
# GRBG on the other. Everything below is resolved from the media graph
# that drivers/misc/fp3_module_slot.c built at boot.
#
# Modes per sensor:
#   full   — native resolution. Used by cam-snap for stills.
#   binned — sensor 2x2 binned. Used by cam-stream for live video: 4x
#            less sensor->DRAM bandwidth and better SNR.
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

# Name of the sensor entity sitting on a given i2c bus-address, as the
# media graph reports it, e.g. "imx363 3-0010". Empty if that slot is
# not populated.
sensor_entity() {
	media-ctl -d "$MEDIA" -p 2>/dev/null |
		sed -n "s/^- entity [0-9]*: \([^(]*$1\) (.*/\1/p" |
		sed 's/[[:space:]]*$//' | head -1
}

# Geometry and Bayer order for a sensor, keyed on the entity name.
# Sets $full, $binned and $code. Returns 1 for a sensor we do not know,
# which is a louder failure than silently configuring the wrong format.
sensor_profile() {
	case "$1" in
	imx363\ *)   full=4032x3024; binned=2016x1512; code=SRGGB10_1X10; bayer=rggb ;;
	s5kgm1sp\ *) full=4000x3000; binned=2000x1500; code=SGRBG10_1X10; bayer=grbg ;;
	s5k4h7yx\ *) full=3264x2448; binned=1440x1080; code=SGRBG10_1X10; bayer=grbg ;;
	s5k3p9sp\ *) full=4608x3456; binned=2304x1728; code=SGRBG10_1X10; bayer=grbg ;;
	*) return 1 ;;
	esac
	return 0
}

# /dev/v4l-subdevN backing an entity. The numbering is not stable — it
# depends on how many subdevs registered, and the FP3 has two more than
# the FP3+ because its rear module declares flash LEDs — so it has to be
# read back rather than assumed.
entity_devnode() {
	media-ctl -d "$MEDIA" -p 2>/dev/null |
		awk -v pat="$1" '
			index($0, "- entity ") == 1 && index($0, pat) { found = 1; next }
			found && /device node name/ { print $NF; exit }
		'
}

# The lens, if the fitted module has one: AK7375 on the Fairphone 3,
# DW9800W on the Fairphone 3+. Matched on subdev subtype so a new VCM
# does not need a new name here.
lens_devnode() {
	media-ctl -d "$MEDIA" -p 2>/dev/null |
		awk '/subtype Lens/ { found = 1 }
		     found && /device node name/ { print $NF; exit }'
}

# $1 = "rear" | "front", $2 = 1 for binned
setup_cam() {
	which="$1"; want_binned="$2"

	if [ "$which" = rear ]; then
		addr=3-0010; phy=msm_csiphy0; csid=msm_csid0
		ispif=msm_ispif0; rdi=msm_vfe0_rdi0; video=/dev/video0
	else
		addr=4-0010; phy=msm_csiphy2; csid=msm_csid1
		ispif=msm_ispif1; rdi=msm_vfe0_rdi1; video=/dev/video1
	fi

	entity=$(sensor_entity "$addr")
	if [ -z "$entity" ]; then
		echo "fp3-cam-setup: no $which sensor in the media graph" >&2
		return 1
	fi
	if ! sensor_profile "$entity"; then
		echo "fp3-cam-setup: unknown $which sensor '$entity'" >&2
		return 1
	fi

	if [ "$want_binned" = 1 ]; then size="$binned"; else size="$full"; fi

	mc_link "\"${phy}\":1->\"${csid}\":0[1]"
	mc_link "\"${csid}\":1->\"${ispif}\":0[1]"
	mc_link "\"${ispif}\":1->\"${rdi}\":0[1]"

	for p in "\"${entity}\":0" \
	         "\"${phy}\":0"   "\"${phy}\":1" \
	         "\"${csid}\":0"  "\"${csid}\":1" \
	         "\"${ispif}\":0" "\"${ispif}\":1" \
	         "\"${rdi}\":0"   "\"${rdi}\":1"; do
		mc_fmt "${p}[fmt:${code}/${size}]"
	done

	# Publish what was found, so the capture tools do not have to
	# rediscover it — and, more to the point, do not have to guess. A
	# still captured with the wrong geometry or Bayer order is not a
	# worse picture, it is -EPIPE out of VIDIOC_STREAMON, because CAMSS
	# checks the video node's format against the pipeline.
	mkdir -p /run
	{
		echo "SENSOR='${entity}'"
		echo "VIDEO=${video}"
		echo "SUBDEV=$(entity_devnode "${addr} (")"
		[ "$which" = rear ] && echo "LENS=$(lens_devnode)"
		echo "WIDTH=${size%x*}"
		echo "HEIGHT=${size#*x}"
		echo "BAYER=${bayer}"
	} > "/run/fp3-cam-${which}.conf"

	echo "fp3-cam-setup: $which ready — $entity ${size} ${code}"
	return 0
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

if [ -n "$cam" ]; then
	setup_cam "$cam" "$binned"
	exit $?
fi

# No camera specified — set both, and only fail if neither came up, so a
# phone with one module missing still gets the other.
setup_cam rear  "$binned"; rear_rc=$?
setup_cam front "$binned"; front_rc=$?
[ "$rear_rc" = 0 ] || [ "$front_rc" = 0 ]
