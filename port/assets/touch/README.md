# The touch controls' art

The Android app's on-screen controls (`TouchControls.java`; "Touch
controls" in `port/android/README.md`) are drawn in one style, with nothing
behind them:

- **The controller's buttons as the HUD draws them** (`port/assets/buttons`,
  credited in `AUTHORS.md`): A, B, X, Y, black and white in the style
  `display.hud_button_style` names (`classic`, `gems`, whose black and white
  are `menu_gems`', or `menu_gems`), as `hud_hires_tags.c` picks them; the
  triggers (`trigger_left`, `trigger_right`) and the stick clicks
  (`thumbstick_left`, `thumbstick_right`, without the words LEFT and RIGHT
  over them), the same in every style. Each gets a thin light edge and a
  soft drop shadow.
- **Line icons** for the small controls, which have no redraw (the D-pad,
  start, back, and the toolbar's hide, show and options): the outlines of
  glyphs of Kenney's Mobile Controls (`kenney-mobile-controls/`, CC0), in
  the HUD's light blue, with the same soft shadow. Its README names the
  upstream, the version, the checksum and what uses each file.
- **The captions' font**: Overpass (`port/assets/fonts`, SIL Open Font
  License 1.1), the font of the HUD's text, copied into
  `res/font/touch_caption.ttf`, with its licence (`res/raw/touch_caption_ofl.txt`,
  which the APK carries).

The move stick's ring and knob are drawn by the app, not pictures.

`python tools/touch_assets.py` draws them all into
`port/android/app/src/main/res/drawable-nodpi/touch_*.png`: the buttons as
`tools/button_assets.py` draws the HUD's (its SVG redraws, recoloured for
`classic`, and the supplied PNGs of `gems`, the triggers and the sticks),
larger and cropped to the art, and Kenney's glyphs outlined. Run it again
after changing a redraw or an SVG (`--check` tells whether the PNGs and
the font are up to date).

`python tools/touch_preview.py --out port/android/docs` draws the default
layout over a picture as the app draws it, on a phone and a tablet, in each
style, held and idle (`port/android/docs/touch-controls-*.png`).
