#!/bin/zsh
# Records what a build shows on the shared Dreamcast: runs the ELF through enDjinn's console queue
# and captures the HDMI output (640x480, 30 fps, H.264) for SECONDS, from the moment the program
# starts. Pair two recordings with tools/dc_visual_compare.py.
#
#   gmake TARGET_DC=1 DC_60FPS=1 BUILD_DIR_BASE=build/vis/mine -j8
#   tools/dc_visual_run.zsh build/vis/mine/us_dc/sm64.us.f3dex2e [SECONDS=100]
#
# 100 s covers the title screen, the white fade, the star iris into the first attract demo and
# that demo. A plain build never exits, so once the recording ends the console gets a soft reset
# (kos-tool -r) while this script still holds the queue lock. The capture device is always free
# to use (Daniel, 2026-10-24). AFX_STAGE=<dir> copies <dir>/afx into /pc for DC_AICAFLOW builds.
setopt err_return
cd "${0:A:h}/.." || exit 1
source ~/projects/dreamcast/enDJinn/environ.sh
caffeinate -dimsu -w $$ &

elf=$1 secs=${2:-100}
[[ -f $elf ]] || { echo "missing $elf"; exit 1; }
img_end=$(/opt/toolchains/dc/sh-elf/bin/sh-elf-nm $elf | awk '$3 == "_end" {print $1}')
if [[ -z $img_end ]] || (( 16#$img_end > 16#8cf00000 )); then
  echo "refusing $elf: _end=0x$img_end leaves under 1 MB free RAM"
  exit 1
fi
device="Live Gamer Ultra 2.1-Video"
ffmpeg -hide_banner -f avfoundation -list_devices true -i "" 2>&1 | grep -q "$device" ||
  { echo "capture device '$device' not connected"; exit 1; }

variant=${${elf:h:h}:t}
out=$PWD/build/hw/visual-$variant-$(date +%Y%m%d-%H%M%S)
mkdir -p $out
[[ -n $AFX_STAGE ]] && cp -R "$AFX_STAGE/afx" $out/
DCTOOL_LOCK_WAIT=${DCTOOL_LOCK_WAIT:-3600} dctool_lock || exit $?
trap 'dctool_unlock' EXIT
# After an earlier soft reset the loader can take a while to come back.
for i in {1..40}; do dctool_loader_ready "$DCTOOL_HOST" && break; sleep 5; done
dctool_loader_ready "$DCTOOL_HOST" || { echo "dc-load-ip not ready"; exit 1; }

kos-tool -f -t "$DCTOOL_HOST" -m "$out" -x "${elf:A}" > $out/console.log 2>&1 &
run=$!
# Start recording when the program reports in (the upload takes a few seconds).
for i in {1..120}; do grep -q "vid_set_mode" $out/console.log 2>/dev/null && break; sleep 1; done
ffmpeg -hide_banner -loglevel error -f avfoundation -framerate 60 -pixel_format uyvy422 -i "$device" \
  -t $secs -vf "scale=640:480,fps=30" -c:v libx264 -preset veryfast -crf 20 -y $out/video.mp4 || true
if kill -0 $run 2>/dev/null; then
  kos-tool -r -t "$DCTOOL_HOST" >> $out/console.log 2>&1 || true
  sleep 3
  kill $run 2>/dev/null || true
fi
wait $run 2>/dev/null || true
echo $out
