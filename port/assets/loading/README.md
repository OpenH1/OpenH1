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
