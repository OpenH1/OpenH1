# The touch controls' art

The Android app's on-screen controls (`TouchControls.java`; "Touch
controls" in `port/android/README.md`) are the app's own interface, not the
game's art. They are drawn with:

- `kenney-mobile-controls/`: SVGs of Kenney's Mobile Controls pack (CC0):
  the plates, the stick, the pressed ring and most icons. Its README names
  the upstream, the version, the checksum and what uses each file.
- `openh1/`: icons the pack lacks, drawn for OpenH1 in the pack's manner
  (flat white glyphs on a 48 x 48 square, a light grey-blue shade on their
  second parts), under the repository's licence (`AGPL-3.0-or-later`):

  | File | Control |
  | --- | --- |
  | `icon_grenade.svg` | Grenade (throw a grenade) |
  | `icon_grenade_type.svg` | Gren. type (change the grenade): the grenade, smaller, with Kenney's `icon_arrow_rotate` |
  | `icon_crouch.svg` | Crouch |
  | `icon_flashlight.svg` | Light (the flashlight) |

`python tools/touch_assets.py` draws them all into
`port/android/app/src/main/res/drawable-nodpi/touch_*.png` (one large size
each, which the app scales with mipmaps); run it after changing an SVG
(`--check` tells whether the PNGs are up to date). An icon shows what its
button does in the player's profile, not the button: with "Swap triggers",
the right trigger's button shows the grenade.

`python tools/touch_preview.py --out port/android/docs` draws the default
layout over a picture as the app draws it, on a phone and a tablet
(`port/android/docs/touch-controls-*.png`).

No icon may come from anywhere else, nor any art or logo of the game.
