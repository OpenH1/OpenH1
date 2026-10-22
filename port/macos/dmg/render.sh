#!/bin/sh
# Renders background.html to background.png (640 x 400) and background@2x.png
# with headless Chrome (whose window keeps some height for itself: rendered
# taller, then cropped). Run from this folder; needs Chrome and Python's PIL.
set -e
chrome="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
for scale in 1 2; do
  out=background.png; [ "$scale" = 2 ] && out=background@2x.png
  "$chrome" --headless=new --disable-gpu --hide-scrollbars --allow-file-access-from-files \
    --force-device-scale-factor=$scale --virtual-time-budget=3000 --window-size=640,600 \
    --screenshot="$PWD/$out" "file://$PWD/background.html" 2> /dev/null
  python3 -c "import sys; from PIL import Image; s = int(sys.argv[2]); Image.open(sys.argv[1]).convert('RGB').crop((0, 0, 640 * s, 400 * s)).save(sys.argv[1], optimize=True)" "$out" "$scale"
done
