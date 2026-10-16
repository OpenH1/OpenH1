# The Vulkan renderer

The renderer's backend for Vulkan, behind the renderer's command stream
(`xgpu_stream_*`), as Metal's is on macOS (`port/macos/xgpu_metal*.m`): the
guest records each frame as a stream, and the host replays it with Vulkan.
It is built into the 64-bit x86 Linux build (the guest on the host,
`tools/linux_guest_build.py`) as `ninja linux_vulkan`, for the desktop first;
the Steam Frame and Android are to follow.

| File | What it is |
| --- | --- |
| `port/linux/src/xgpu_spirv.c` | The shaders: the NV2A's programs (the shader IR, `xgpu_shader.h`) written as SPIR-V |
| `xgpu_vulkan.c` | The device, memory, images, samplers, pipelines, passes, draws, visibility queries, the swapchain |
| `xgpu_vulkan_stream.c` | The stream's backend table: each record made into the calls above |
| `xgpu_vulkan_textures.c` | Textures, decoded by the shared Xbox decoder (swizzled, palettes, 3D) where Vulkan cannot sample them as they are |
| `host_vulkan.c`, `guest_vulkan.c` | The window: SDL's Vulkan surface handed from the guest to the host |

## Build and run

    python3 configure.py --linux-vulkan
    ninja linux_vulkan
    build/linux_vulkan/halo

Configuring once fetches the Khronos Vulkan headers (a pinned release, checked
by SHA-256) into `build/linux_vulkan/third_party`; the game loads the system's
`libvulkan.so.1` when it starts. It needs Vulkan 1.3 (dynamic rendering,
timeline semaphores, host query reset): any current desktop driver.

| Variable | What it does |
| --- | --- |
| `HALO_VULKAN_VALIDATION=1` | The validation layers on, their messages logged (the layers must be installed, or found through `VK_ADD_LAYER_PATH`) |
| `HALO_VULKAN_PIPELINE_CACHE=<file>` | The pipeline cache's file (by default `$XDG_CACHE_HOME/opence/vulkan_pipelines.bin`); empty: none |
| `HALO_VULKAN_SHADERS=<dir>` | Each shader module written there as it is made (`.spv`) |
| `HALO_VULKAN_STATS=1` | Every 600 frames, the submissions, passes, draws, clears, pipelines made and their time, the upload ring's bytes and the textures made |
| `HALO_NO_VSYNC=1` | Presented without waiting for the display (immediate, or mailbox) instead of FIFO |

A window SDL cannot draw with Vulkan (its offscreen driver: NVIDIA's driver
has no headless surfaces) is drawn without a swapchain: the frames are drawn
and read back for screenshots, but not shown.

## Shaders: SPIR-V written directly

`xgpu_spirv.c` writes SPIR-V 1.3 (Vulkan 1.1) from the shader IR, as
`xgpu_msl.c` writes Metal's language from it. The alternative, GLSL for Vulkan
compiled by glslang, would put a GLSL compiler in the game (megabytes, and
milliseconds a shader): the shaders are made while the game draws, from
pipeline keys, not ahead of time, so they cannot be compiled when the game is
built. Written directly, a module takes microseconds and needs no library.

The layout: set 0 has the vertex program's constants (binding 0, a dynamic
uniform buffer), the shaders' other uniforms (1, dynamic), the dynamic lights
(2), their grid of cells (3) and the sample counters (4); set 1 has the four
textures (bindings 0 to 3). The vertex shader negates y (the NV2A's screen is
y down, as Vulkan's framebuffer is, and the clip space is OpenGL's) and its
position is `Invariant`, with the arithmetic it depends on `NoContraction`,
as GLSL's `invariant gl_Position` makes the OpenGL backend's.

`tools/spirv_corpus` writes the Metal corpus's variants as SPIR-V and checks
each with `spirv-val` (its README).

## As the OpenGL backend draws

The aim is the OpenGL backend's pixels. What was measured on an RTX 4090
(NVIDIA's driver, both APIs), drawing the same triangles both ways:
rasterization, interpolation, filtering, BC1 to BC3 decoding and short vertex
attributes are identical. Where the drivers differ, the backend makes up the
difference on NVIDIA's proprietary driver alone:

- Anisotropic filtering: NVIDIA's OpenGL centres its blend between mip levels
  where its Vulkan begins it at the lower level; a LOD bias (-42/256 at 2x
  and 3x, -54/256 at 4x to 7x, -64/256 at 8x to 16x) makes them identical.
- Cube maps: the OpenGL backend leaves seamless filtering off, so cube maps
  are sampled without it (`VK_EXT_non_seamless_cube_map`), where the device
  has it.

NVIDIA's OpenGL compiler fuses multiplications into the additions that use
them; its Vulkan compiler fuses none. Fusing them in the SPIR-V as OpenGL's
compiler does (explicit `Fma`) changed no pixel of Blood Gulch, so it is not
done: the remaining differences (a few thousand pixels a frame, most by one
level) are rounding in the shaders.

## Not yet

- The enhanced dynamic lights and their halos (the stream's capabilities say
  so: the game draws the classic lights), and SMAA (the frame is presented
  without it). MSAA is drawn.
- The VR views.
- A clear of some color channels alone is a clear of all of them (logged once;
  none of the menus, Blood Gulch or a10 has one).
