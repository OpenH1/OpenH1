"""The host's OpenXR Vulkan ABI and swapchain ownership, without a GPU.

Requires the pinned headers that configure.py fetches for the Linux build
(and with --vr, OpenXR's) in build/linux_x86_64/third_party or
build/linux_arm64/third_party.
"""
import platform
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def third_party():
    """the Linux build's downloaded sources (the Vulkan and OpenXR headers):
    this computer's build's, else the other's"""
    folders = [ROOT / f"build/{name}/third_party" for name in ("linux_x86_64", "linux_arm64")]
    return next((folder for folder in folders if (folder / "vulkan-headers").is_dir()), folders[0])


@pytest.mark.parametrize(("name", "sanitizer"), [
    ("host_vr_vulkan_test", None),
    ("host_vr_loading_test", None),
    ("host_vr_loading_test", "thread"),
    ("host_vr_haptics_test", None),
    ("host_vr_haptics_test", "thread"),
])
def test_runtime_binding_and_image_ownership(tmp_path, name, sanitizer):
    if platform.system() != "Linux":
        pytest.skip("Linux OpenXR host")
    cc = next((shutil.which(name) for name in ("clang", "cc") if shutil.which(name)), None)
    if not cc:
        pytest.skip("no C compiler")
    headers = third_party()
    if not (headers / "openxr/openxr/openxr_platform.h").is_file() or not (
            headers / "vulkan-headers/vulkan/vulkan_core.h").is_file():
        pytest.skip("configure.py --vr fetches the headers")
    binary = tmp_path / name
    command = [
        cc, "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function", "-O1", "-D_GNU_SOURCE",
        *([f"-fsanitize={sanitizer}"] if sanitizer else []),
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        f"-I{ROOT / 'port/android/include'}", f"-I{ROOT / 'port/android/host'}",
        f"-I{ROOT / 'port/linux/src'}", f"-I{headers / 'openxr'}", f"-I{headers / 'vulkan-headers'}",
        str(ROOT / "port/linux/tests" / f"{name}.c"), "-pthread", "-ldl", "-lm", "-o", str(binary),
    ]
    try:
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError:
        if sanitizer:
            pytest.skip(f"the compiler cannot build with -fsanitize={sanitizer}")
        raise
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0 and result.stdout.strip() == "ok", result.stdout + result.stderr
    assert "ThreadSanitizer" not in result.stderr, result.stderr


def test_gpu_timestamps_without_waiting(tmp_path):
    if platform.system() != "Linux":
        pytest.skip("Linux Vulkan host")
    cc = next((shutil.which(name) for name in ("clang", "cc") if shutil.which(name)), None)
    if not cc:
        pytest.skip("no C compiler")
    headers = third_party() / "vulkan-headers"
    if not (headers / "vulkan/vulkan_core.h").is_file():
        pytest.skip("configure.py fetches the headers for the Linux build")
    binary = tmp_path / "vulkan_gpu_time_test"
    subprocess.run([
        cc, "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function", "-O1", "-D_GNU_SOURCE",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        f"-I{ROOT / 'port/android/include'}", f"-I{ROOT / 'port/android/host'}", f"-I{headers}",
        f"-I{ROOT / 'port/linux/src'}", str(ROOT / "port/linux/tests/vulkan_gpu_time_test.c"), "-pthread", "-ldl", "-lm", "-o", str(binary),
    ], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0 and result.stdout.strip() == "ok", result.stdout + result.stderr
