"""The Vulkan core's pass/load actions and layered image views over a fake Vulkan.

Compiles the actual core; no GPU, display, game data or Vulkan loader is
needed. Vulkan headers come from configure's downloaded host dependencies
or the system installation.
"""

import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import zlib

import pytest

ROOT = Path(__file__).resolve().parent.parent


def test_vulkan_passes(tmp_path):
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not cc or not sys.platform.startswith("linux"):
        pytest.skip("needs a C compiler on Linux")
    candidates = [ROOT / f"build/{name}/third_party/vulkan-headers" for name in ("linux_x86_64", "linux_arm64")]
    headers = next((path for path in candidates + [Path("/usr/include")]
                    if (path / "vulkan/vulkan_core.h").exists()), None)
    if headers is None:
        pytest.skip("needs Vulkan headers (configure.py fetches them for the Linux build)")
    binary = tmp_path / "vulkan_passes"
    subprocess.run([
        cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
        f"-I{headers}", f"-I{ROOT / 'port/android/host'}", f"-I{ROOT / 'port/android/include'}",
        str(ROOT / "port/linux/vulkan/tests/passes_test.c"), "-o", str(binary), "-ldl", "-lm", "-pthread",
    ], check=True)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr


def test_vr_required_capabilities(tmp_path):
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("needs a native C compiler")
    # Compile the compositor's actual gate; it must reject an optional GPU
    # feature that the sky needs instead of silently changing its pixels.
    text = (ROOT / "port/linux/vulkan/host_vulkan_vr.c").read_text()
    function = text[text.index("static BOOL supported(void)"):
                    text.index("static BOOL sample_counting(void)")]
    source = tmp_path / "vr_capabilities.c"
    source.write_text(r"""
#include <assert.h>
#include <stdarg.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define HOST_LOG_WARN 1
struct xgpu_vulkan_capabilities { unsigned int multiview_views; int depth_clamp; };
static struct xgpu_vulkan_capabilities available;
static unsigned int warnings;
static void xgpu_vulkan_capabilities(struct xgpu_vulkan_capabilities *capabilities)
{
    *capabilities = available;
}
static void host_logf(int priority, const char *format, ...)
{
    assert(priority == HOST_LOG_WARN && format);
    warnings++;
}
""" + function + r"""
int main(void)
{
    available.multiview_views = 2;
    available.depth_clamp = 1;
    assert(supported() && !warnings);
    available.multiview_views = 4;
    assert(supported() && !warnings);
    available.depth_clamp = 0;
    assert(!supported() && warnings == 1);
    available.depth_clamp = 1;
    available.multiview_views = 1;
    assert(!supported() && warnings == 2);
    available.multiview_views = 0;
    assert(!supported() && warnings == 3);
    return 0;
}
""")
    binary = tmp_path / "vr_capabilities"
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_post_shaders(tmp_path):
    # The anti-aliasing passes' generated file (tools/vulkan_post_shaders.py):
    # SMAA's lookup textures are port/third_party/smaa's, inflated, and each
    # module is valid for Vulkan 1.1 where spirv-val is installed.
    text = (ROOT / "port/linux/vulkan/xgpu_vulkan_post_shaders.c").read_text()
    arrays = {name: [int(word, 16) for word in re.findall(r"0x([0-9a-f]{8})", body)]
              for name, body in re.findall(r"const uint32_t (\w+)\[\] =\n\{\n(.*?)\n\};", text, re.S)}
    for name, file, size in (("area", "area_tex.zlib", 160 * 560 * 2), ("search", "search_tex.zlib", 64 * 16)):
        texels = zlib.decompress((ROOT / "port/third_party/smaa" / file).read_bytes())
        words = arrays[f"xgpu_vulkan_post_{name}_texture"]
        assert len(texels) == size
        assert struct.pack(f"<{len(words)}I", *words)[:size] == texels
    modules = sorted(name for name in arrays if name.endswith(("_vert", "_frag")))
    assert modules == ["clear_frag", "fxaa_frag", "post_vert", "rotate_frag", "smaa_blend_frag", "smaa_edges_frag",
                       "smaa_weights_frag"]
    validator = shutil.which("spirv-val")
    if not validator:
        pytest.skip("needs spirv-val (SPIRV-Tools)")
    for name in modules:
        assert arrays[name][0] == 0x07230203
        module = tmp_path / f"{name}.spv"
        module.write_bytes(struct.pack(f"<{len(arrays[name])}I", *arrays[name]))
        subprocess.run([validator, "--target-env", "vulkan1.1", str(module)], check=True)
