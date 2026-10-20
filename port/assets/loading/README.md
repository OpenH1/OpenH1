# Loading artwork

`LOADING.jpg` is Paul Hontz's supplied remake, retained verbatim. Its SHA-256
is `5062ff5c83664c6b2f692657ae864e9ad74d71b0af8af1e4305bd9a26dc3fa0f`.

`loading.rgba.zlib` contains 640×480 RGBA bytes, compressed with zlib. It is
made by decoding the JPEG, converting to RGBA and resizing with Pillow's
LANCZOS filter. No recoloring, thresholding or generated artwork is applied.

To regenerate, install Pillow and run `python tools/loading_assets.py`.
Normal builds need neither Pillow nor JPEG decoding: `tools/embed_assets.py`
embeds the committed compressed payload, which is decoded once at startup.

The committed conversion used Pillow 13.0.0 and zlib 1.3.2. The input is
1600×1200; the output has 1,228,800 uncompressed bytes and opaque alpha.
The compressed payload's SHA-256 is
`f66b23eb3960213d8f0c821cf8feed577fad5116ec8762bab21fdd199ad51359`.

## The main menu's ring

Once the main menu has been shown, the loading screen draws the menu's own
scene in the picture's place: the ring against the dark, as the picture's
is (its sky, stars and planets left out), read from the player's `ui.map`
as the menu drew it (`port/linux/game/loading_scene.c`)
when the map change leaves the menu, from the menu's camera. Nothing of the
map's is kept in the repository or the releases; without a scene (the first
load, before the menu, or another ui map without one) the picture above is
drawn as before.

The presenter draws the scene on the processor, into the same 640x480 image
the picture is (`port/linux/src/loading_ring.h`), so every renderer shows it
without pipelines of its own, and the picture's sweep runs over it as it did
over the picture (`loading_art.h`):

- in the picture's one hue: the scene's brightness matched to the picture's,
  its detail etched brighter, coloured from black through navy and the
  picture's blue to nearly white; the picture's words over it;
- the sweep lights it: what it feeds the sweep's trail is the ring etched,
  with the light of the sweep's band (a light behind the ring at the band's
  place) on its rim and in its detail, so its edges flare as the band passes;
- the scene stays dimly in view between the sweeps (`LOADING_ART_SCENE_BASE`),
  the ring's surface flowing slowly along it as it turns, the camera
  drifting a little;
- in VR, a stereo pair on a window in the world, 8 m before the head as the
  load began, as wide as the menu's camera saw: it starts flat, both eyes'
  pictures the same, and as the first sweep crosses the eyes come apart and
  follow the head, the ring found far behind the glass at eight times the
  game's scale; the sweep's light stays on the window. The level's first
  pictures then come up out of the dark (`vr_loading_fade_draw`).

Diagnostics: `HALO_LOADING_SCENE_DUMP=<file>` writes the scene as the
presenter receives it, and `HALO_LOADING_CAPTURE_DIR=<folder>` writes the
loading frames (`HALO_LOADING_CAPTURE_EVERY=<frames>`: every that many).
