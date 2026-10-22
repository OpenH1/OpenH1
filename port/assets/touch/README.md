# The touch controls' art

The Android app's on-screen controls (`TouchControls.java`; "Touch
controls" in `port/android/README.md`) are drawn with:

- **The controller's buttons as the HUD draws them** (`port/assets/buttons`,
  credited in `AUTHORS.md`): A, B, X, Y, black and white in the style
  `display.hud_button_style` names (`classic`, `gems`, whose black and white
  are `menu_gems`', or `menu_gems`), as `hud_hires_tags.c` picks them; the
  triggers (`trigger_left`, `trigger_right`) and the stick clicks
  (`thumbstick_left`, `thumbstick_right`), the same in every style.
- **Kenney's Mobile Controls** (`kenney-mobile-controls/`, CC0), only where
  there is no redraw: the move stick's base and knob, the plates of start,
  back and the D-pad with their icons, the toolbar's plates and gear, and
  the ring of a plate held. Its README names the upstream, the version, the
  checksum and what uses each file.

`python tools/touch_assets.py` draws them all into
`port/android/app/src/main/res/drawable-nodpi/touch_*.png`: the buttons as
`tools/button_assets.py` draws the HUD's (its SVG redraws, recoloured for
`classic`, and the supplied PNGs of `gems`, the triggers and the sticks),
larger and cropped to the art, and Kenney's SVGs. Run it again after
changing a redraw or an SVG (`--check` tells whether the PNGs are up to
date).

`python tools/touch_preview.py --out port/android/docs` draws the default
layout over a picture as the app draws it, on a phone and a tablet, in each
style (`port/android/docs/touch-controls-*.png`).
