#!/bin/bash
set -e
in=../hero-4k.mp4
for spec in 720:1280:1600k 1080:1920:3200k 1440:2560:5500k; do
  IFS=: read h w rate <<< "$spec"
  ffmpeg -nostdin -loglevel error -y -i $in -vf "scale=$w:$h:flags=lanczos" -an -c:v libx264 -preset slow -profile:v high \
    -b:v $rate -maxrate $rate -bufsize $rate -g 60 -keyint_min 60 -sc_threshold 0 -pix_fmt yuv420p -movflags +faststart hero-$h.mp4
  ffmpeg -nostdin -loglevel error -y -i $in -vf "scale=$w:$h:flags=lanczos" -an -c:v libvpx-vp9 -b:v $rate -maxrate $rate \
    -g 60 -row-mt 1 -cpu-used 2 -pass 1 -f null /dev/null
  ffmpeg -nostdin -loglevel error -y -i $in -vf "scale=$w:$h:flags=lanczos" -an -c:v libvpx-vp9 -b:v $rate -maxrate $rate \
    -g 60 -row-mt 1 -cpu-used 2 -pass 2 hero-$h.webm
done
ffmpeg -nostdin -loglevel error -y -i $in -frames:v 1 -vf "scale=1920:1080:flags=lanczos" poster.png
ls -la | awk '{print $5, $9}'
