#!/bin/sh
# fp3-cam-setup: enable rear + front camera media pipelines on FP3+.
# Idempotent — safe to re-run. media-ctl prints "Unable to parse link"
# if a link is already enabled; ignore those.
#
# Rear:  s5kgm1sp → csiphy0 → csid0 → ispif0 → vfe0_rdi0 → /dev/video0
# Front: s5k3p9sp → csiphy2 → csid1 → ispif1 → vfe0_rdi1 → /dev/video1

set +e

MEDIA=/dev/media0

mc_link() {
	media-ctl -d "$MEDIA" -l "$1" >/dev/null 2>&1
}
mc_fmt() {
	media-ctl -d "$MEDIA" -V "$1" >/dev/null 2>&1
}

# Rear pipeline
mc_link '"msm_csiphy0":1->"msm_csid0":0[1]'
mc_link '"msm_csid0":1->"msm_ispif0":0[1]'
mc_link '"msm_ispif0":1->"msm_vfe0_rdi0":0[1]'
for p in '"s5kgm1sp 3-0010":0' \
         '"msm_csiphy0":0' '"msm_csiphy0":1' \
         '"msm_csid0":0'   '"msm_csid0":1' \
         '"msm_ispif0":0'  '"msm_ispif0":1' \
         '"msm_vfe0_rdi0":0' '"msm_vfe0_rdi0":1'; do
	mc_fmt "${p}[fmt:SGRBG10_1X10/4000x3000]"
done

# Front pipeline
mc_link '"msm_csiphy2":1->"msm_csid1":0[1]'
mc_link '"msm_csid1":1->"msm_ispif1":0[1]'
mc_link '"msm_ispif1":1->"msm_vfe0_rdi1":0[1]'
for p in '"s5k3p9sp 4-0010":0' \
         '"msm_csiphy2":0' '"msm_csiphy2":1' \
         '"msm_csid1":0'   '"msm_csid1":1' \
         '"msm_ispif1":0'  '"msm_ispif1":1' \
         '"msm_vfe0_rdi1":0' '"msm_vfe0_rdi1":1'; do
	mc_fmt "${p}[fmt:SGRBG10_1X10/4608x3456]"
done

echo "fp3-cam-setup: rear (/dev/video0) + front (/dev/video1) pipelines ready."
