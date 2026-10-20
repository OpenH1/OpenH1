# HDR (`display.hdr`)

How the renderer draws high dynamic range: one contract, decided once on
the shared side (`port/linux/src/xgpu_hdr.h`, `xgpu_hdr.c`, the front end
`d3d8_device.c`) and carried out by each backend. Metal (macOS, EDR) draws
it now; Vulkan's desktop half (HDR10 or scRGB swapchains) implements the
same contract. Android and the VR mode do not: the Frame's OpenXR
swapchains are 8-bit sRGB on SDR panels, and the extra bandwidth would cost
the headset frame time for highlights it cannot show.

## What the player gets

- **Faithful** (the default): the picture as the game draws it, with only
  its highlights brighter. Surfaces, terrain, skin, the HUD and the menus
  sit at *paper white*, the display's SDR white, exactly where the SDR
  picture has them. The light the game adds on top goes past it: plasma,
  explosions, muzzle flashes, the dynamic lights and their glows, lens
  flares, the sun's glare and a sky's additive layers. Colours stay in
  sRGB's gamut.
- **Punchy**: the whole scene lifted, its bright parts more, each colour
  keeping its hue, and the highlights pushed further toward the peak.
- **Glows**: the first-person weapon's displays (the assault rifle's
  ammunition counter), meters and additive glows reach up to 1.75 times
  paper white; the HUD's deliberate glows (a meter's flash, the motion
  tracker) up to 1.5 times. The rest of the HUD and the menus stay at
  paper white.
- **Settings**: `display.hdr` (`auto`, `on`, `off`), `display.hdr_mode`
  (`faithful`, `punchy`), `display.hdr_paper_white` (50 to 250 percent of
  the system's SDR white). Video Setup's Extra Effects screen has a row for
  each on a renderer that draws HDR (the menus' `platform="hdr"`).
- **Fallback**: `auto` turns HDR on only on an HDR display; off, and on an
  SDR display, the frame is the 8-bit one, byte for byte. Highlights roll
  off under a soft shoulder to the display's peak (its EDR headroom, which
  follows its brightness), never clipping.

## The frame

The back buffer is RGBA16F. It holds the game's own values, blended by the
game's own maths: 1.0 is paper white, the encoding is sRGB's curve (extended
past 1 as sRGB's extended colour spaces extend it), the primaries sRGB's.
The pipeline is not made linear: the game's blends (its fog, its decals, its
transparent layers) were authored for gamma-space blending, and making them
linear would change every one of them. Nothing clips at 1 instead.

A value past 1 can only come of blending: every pixel shader's result is
clamped to [0, 1], as the NV2A's is. So a frame whose draws add no light past
white is the SDR frame, value for value (to half floats' precision: an 8-bit
target rounds each blend to 1/255, a half float keeps more).

## Which draws go past white

The front end tags each draw into the HDR back buffer
(`nv2a_pixel_shader_key.hdr`, `xgpu_hdr_classify`) from:

- the draw's blend: only an add whose factors are neither zero nor one and
  its complement can go past 1 from values in [0, 1] (an add, an alpha
  multiply add, a double multiply, a meter's constant blend), and only a
  subtraction or a factor of one less the destination's colour can go below
  0 over a pixel past white;
- the game's renderer's category, from `rasterizer_profile_begin` and
  `_end` (`halo_hdr_category`);
- what the drawing code knows (`halo_hdr_draw_kind`): a transparent group of
  the first-person weapon's own shaders (generic, chicago, meter; numeric
  ones are the ammunition counters), glass and water (a surface's specular
  light), a HUD meter while it flashes, the motion tracker;
- whether the 3D view is being drawn or the HUD over it (after the tone
  map).

| Draws | Examples | Tag |
| --- | --- | --- |
| Opaque, alpha blends, multiplies, min, max | the structure, models, fog, smoke, text | none: they cannot leave [0, 1] |
| Additive light in the 3D view | effects, particles, contrails, plasma shaders, light volumes, lens flares, the sun, the game's dynamic lights (diffuse and specular passes), the port's glows, a sky's additive layers | none: their light keeps going past white |
| A surface's look | double multiply decals and transparents; in Faithful the structure's specular lightmaps, reflections and mirrors, water's and glass's reflections; the fog screens | ceiling at paper white |
| Over the tone mapped view | the HUD's bitmaps and meters, screen flashes, screen effects, menus | ceiling at paper white |
| The first-person weapon's glows | the ammunition counter (whatever its blend), meters, additive display parts | expanded, ceiling 1.75 times paper white |
| The HUD's glows | a meter flashing, the motion tracker | expanded, ceiling 1.5 times paper white |
| Destination alpha | any draw writing alpha whose blend could take it past 1 (a double multiply's) | alpha limited to 1 (the game reads it as a blend factor) |
| The enhanced dynamic lights | a structure's lightmap pass | their shoulder goes on past white (below white, unchanged) |

A **ceiling** means: the draw may take no channel past *max(what the pixel
had, the ceiling)*, nor below 0. A surface's draw over a pixel that was
already past white (a decal over a wall a plasma bolt lights) therefore
does not dim that light, and over a pixel under white it stops where the 8-bit
target stopped. A float target cannot clamp a blend, so the backend blends
such a draw in its pixel shader, reading the pixel it draws over (Metal's
programmable blending), with the key's blend (`XGPU_HDR_BLEND_*`) and its
fixed-function blending off. One less the destination's colour is never
below 0 there.

**Expansion** brightens a glow where it is bright (its brightest channel
from 0.6 up) by more of the light the draw added (its own colour: what was
under it is not brightened), up to its ceiling, which keeps its hue
(`xgpu_hdr_expand`). The counter's digits, cyan, reach about 1.5 times paper
white in their blue (MaxRGB).

## The tone map and the encoding

The tone map runs over each window's 3D view after its lens flares and
before its anti-aliasing and the HUD (`halo_screen_tone_map`,
`xgpu_backend_tone_map`). Given the mode and the peak *P* (the display's
headroom over paper white; 1 on an SDR display):

- linear light *L* = sRGB's curve inverted (extended past 1); the SDR
  frame's colour *S* = the same of the value clamped to 1;
- **Faithful**: a pixel no channel of which is past 1 is left as it is.
  Past it, two colours are mixed by *t* = smoothstep(1, 2, the brightest
  channel's value): its own hue, *L* under the shoulder by its brightest
  channel, and white-hot, *S* (a channel at 1) brightened by the light that
  was clipped (luminance(*L*) / luminance(*S*)) under the shoulder. A glow a
  little past white keeps its colour; the core of an explosion or a plasma
  bolt is white-hot, as the SDR picture has it, only brighter.
- **Punchy**: the same, with *L* lifted by 1.2 and by up to 1.6 more as the
  SDR luminance goes from 0.5 to 1, and the shoulder starting at
  min(1, 0.75 *P*): the whole picture brighter, saturated colours kept.
- **The shoulder**: identity up to its knee, then Reinhard's curve meeting
  the line at its slope, toward *P* and never reaching it.

The encoding, as the frame is presented (after the HUD), makes it linear,
at most *P*, times paper white, in the display's encoding:

| Output | Values | Paper white |
| --- | --- | --- |
| SDR | the frame clamped to [0, 1] (the window's 8 bits) | the window's white |
| EDR (macOS) | extended linear sRGB, 1.0 the system's SDR white | `display.hdr_paper_white` percent of 1.0, at most the headroom |
| scRGB (Windows) | extended linear sRGB, 1.0 80 nits | the OS's SDR white times the setting |
| HDR10 | BT.2020 primaries (the same colours: no saturation added), PQ | 203 nits (BT.2408) or the OS's, times the setting |

The curves are one GLSL source (`xgpu_hdr_functions_source`, the tone map's
and the encoding's main functions in `xgpu_hdr.c`), compiled to SPIR-V by
`tools/vulkan_post_shaders.py` and from it to MSL by
`tools/metal_post_shaders.py` (SPIRV-Cross; the in-place tone map with
`--msl-framebuffer-fetch`), as FXAA and SMAA are. The C in `xgpu_hdr.h`
(`XGPU_HDR_REFERENCE`) is the same maths: the reference the tests hold the
shaders to, and the host's conversion of shots.

## What the backends do

The front end sends: the state (`xgpu_backend_hdr`: drawn in HDR, the mode,
paper white), the back buffer's format (`XGPU_FORMAT_RGBA16F` in its target
description and the screen's pipeline keys), each draw's tag in its pixel
key, and the tone map (`xgpu_backend_tone_map`). It reads the display from
the backend (`xgpu_backend_display_hdr`: whether it shows HDR, its headroom;
the stream's ring carries it, written by the host). A backend declares
`xgpu_backend_capabilities.hdr` and:

1. makes the back buffer RGBA16F, and its copies (the feedback copy of a
   target drawn into, anti-aliasing's) of its format;
2. carries out each tag: a ceiling or alpha limit blends in the shader with
   the pixel drawn over and the key's blend; expansion; the lights' raised
   shoulder;
3. tone maps each 3D view with the shared shader (Metal: a draw in the pass
   that drew the view, its pixels read in tile memory; Vulkan: fused into
   the anti-aliasing passes' copy, or a copy and a draw);
4. encodes the frame for its display and fills in the frame's paper white,
   peak and output (`struct xgpu_hdr_frame`) from what the display says
   (Metal: NSScreen's EDR headroom, polled and read when the window changes
   screens; Vulkan: the swapchain's colour space and SDL's SDR white and
   headroom; the encoding's limit at the peak is what holds the HUD's glows
   under it on a display of less headroom than their ceiling);
5. converts a half-float target to 8 bits where the front end reads one back
   (screenshots, the determinism log).

## Tests

- `tools/test_hdr.py`: the classification, table by table; the reference
  (Faithful leaves under white exactly as it is, never passes the peak,
  brightens with the light; Punchy lifts and keeps hues; a core is white-hot;
  the encodings' paper white and peak; PQ's 203 nits); the GLSL's constants
  against the reference's; the game's hooks.
- `port/macos/tests/xgpu_metal_test.m` (`test_stream_hdr`, through the
  command stream's table on the GPU): sixteen golden colours (under white,
  at it, a little and far past it, saturated and grey), tone mapped at peaks
  1, 4 and 16 in both modes and encoded for EDR, each against the reference
  to half floats' precision; the game's alpha kept; under white exactly as
  it was. The Vulkan backend's shaders run the same vectors.
- `tools/test_metal_post_shaders.py`: the generated MSL is what the tool
  writes from the GLSL.
- SDR identity: with `display.hdr` off, and `auto` on an SDR display, every
  frame's hash in `debug.determinism_log` matches master's.

## Choices, and where they depart from the plan

- **Ceilings blend in the shader, not by clamping after.** A clamp after a
  draw (a second draw taking the minimum) would dim light that was already
  past white under it: a specular pass over a wall a plasma bolt lights
  would wipe that light out. Only reading the pixel can stop a draw at
  max(what was there, the ceiling).
- **Faithful's colour past white mixes hue and the SDR colour.** Keeping hue
  alone would make plasma cores deep blue where the game shows them
  white-hot; the SDR colour alone would wash out a glow that is barely past
  white (the counter). The mix keeps both looks.
- **sRGB's curve, not a 2.2 power.** Untagged 8-bit content on a Mac reaches
  the panel through its sRGB-curve transfer, so sRGB's curve keeps the
  shadows where the SDR picture has them.
- **sRGB's gamut.** The EDR layer is tagged extended linear sRGB, as the
  owner asked: no colour is pushed into P3. The SDR layer stays untagged (it
  is unchanged), which on a P3 panel shows the game's colours slightly more
  saturated than sRGB's; switching HDR on a Mac with an XDR display shows the
  true sRGB colours.
- **HDR on an SDR display** (`on` where `auto` would be off) uses the same
  tone map at a peak of 1: surfaces as they are, highlights compressed into
  white with their colour rolled toward white-hot.
