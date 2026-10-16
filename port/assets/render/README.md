# The renderer's manifest

`manifest.txt` lists every shader and pipeline the game draws with on the
stock maps, so that a backend can build them when the game is built instead
of the first time a draw needs one (a stall of tens of milliseconds each).

It belongs to the game, not to a backend: it is what the shared front end
(`port/linux/src/d3d8_device.c`) hands any backend. Its format is in
`port/linux/src/xgpu_manifest.h`; `xgpu_manifest_read` reads it.

## What it holds

- **Vertex programs:** the NV2A microcode of the game's vertex shaders
  (`source/rasterizer/xbox/rasterizer_xbox_vertex_shaders_data.inc`), each
  named by its hash.
- **Pixel shader keys:** the combiner registers and render states the game's
  code sets for a draw (`nv2a_pixel_shader_key`), each named by its hash. The
  game builds them at each draw from its code and from the shader tags'
  fields (the transparent shaders' stages, their blend and fog).
- **Draws:** each pipeline key the draws use (the vertex program, the blend,
  depth, stencil, raster and target state, and the pixel key), with the
  vertex layout and the texture stages' sampling (sampler type, addressing,
  border colour) that a backend's shaders and pipelines can depend on.

Nothing else from the maps is in it: no textures, geometry, sounds, strings
or tag names, and no coordinates. Every value is a register, a format or a
state number of the Xbox's GPU. The build does not need the maps.

## How it is made

The front end records each new draw configuration into a file with
`debug.render_manifest` (`HALO_RENDER_MANIFEST=<file>`). The recordings are
merged:

```sh
python tools/render_manifest.py merge port/assets/render/manifest.txt <recording>...
```

`merge --replace` makes the manifest from the recordings alone.

The committed one is each stock map's first frame drawn through every shader
its tags hold (`display.prepare_shaders`: `port/linux/game/render_enumerate.c`),
recorded between the recording's `# enumerating` and `# prepared` notes, one
short run a map, all 24 merged.

The vertex shaders are not recorded: they are the game's code's, 67 programs
each with its declaration. `python tools/render_manifest.py vertex <file>`
reads them from the decompilation (`rasterizer_xbox_vertex_shaders.c` and
`_initialize.c`) as `v` and `s` records; a build makes every vertex function
from them, whatever a map or play meets.

## How it is checked

A recording made with a build whose shaders come from the manifest has no
draw the manifest does not:

```sh
python tools/render_manifest.py stats port/assets/render/manifest.txt <recording>...
```

## Another backend

A backend reads the manifest with `xgpu_manifest_read` and builds each draw
as it would at the draw (the Mac's, `port/macos/README.md`, builds the vertex
shaders' functions and those of the pixel programs its combiner's function
cannot draw into the app's library, and checks the rest against it):

1. It decodes the vertex program's microcode (`nv2a_vertex_program_decode`)
   and the pixel key (`xgpu_stream_pixel_key_restore`, then
   `nv2a_pixel_program_decode`).
2. It writes their shaders: GLSL with `xgpu_glsl.c`, for example.
3. It makes the pipeline from the key, the layout and the stages.

An OpenGL backend could compile and link the programs at start-up, or keep
their binaries (`glGetProgramBinary`). A Vulkan one could fill its pipeline
cache.
