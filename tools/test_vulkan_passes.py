"""The Vulkan core's pass/load actions and layered image views over a fake Vulkan.

Compiles the actual core; no GPU, display, game data or Vulkan loader is
needed. Vulkan headers come from configure's downloaded host dependencies
or the system installation.
"""

import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parent.parent


def test_vulkan_passes(tmp_path):
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not cc or not sys.platform.startswith("linux"):
        pytest.skip("needs a C compiler on Linux")
    candidates = [ROOT / "build/linux_vulkan/third_party/vulkan-headers", Path("/usr/include")]
    headers = next((path for path in candidates if (path / "vulkan/vulkan_core.h").exists()), None)
    if headers is None:
        pytest.skip("needs Vulkan headers (configure --linux-vulkan)")
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
