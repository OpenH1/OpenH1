#!/bin/bash
# encode.sh <clip> <original start frame> <openh1 start frame> <seconds>
# Each side: <seconds> of frames, cut straight back to the first (FADE=0.4 crossfades the last 0.4 s into the first).
# WIDTH x HEIGHT is the clips' size (default 1280x960): the Original's 640x480 scaled up
# by nearest neighbour, which wants a whole multiple (3840x2880 is six times).
# MP4_CRF and WEBM_CRF set the quality.
set -eu
RUNS=${RUNS:-$HOME/halo-capture-runs} WIDTH=${WIDTH:-1280} HEIGHT=${HEIGHT:-960}
clip=$1 ostart=$2 hstart=$3 len=$4
cd "$RUNS"; mkdir -p out
fade=${FADE:-0}
for side in original openh1; do
  if [ $side = original ]; then
    input="-framerate 30 -start_number $ostart -i $clip-original/frames/frame%05d.bmp"
    count=$(python3 -c "print(round(($len+$fade)*30))"); prep="scale=$WIDTH:$HEIGHT:flags=neighbor,fps=60"
  else
    input="-framerate 60 -start_number $hstart -i $clip-openh1/frames/frame%05d.bmp"
    count=$(python3 -c "print(round(($len+$fade)*60))"); prep="fps=60"
  fi
  if [ "$fade" = 0 ]; then graph="[0]trim=end_frame=$count,$prep,format=yuv420p"; else
  graph="[0]trim=end_frame=$count,$prep,format=yuv444p,split[a][b];[a]trim=start=$fade,setpts=PTS-STARTPTS[body];[b]trim=end=$fade,setpts=PTS-STARTPTS[head];[body][head]xfade=transition=fade:duration=$fade:offset=$(python3 -c "print($len-$fade)"),format=yuv420p"; fi
  ffmpeg -loglevel error -y $input -filter_complex "$graph" -an -c:v libx264 -crf ${MP4_CRF:-26} -profile:v high -pix_fmt yuv420p -movflags +faststart out/$clip-$side.mp4
  ffmpeg -loglevel error -y $input -filter_complex "$graph" -an -c:v libvpx-vp9 -crf ${WEBM_CRF:-38} -b:v 0 -row-mt 1 out/$clip-$side.webm
  ffmpeg -loglevel error -y -i out/$clip-$side.mp4 -frames:v 1 -c:v libwebp -quality 75 out/$clip-$side-poster.webp
  printf '%s: ' $side; for f in out/$clip-$side.mp4 out/$clip-$side.webm out/$clip-$side-poster.webp; do printf '%s %sKB, ' ${f##*/} $(( $(stat -c%s $f) / 1024 )); done
  ffprobe -v error -count_frames -select_streams v -show_entries stream=width,height,nb_read_frames -of csv=p=0 out/$clip-$side.mp4
done
