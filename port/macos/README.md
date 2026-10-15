# macOS

`ninja macos` builds the game for macOS on Apple silicon (arm64; not
Intel). On such a Mac, `ninja` without a target builds it.

| Result | Item |
| --- | --- |
| `build/macos/halo` | The game, an arm64 executable |
| `build/macos/libSDL3.0.dylib` | SDL 3.4.16, without OpenGL. The game finds it next to the executable. |
| `build/macos/xgpu_metal_test` | The Metal backend's tests (`ninja macos_metal_test`) |

The renderer is Metal's (refer to "Metal"), but the game does not draw
through it yet. Until it does, the game runs with the null renderer
(`HALO_NULL_RENDERER=1`): it plays, but shows nothing, or with
`HALO_METAL_WINDOW=1` a window that the host clears at each of the
display's refreshes. It was tested on an Apple A18 Pro with macOS 27.

## Requirements

- macOS 14.4 or later, on Apple silicon.
- Xcode's clang (it has the `arm64_32` target), Python and ninja.
- `ld.lld` and `llvm-ar`: `brew install llvm lld`. `configure.py` finds them
  on the `PATH` or in Homebrew's folders. The option `--macos-cc` of
  `configure.py` selects a different clang.
- CMake, for SDL3.
- A network connection for the first build. `configure.py` downloads musl
  1.2.5, SDL 3.4.16 and the Khronos OpenGL ES headers (declarations only,
  for the guest's renderer code) to `build/macos/third_party`.

## Start the game

```sh
HALO_NULL_RENDERER=1 HALO_DATA_ROOT=/path/to/folder/with/maps build/macos/halo
```

With `HALO_METAL_WINDOW=1` (`debug.metal_window`) the window opens too.

The game data, the settings and system link are as on Linux (refer to
[port/linux/README.md](../linux/README.md)): `config.toml` is next to the
executable, `HALO_DATA_ROOT` (`paths.data`) is the folder that holds
`maps/`, and the saved games go to `~/.local/share/halo-linux`
(`HALO_SAVE_ROOT`, `paths.saves`). To play a game without a person at it,
use the network tests (`HALO_NETWORK_TEST=host:bloodgulch`;
[port/linux/NETCODE.md](../linux/NETCODE.md)).

## How it operates

The game runs as on Linux arm64 (refer to "64-bit ARM" in
[port/linux/README.md](../linux/README.md#64-bit-arm)): it is ILP32
AArch64 code, a guest image in a 64-bit process, with the desktop code
paths of the platform layer. The host is the Linux arm64 one
(`port/android/host`, `port/linux/arm64/host_main.c` and
`host_desktop.c`), with these differences.

### The guest's memory: a base register

The guest's 32-bit pointers are addresses in a 4 GB space. On Linux and
Android that space is the low 4 GB of the process. macOS on Apple silicon
keeps the low 4 GB from every process (`__PAGEZERO`): the kernel maps
nothing there, and an executable linked with a smaller `__PAGEZERO` is
killed before `main`.

So the guest's 4 GB lie at a base that the host chooses, a multiple of
4 GB, and each guest address is that base plus a 32-bit offset. The guest
image is built with the base register (`tools/guest_base_register.py`,
after `tools/android_asm_convert.py`), as LFI (Lightweight Fault Isolation)
rewrites AArch64 code:

- `x21` holds the base, and `x22` is a scratch register. The guest is
  compiled with both reserved (`-ffixed-x21 -ffixed-x22`).
- Each load and store through a register adds the base to the register's
  low 32 bits: `ldr w0, [x1]` becomes `ldr w0, [x21, w1, uxtw]`, and
  `ldr w0, [x1, #8]` becomes `add w22, w1, #8` then
  `ldr w0, [x21, w22, uxtw]`. The instructions without that addressing
  mode (`ldp`, exclusives, SIMD structures) add the base into `x22` first.
- The stack pointer and the frame pointer hold full addresses (the stack is
  in the guest's memory), and accesses through them are left as they are.
  So are accesses to globals through `adrp`, whose result is a full address.
- An address taken from the stack, the frame or a page (`add x0, sp, #16`)
  is computed in 32 bits (`add w0, wsp, #16`): every other register holds
  a pointer as its 32-bit offset, as the arm64_32 code expects.
- Indirect branches go to the base plus the target (`blr x8` becomes
  `add x22, x21, w8, uxtw` then `blr x22`).
- An instruction that the pass does not know is an error of the build,
  never left as it is.

The host enters guest code through `port/android/host/host_enter.S`, which
sets `x21`. The guest's calls to the host go through import stubs that
change the pointer arguments into host addresses (`tools/android_imports.py`,
from the prototypes in `guest/runtime/guest_host.h`; the OpenGL and posix
wrappers do so in C). The host changes the pointers inside the structures
that the guest gives it (`iovec`, `timespec`, ...).

The guest's memory is `port/android/host/host_memory_base.c`: the 4 GB
reserved at one time, with guard regions below and above. The Xbox window
and the image are at their usual guest addresses, `0x80000000` and
`0x88000000`.

The Linux arm64 and Android builds can use the base register too
(`configure.py --guest-base`): then they do not need the fixed low ranges
either. By default they do not use it.

The cost of the base register: refer to `tools/base_register_bench`
(`python3 tools/base_register_bench/run.py`). On an Apple A18 Pro, the
rewrite adds about 10% to the code; the instructions it adds cost from 0
to 6% of the time of CPU-bound kernels (about 3%), and a chain of dependent
loads (pointer chasing) takes one cycle more for each load (4 instead of
3). LFI measured 6.4% on SPEC 2017 on an Apple M1 with the same scheme.
On Linux arm64 the script measures the same guest with and without the
base register.

### Pages of 16 KB

macOS's pages are 16 KB; the guest's are 4 KB (its musl, and the Xbox
memory of `src/xbox_memory.c`). `host_memory_base.c` keeps the protection
of each 4 KB page of the guest. A host page gets the union of the
protections of its four guest pages, stays mapped while any of them is,
and a guest page mapped into a host page in use is zeroed in place. The
write tracking (`host_memory_watch.c`) protects host pages, so a write
marks the whole 16 KB as written.

### System calls

The guest's musl makes Linux system calls. `port/macos/host_syscall.c`
does each one with the Darwin call that does the same, and changes the
numbers, the flags (`open`, `fcntl`, `mmap`, the `AT_` flags), the
structures (`stat`, `dirent64`, `utsname`, `timespec`), the clocks and the
error numbers. The futexes of musl's locks use `os_sync_wait_on_address`.
`HALO_TRACE_SYSCALLS=1` writes each call and its result to the log.

`src/posix_*.c` (files, sockets, UPnP, DTLS) run in the host as on Linux,
with macOS's differences: no `SOCK_CLOEXEC` or `accept4`, `getentropy`
for random bytes, and socket addresses that start with a length byte.

### The main thread

Cocoa's windows and events belong to the process's main thread, and the
game's main thread makes the window and pumps the events. So the game runs
on the main thread, after `host_main.c` moves it to a stack in the guest's
memory (`host_run_on_stack`). The other threads are made as on Linux.

### Renderer

The guest still contains the OpenGL ES renderer (`src/d3d8_gl.c`), but the
host gives it no OpenGL: `host_renderer.c` makes each OpenGL entry point
stop the game with a message. SDL is built without OpenGL. The renderer
of this build is Metal's, in the host (below).

## Metal

The renderer's backend for macOS is Metal, in the host: Metal is a 64-bit
API, and the game is a 32-bit guest. The game's renderer (its front end,
in the guest) will write each frame as a command stream into its memory,
and the host's render thread plays it into the backend
(`port/android/host/host_render_stream.c`, the backend's table in
`xgpu_metal_stream.m`) while the game simulates the next frame. That
wiring comes next; what is here is all that does not wait for it.

### The window and presentation (`host_metal.m`)

The guest makes its window with `SDL_WINDOW_METAL` (`sdl_platform.c`, with
`HALO_METAL`) and hands it to the host (`host_metal_window_open`, an import
of `host_imports.list`), on the main thread, which Cocoa's views need. The
host takes SDL's view and its `CAMetalLayer` (BGRA8, the window's pixels:
Retina's, resized with the window and in fullscreen), and brings the
window to the front (a window behind others shows nothing, and the
drawables presented to it never reach the screen).

Presentation has a thread of its own, run by a `CAMetalDisplayLink`: at
each refresh of the display, the link hands over a drawable, and the
thread draws the oldest frame queued into it, letterboxed to the frame's
shape, and presents it. A ProMotion display's rate follows the frames, up
to its highest; a fixed display's is its own. A frame's picture is copied
into one of three presentation images when the frame ends (the next frame
may draw into the back buffer before the display shows this one). These
images pace the game: with all three queued or on screen, the next frame
waits, so the game runs no faster than the display shows frames. When the
display takes no frames for 100 ms (the window minimized, or between the
desktop and fullscreen), the oldest queued frame is dropped instead, so
that the game goes on.

Until frames come, as with the null renderer and `debug.metal_window`,
each refresh clears the drawable and presents it.

`HALO_METAL_STATS=<seconds>` logs the pacing that often: the link's
refreshes, the frames and clears presented, how many drawables reached the
screen and the intervals between them, the frames that waited for an
image or were dropped.

### The backend (`xgpu_metal.m`)

`xgpu_metal.h` declares its functions; `xgpu_metal_stream.m` makes them the
command stream's backend table (it is built where
`port/android/host/host_render_stream.h` is). They run on one thread, the
render thread.

- **Pipelines.** A render pipeline state is made for what Metal bakes into
  one (the shaders, the vertex layout, blend, the write mask, the targets'
  formats and samples) and kept by that; depth-stencil states by the depth
  and stencil part. Culling, winding, the fill mode, depth bias and depth
  clamping are the encoder's state. New pipelines are added to a binary
  archive (`~/Library/Caches/OpenCE/metal_pipelines.bin`, or
  `HALO_METAL_ARCHIVE`), saved from the render thread once no pipeline has
  been added for 3 seconds (the game ends its process without warning);
  the next run makes its pipelines from it.
- **Shaders** are MSL functions (`xgpu_metal_shader_create`), compiled
  with `newLibraryWithSource` and safe maths. Where a draw's shaders read
  what is set in `xgpu_metal_bindings.h`, the one place the MSL emitter
  (`port/linux/src/xgpu_msl.c`) and the backend agree on: the vertex
  constants (`setVertexBytes`), the draw's uniforms (both stages), the
  inputs no stream feeds (a buffer read at a constant step: every input is
  an `[[attribute(n)]]`), the streams, a texture and a sampler for each
  stage.
- **Samplers** are kept by their description. Metal's samplers have no LOD
  bias (the shader applies it) and three border colours (another is the
  emitter's to blend in).
- **Textures** in the Xbox formats Metal has: BGRA8 (and X8R8G8B8, its
  alpha read as 1 by a swizzle), R5G6B5, A1R5G5B5, X1R5G5B5, A4R4G4B4 (read
  through ABGR4 by a swizzle), L8, A8, A8L8, DXT1 to DXT5 (BC1 to BC3), and
  the depth formats (D24S8 is 32-bit float depth with 8-bit stencil: Apple's
  GPUs have no 24-bit depth). An upload writes a texture at once where no
  work the GPU still has reads it, else it is a blit in the frame's order.
  A multisampled target draws into a multisampled attachment resolved into
  it at each pass's end.
- **Guest data in place.** Guest memory is the host's (unified memory), so
  a range the GPU reads is wrapped in a buffer
  (`newBufferWithBytesNoCopy`) over the readable host pages about it, up to
  a granule of 4 MB, without a copy. A buffer keeps the pages it was made
  over: when the guest maps pages again, `host_memory_base.c` says so
  (`host_memory_remapped`), and the buffers over them go. The command
  stream's ring is one buffer too, so its inline data is drawn in place.
- **Passes.** Each change of targets ends the render command encoder; the
  next draw or clear begins one, with load actions "load", or "clear" for an
  attachment that a clear of the whole of it came first for. Other clears
  are quads drawn through a clear pipeline (of the channels cleared).
- **Visibility tests** count samples in a visibility result buffer, one for
  each pass (Apple's GPUs zero the first results an earlier pass counted
  into a buffer when a later pass counts into the same one), and the counts
  are known when the frame's command buffer completes; they are written for
  the game divided by the samples one of its pixels is.
- **Frames.** Each frame is a command buffer; at most two frames are in
  flight, and each has a fence (the serial of its last command buffer).

### Tests

`ninja macos_metal_test`, then `python3 -m pytest -q tools/test_macos_metal.py`,
or `build/macos/xgpu_metal_test` itself: clears (whole, rectangles, a
channel alone), a textured quad with the vertex constants and the draw's
uniforms, every format, the pipeline cache, the winding, visibility tests,
multisampling, the order of uploads, cubes, arrays and volumes, guest
memory read in place and mapped again, frames in flight, and the command
stream's table where its header is. Each draws with shaders written in the
test, reads the pixels back and checks them. With `--archive file`, a
second run checks that every pipeline comes from the archive.
`build/macos/xgpu_metal_test --window 15 --resize` opens a window, draws
frames as fast as the presentation takes them, resizes it, makes it
fullscreen, and logs the rate (with `HALO_METAL_STATS=3`, the
presentation's own log).

## Find problems

- The log goes to the terminal; the game's own log is `debug.txt` in the
  data folder.
- A crash prints the registers, the guest's base and the frame chain. The
  guest's addresses are the low 32 bits:
  `llvm-symbolizer --obj=build/macos/halo_guest.elf 0x<address>`.
- `HALO_TRACE_SYSCALLS=1` logs the guest's system calls.

## Limits

- The game does not draw through the Metal renderer yet: it runs with the
  null renderer.
- No app bundle, signature or notarization yet; the executable runs from
  the build folder.
- The self-updater and the Discord invites are not set up for macOS.
