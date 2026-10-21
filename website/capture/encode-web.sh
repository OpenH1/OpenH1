#!/bin/bash
# encode-web.sh <master> <name> <crop w:h:x:y of the 3840x2880 master> <width>x<height>
# A final pair's web clips from its masters (final/out): cropped, then scaled,
# the Original by nearest neighbour (so a whole multiple of 640x480 keeps its
# pixels square), OpenH1 by Lanczos; MP4, WebM and the first frame as a poster.
set -eu
RUNS=${RUNS:-$HOME/halo-capture-runs}
master=$1 name=$2 crop=$3 size=$4
cd "$RUNS/final/out"; mkdir -p web
for side in original openh1; do
  [ $side = original ] && flags=neighbor || flags=lanczos
  vf="crop=$crop,scale=${size/x/:}:flags=$flags,format=yuv420p"
  ffmpeg -nostdin -loglevel error -y -i $master-$side.mp4 -vf "$vf" -an -c:v libx264 -crf ${MP4_CRF:-26} -profile:v high -g 60 -movflags +faststart web/$name-$side.mp4
  ffmpeg -nostdin -loglevel error -y -i $master-$side.mp4 -vf "$vf" -an -c:v libvpx-vp9 -crf ${WEBM_CRF:-38} -b:v 0 -row-mt 1 -g 60 web/$name-$side.webm
  ffmpeg -nostdin -loglevel error -y -i web/$name-$side.mp4 -frames:v 1 -c:v libwebp -quality 75 web/$name-$side-poster.webp
  printf '%s: ' $name-$side; for f in web/$name-$side.mp4 web/$name-$side.webm web/$name-$side-poster.webp; do printf '%s %sKB, ' ${f##*/} $(( $(stat -c%s $f) / 1024 )); done; echo
done
