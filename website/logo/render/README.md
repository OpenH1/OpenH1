# Photo-real renders of the OpenH1 mark

Blender renders of the chosen logo (Hull Marking, variation C) as a worn,
stencilled steel panel. Both scripts need Blender 4.2 or later (made with 5.2).

- `build_scene.py` reads `../round4/c-bright/mark.svg`, builds the plate,
  fasteners, materials, cameras and lights, and saves `scene.blend`. The
  stencil masks it rasterises from the SVG go into `_build/` and are packed into
  the .blend. Everything is procedural: there are no downloaded textures or HDRIs.
- `render.py` renders the shots from `scene.blend`:
  `hero.png` (2400 x 1600, head-on on a dark backdrop), `front.png` (2048 x 2048,
  transparent), `icon-1024.png` (1024 x 1024, transparent, panel about 85 %) and
  `detail.png` (2400 x 1600, a fastener close-up).

`scene.blend` (about 1 MB) is committed for convenience; `_build/` is not, and
the first command below recreates both.

## The final render on an NVIDIA machine

From the repository root, in one go:

```sh
blender -b -P website/logo/render/build_scene.py && blender -b website/logo/render/scene.blend -P website/logo/render/render.py -- --device OPTIX --denoiser OPTIX --samples 4096 --res 4096
```

`--device` falls back to CUDA, HIP, Metal and then the CPU if OptiX isn't there
(`AUTO`, the default, tries them in that order). `--res` sets the long edge of
every shot (leave it out for the sizes above), and `--samples` the samples per
pixel (the defaults, 192 to 256 per shot with adaptive sampling and denoising, are
set for a quick render on a laptop). Other options: `--shots
hero,front` to render some, `--out DIR`, `--denoiser OIDN|NONE`, `--no-dof`, and
`--scale 0.25` for quick previews. The defaults are at the top of `render.py`.

## Next changes (not done yet)

- A grazing key light from the upper left, to bring out the paint thickness and the stencil edges.
