# msdfgen (adapted)

Multi-channel signed distance field generator by Viktor Chlumsky, MIT
licensed (`LICENSE.txt`, as upstream has it, SHA-256
`32a94b25ad7072a78457d35019b32d73d2c265c806b0b76e9638fe800330bbe4`).

Upstream: https://github.com/Chlumsky/msdfgen, at commit
1c106ed8117893bf943e577f62eb0665fb271e46.

No file of it is copied here. `port/linux/src/text_msdf_gen.h` is a C
adaptation of its algorithms: the edge colouring (`edgeColoringSimple`,
`switchColor`), the line's and quadratic curve's signed distances and the
equation solver they use, the pseudo-distance, and the error correction's
clash detection (`detectClash`), with the fill's sign taken from the
outline's winding. It makes the high-res text's distance fields
(`display.text_renderer "msdf"`): the build's baked atlas
(`tools/msdf_atlas.py`, `port/assets/fonts/msdf_atlas.zlib`) and any glyph
made at run time (`port/linux/src/text_hires.c`). It builds into every
platform's game, so the releases carry this licence
(`tools/linux_package.py`, `msdfgen-LICENSE.txt`).
