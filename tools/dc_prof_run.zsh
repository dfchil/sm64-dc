#!/bin/zsh
# Runs DC_PROF builds on the shared Dreamcast through enDjinn's console queue
# and prints each run's per-scene report.
#
#   gmake TARGET_DC=1 DC_PROF=1 BUILD_DIR_BASE=build/prof/os_vbl2 -j10
#   tools/dc_prof_run.zsh build/prof/os_vbl2/us_dc/sm64.us.f3dex2e [another.elf ...]
#
# Each run gets build/hw/<variant>-<time>/, mounted as /pc on the console, so it
# holds console.log plus the sm64_prof.csv the build writes before it exits.
# The console is never restarted: if dc-load-ip does not answer, the run is
# skipped. The Mac is kept awake, since a sleeping host stalls dcload.
setopt err_return
cd "${0:A:h}/.." || exit 1
source ~/projects/dreamcast/enDJinn/environ.sh
caffeinate -dimsu -w $$ &

for elf in "$@"; do
  [[ -f $elf ]] || { echo "missing $elf"; continue; }
  # An image that leaves no room for the heap and stack dies before KOS prints anything, with no
  # way back to dcload: refuse anything ending within 1 MB of the 16 MB RAM top (0x8d000000).
  img_end=$(/opt/toolchains/dc/sh-elf/bin/sh-elf-nm $elf | awk '$3 == "_end" {print $1}')
  if [[ -z $img_end ]] || (( 16#$img_end > 16#8cf00000 )); then
    echo "refusing $elf: _end=0x$img_end leaves under 1 MB free RAM"
    continue
  fi
  variant=${${elf:h:h}:t}
  out=$PWD/build/hw/$variant-$(date +%Y%m%d-%H%M%S)
  mkdir -p $out
  # AFX_STAGE=<dir>: a DC_AICAFLOW build reads its assets from /pc/afx, so copy <dir>/afx into the run.
  [[ -n $AFX_STAGE ]] && cp -Rc "$AFX_STAGE/afx" $out/
  DCTOOL_LOCK_WAIT=3600 dctool_lock || exit $?
  for i in {1..10}; do dctool_loader_ready "$DCTOOL_HOST" && break; sleep 2; done
  if ! dctool_loader_ready "$DCTOOL_HOST"; then
    echo "dc-load-ip not ready; skipping $elf (no restart)"
    dctool_unlock
    continue
  fi
  # The build exits by itself after DC_AUTO_EXIT_SECONDS; the timeout is only a backstop.
  gtimeout -s KILL ${HW_TIMEOUT:-900} kos-tool -f -t "$DCTOOL_HOST" -m "$out" -x "${elf:A}" > $out/console.log 2>&1 &
  run=$!
  if [[ -n $CAPTURE ]]; then
    # CAPTURE=1: a 640x480 still from the HDMI capture every 8 s while our program runs.
    # The device is selected by name; avfoundation indices move.
    until grep -q "AICAFLOW: init\|AicaSynth_Init" $out/console.log 2>/dev/null || ! kill -0 $run 2>/dev/null; do sleep 1; done
    ffmpeg -hide_banner -loglevel error -f avfoundation -framerate 60 -video_size 1920x1080 \
      -pixel_format uyvy422 -i "Live Gamer Ultra 2.1-Video:none" \
      -vf "fps=1/8,scale=640:480" $out/cap_%03d.jpg < /dev/null &
    cap=$!
  fi
  if [[ -n $CAPTURE_VIDEO ]]; then
    # CAPTURE_VIDEO=1: the whole run as 640x480 30 fps video.mp4, to pick out short events (screen
    # transitions) afterwards, e.g. with ffmpeg's blackframe filter.
    until grep -q "AICAFLOW: init\|AicaSynth_Init" $out/console.log 2>/dev/null || ! kill -0 $run 2>/dev/null; do sleep 0.2; done
    ffmpeg -hide_banner -loglevel error -f avfoundation -framerate 60 -video_size 1920x1080 \
      -pixel_format uyvy422 -i "Live Gamer Ultra 2.1-Video:none" -t ${CAPTURE_VIDEO_SECONDS:-330} \
      -vf "fps=30,scale=640:480" -c:v h264_videotoolbox -b:v 4M $out/video.mp4 < /dev/null &
    vcap=$!
  fi
  if [[ -n $CAPTURE_AUDIO ]]; then
    # CAPTURE_AUDIO=1: the console's sound from the HDMI capture for the whole run, to audio.wav,
    # sample for sample (tools/dc_hdmi_audio.swift). The device has four channels, two of them copies.
    [[ build/dc_hdmi_audio -nt tools/dc_hdmi_audio.swift ]] || swiftc -O tools/dc_hdmi_audio.swift -o build/dc_hdmi_audio
    until grep -q "AICAFLOW: init\|AicaSynth_Init" $out/console.log 2>/dev/null || ! kill -0 $run 2>/dev/null; do sleep 0.2; done
    build/dc_hdmi_audio $out/audio4.wav &
    acap=$!
  fi
  wait $run || true
  # The capture stops with the program, so it never holds the lock.
  if [[ -n $acap ]]; then
    sleep 2
    kill -TERM $acap 2>/dev/null
    wait $acap 2>/dev/null || true
    unset acap
    ffmpeg -hide_banner -loglevel error -y -i $out/audio4.wav -af "pan=stereo|c0=c0|c1=c1" $out/audio.wav < /dev/null && rm $out/audio4.wav
  fi
  [[ -n $cap ]] && { kill -INT $cap 2>/dev/null; wait $cap 2>/dev/null || true; unset cap; }
  [[ -n $vcap ]] && { kill -INT $vcap 2>/dev/null; wait $vcap 2>/dev/null || true; unset vcap; }
  dctool_unlock
  echo "== $variant ($out)"
  python3 tools/dc_prof_report.py $out/console.log || echo "no DCPROF report in $out/console.log"
  if (( $# > 1 )); then sleep 20; fi  # Let other sessions have the console.
done
