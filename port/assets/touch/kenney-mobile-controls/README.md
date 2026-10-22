# Kenney's Mobile Controls

Eight SVGs of [Mobile Controls](https://kenney.nl/assets/mobile-controls)
1.0 (created 9 August 2025) by Kenney (www.kenney.nl), under Creative
Commons Zero 1.0 (CC0, public domain: `License.txt`, the pack's own).
Downloaded from https://kenney.nl/assets/mobile-controls; the zip's SHA-256 is
`7331c648651858fb3186a8c76e0e360e4d3ea71cfec9b502eb5cbb04a02e192b`.
Each file is copied unchanged from the zip's `Vector/` folder; only those
used are kept.

CC0 asks for no credit; Kenney is credited in `AUTHORS.md` anyway.

| File | From the zip | What uses it |
| --- | --- | --- |
| `button_circle.svg` | `Vector/Style A/` | the plates of start, back, the D-pad and the toolbar |
| `button_circle_highlight.svg` | `Vector/Highlights A/` | the ring of a plate (or the stick's knob) held |
| `joystick_circle_pad_d.svg` | `Vector/Style A/` | the move stick's base |
| `joystick_circle_nub_a.svg` | `Vector/Style A/` | the move stick's knob |
| `icon_pause.svg` | `Vector/Icons/` | start (Pause) |
| `icon_menu.svg` | `Vector/Icons/` | back |
| `icon_arrow.svg` | `Vector/Icons/` | the D-pad's buttons, turned to point each way |
| `icon_cog.svg` | `Vector/Icons/` | the toolbar's Options |

The controller's other buttons are the HUD's redraws (`port/assets/buttons`),
not Kenney's.

`tools/touch_assets.py` draws them into the Android app's
`res/drawable-nodpi/touch_*.png`, which `TouchControls.java` draws;
`../README.md` lists every picture.
