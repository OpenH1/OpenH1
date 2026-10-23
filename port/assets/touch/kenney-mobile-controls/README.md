# Kenney's Mobile Controls

Six SVGs of [Mobile Controls](https://kenney.nl/assets/mobile-controls)
1.0 (created 9 August 2025) by Kenney (www.kenney.nl), under Creative
Commons Zero 1.0 (CC0, public domain: `License.txt`, the pack's own).
Downloaded from https://kenney.nl/assets/mobile-controls; the zip's SHA-256 is
`7331c648651858fb3186a8c76e0e360e4d3ea71cfec9b502eb5cbb04a02e192b`.
Each file is copied unchanged from the zip's `Vector/` folder; only those
used are kept.

CC0 asks for no credit; Kenney is credited in `AUTHORS.md` anyway.

| File | From the zip | What uses it |
| --- | --- | --- |
| `icon_pause.svg` | `Vector/Icons/` | start (Pause) |
| `icon_menu.svg` | `Vector/Icons/` | back |
| `icon_arrow.svg` | `Vector/Icons/` | the D-pad's buttons, turned to point each way |
| `icon_cog.svg` | `Vector/Icons/` | the toolbar's Options |
| `icon_size_smaller.svg` | `Vector/Icons/` | the toolbar's Hide |
| `icon_size_larger.svg` | `Vector/Icons/` | the toolbar's Hide, while the controls are hidden (show them) |

The app draws each as a line icon: its outline, in the HUD's light blue.
The controller's buttons are the HUD's redraws (`port/assets/buttons`),
not Kenney's.

`tools/touch_assets.py` draws their outlines into the Android app's
`res/drawable-nodpi/touch_*.png`, which `TouchControls.java` draws;
`../README.md` lists every picture.
