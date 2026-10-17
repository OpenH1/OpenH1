"""Ninja rules for the OpenGL backend in the host of the guest builds (the
Linux desktop guests', tools/linux_guest_build.py, and Android's,
tools/android_build.py): port/linux/src/xgpu_gl.c and its files built into
the 64-bit host behind the renderer's command stream (XGPU_GL_HOST;
port/linux/host/host_gl_stream.c), beside the host's stand-ins for the
guest's headers (port/linux/host/gl). Refer to "The renderer's command
stream" in port/linux/README.md."""

from pathlib import Path
from typing import List

from .android_build import ZLIB_DEFINES, ZLIB_DIR, ZLIB_SOURCES
from .embed_assets import smaa_assets_build
from .ninja_syntax import Writer

PORT_DIR = Path("port/linux/host")
LINUX_DIR = Path("port/linux")

# the OpenGL backend the host has, for the renderer's command stream
# (port/linux/host/host_gl_stream.c): the
# backend's files of port/linux/src, copied beside the host's stand-ins for
# the guest's headers (port/linux/host/gl), which their quoted includes then
# find in place of the guest's
HOST_GL_SHARED = ["xgpu.h", "xgpu_backend.h", "loading_presentation.h", "loading_art.h", "loading_art_tables.h", "loading_gl.h", "xgpu_shader.h", "xgpu_gl.h", "gl.h", "xgpu_gl.c", "xgpu_glsl.c",
                  "xgpu_post.c", "xgpu_text.c", "gl_functions.c", "xbox_texture_decode.c"]
HOST_GL_STAND_INS = ["platform.h", "sdl_platform.h", "port_config.h"]
HOST_GL_SOURCES = ["host_gl_stream.c", "host_gl_textures.c"]
# (and the VR mode's: its headers, and the records' entries)
HOST_GL_VR_SHARED = ["vr.h", "vr_host.h", "vr_visor.h"]
HOST_GL_VR_SOURCES = ["host_gl_vr.c"]
# (the guest's renderer, then: none of its own)
RENDER_STREAM_GUEST_EXCLUDED = ["xgpu_gl.c", "xgpu_post.c", "xgpu_glsl.c", "gl_functions.c"]


def host_gl_renderer(n: Writer, prefix: str, label: str, build: Path, host_cc: str, host_cflags: str,
                     desktop_gl: bool, vr: bool) -> List[Path]:
    """The OpenGL backend in the host (port/linux/host/host_gl_stream.c), for
    the renderer's command stream: the backend's files of port/linux/src
    (HOST_GL_SHARED) copied beside the host's stand-ins for the guest's
    headers (port/linux/host/gl), and the XDK's enumerations and texture
    format words for them (xdk_enums.h, as tools/macos_build.py makes it);
    its anti-aliasing's SMAA data and zlib; with vr, its VR mode
    (host_gl_vr.c, with vr.h's and vr_visor.h's structures). Every guest
    build's host has it (its import, host_gl_window, is every guest's); the
    guests draw with it. Returns the objects."""
    shared_dir = build / "host" / "gl"
    obj_dir = build / "host" / "obj" / "gl"
    n.rule(name=f"{prefix}_gl_copy", command="cp $in $out", description=f"{label} HOST GL $out")
    xdk = Path("port/include/xdk")
    n.rule(name=f"{prefix}_xdk_enums",
           command=(f"(sed -n '/^\\/\\* ---------- enumerations/,/^\\/\\* ---------- structures and unions/p' "
                    f"{xdk / 'xdk_pdb.h'} | sed '$$d' && "
                    f"grep -E '^#define D3D(FORMAT|SIZE|TEXTURE)_|^#define D3DTSS_MAXSTAGES ' {xdk / 'xdk_d3d8.h'}) > $out"),
           description=f"{label} HOST GL $out")
    copies: List[Path] = []
    for name in HOST_GL_SHARED + (HOST_GL_VR_SHARED if vr else []):
        n.build(outputs=shared_dir / name, rule=f"{prefix}_gl_copy", inputs=LINUX_DIR / "src" / name)
        copies.append(shared_dir / name)
    for name in HOST_GL_STAND_INS:
        n.build(outputs=shared_dir / name, rule=f"{prefix}_gl_copy", inputs=PORT_DIR / "gl" / name)
        copies.append(shared_dir / name)
    n.build(outputs=shared_dir / "xdk_enums.h", rule=f"{prefix}_xdk_enums",
            inputs=[xdk / "xdk_pdb.h", xdk / "xdk_d3d8.h"])
    copies.append(shared_dir / "xdk_enums.h")
    # (the renderer's own code paths: the host's, OpenGL ES's or desktop
    # OpenGL's, and the VR mode's as the host's defines say)
    cflags = " ".join([f"-I{shared_dir}", host_cflags, f"-I{ZLIB_DIR}", "-DXGPU_GL_HOST=1",
                       *([] if desktop_gl else ["-DHALO_GLES=1"])])
    objects: List[Path] = []
    for source in [shared_dir / name for name in HOST_GL_SHARED if name.endswith(".c")] + \
            [PORT_DIR / name for name in HOST_GL_SOURCES + (HOST_GL_VR_SOURCES if vr else [])]:
        obj = obj_dir / (source.name + ".o")
        n.build(outputs=obj, rule=host_cc, inputs=source, order_only=copies, variables={"cflags": cflags})
        objects.append(obj)
    smaa = smaa_assets_build(n, f"{prefix}_host", build / "host" / "smaa_embedded.c")[0]
    n.build(outputs=obj_dir / "smaa_embedded.c.o", rule=host_cc, inputs=smaa, variables={"cflags": cflags})
    objects.append(obj_dir / "smaa_embedded.c.o")
    for name in ZLIB_SOURCES:
        obj = obj_dir / "zlib" / (name + ".o")
        n.build(outputs=obj, rule=host_cc, inputs=ZLIB_DIR / name,
                variables={"cflags": " ".join([host_cflags, *ZLIB_DEFINES, "-w"])})
        objects.append(obj)
    return objects
