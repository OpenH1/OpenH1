"""Tests of how configure.py generates the desktop guest builds
(tools/linux_guest_build.py): the 64-bit x86 one, not its computer's default
build, downloads nothing unless asked for, and a build that cannot be
generated (here, a compiler that does not run) is a target that stops with
the reason instead of stopping configure.py for every target."""
import io
import json
import shutil
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tools import linux_guest_build  # noqa: E402
from tools.ninja_syntax import Writer  # noqa: E402

X86_64 = next(target for target in linux_guest_build.LINUX_GUESTS if target.name == "linux_x86_64")


@pytest.fixture
def tree(tmp_path, monkeypatch):
    """a folder that configure.py runs in, with the build's settings and the
    system's OpenGL headers, on a 64-bit x86 computer; fetches are recorded"""
    (tmp_path / "port" / "linux" / "host").mkdir(parents=True)
    shutil.copy(ROOT / "port" / "linux" / "port.json", tmp_path / "port" / "linux" / "port.json")
    headers = tmp_path / "gl"
    for name in ("GLES3/gl32.h", "GLES2/gl2ext.h", "EGL/egl.h", "KHR/khrplatform.h", "GL/glcorearb.h"):
        (headers / name).parent.mkdir(parents=True, exist_ok=True)
        (headers / name).write_text("")
    monkeypatch.chdir(tmp_path)
    monkeypatch.setattr(linux_guest_build, "GL_HEADERS", headers)
    monkeypatch.setattr(linux_guest_build, "native_linux_guest", lambda: X86_64)
    fetches = []
    monkeypatch.setattr(linux_guest_build, "fetch_third_party", lambda third_party: fetches.append(third_party))
    return fetches


def generate(capsys, **settings):
    output = io.StringIO()
    linux_guest_build.generate_linux_guest_build(Writer(output), SimpleNamespace(**settings), X86_64)
    return output.getvalue(), capsys.readouterr().err


def test_not_asked_for_downloads_nothing(tree, capsys):
    text, err = generate(capsys)
    assert tree == []
    assert "build linux_x86_64: linux_x86_64_unavailable" in text and "--linux-x86-64" in text
    # (quietly: every configure.py of an x86 computer's default build gets here)
    assert err == ""


def test_fetched_sources_are_asked_for(tree, tmp_path):
    third_party = tmp_path / "third_party"
    assert not linux_guest_build.requested(SimpleNamespace(), X86_64, third_party)
    (third_party / f"musl-{linux_guest_build.MUSL_VERSION}").mkdir(parents=True)
    (third_party / "SDL3").mkdir()
    assert linux_guest_build.requested(SimpleNamespace(), X86_64, third_party)
    assert linux_guest_build.requested(SimpleNamespace(linux_x86_64=True), X86_64, tmp_path / "none")
    assert linux_guest_build.requested(SimpleNamespace(port_vr=True), X86_64, tmp_path / "none")


def test_compiler_that_cannot_run_is_this_target_unavailable(tree, capsys):
    text, err = generate(capsys, linux_x86_64=True, linux_x86_64_cc="/nonexistent/clang")
    assert tree == [Path("build/linux_x86_64/third_party")]
    assert "(linux_x86_64) unavailable" in err and "/nonexistent/clang" in err
    # (only the stopping target: nothing of the build written halfway)
    builds = [line for line in text.splitlines() if line.startswith("build ")]
    assert builds == ["build linux_x86_64: linux_x86_64_unavailable"], text


def test_pinned_llvm_that_cannot_be_fetched_is_this_target_unavailable(tree, capsys, monkeypatch):
    def unavailable(sln, explicit_cc):
        assert explicit_cc is None
        raise RuntimeError("no network")
    monkeypatch.setattr(linux_guest_build, "guest_toolchain", unavailable)
    text, err = generate(capsys, linux_x86_64=True)
    assert "(linux_x86_64) unavailable" in err and "no network" in err
    builds = [line for line in text.splitlines() if line.startswith("build ")]
    assert builds == ["build linux_x86_64: linux_x86_64_unavailable"], text


@pytest.mark.parametrize("native", linux_guest_build.LINUX_GUESTS, ids=lambda target: target.name)
def test_vulkan_uses_native_guest(tree, monkeypatch, native):
    """The Vulkan target keeps a separate output folder, using this host's
    ILP32 guest, and does not become an implicit download just because VR is on."""
    generated = []
    monkeypatch.setattr(linux_guest_build, "native_linux_guest", lambda: native)
    monkeypatch.setattr(linux_guest_build, "_generate", lambda n, sln, target, config, third_party:
                        generated.append((target, third_party)))
    linux_guest_build.generate_linux_guest_builds(Writer(io.StringIO()), SimpleNamespace(port_vr=True))
    assert not any(target.renderer == "vulkan" for target, _ in generated)
    generated.clear()
    linux_guest_build.generate_linux_guest_builds(Writer(io.StringIO()),
                                                 SimpleNamespace(linux_vulkan=True, port_vr=True))
    vulkan, folder = next(entry for entry in generated if entry[0].renderer == "vulkan")
    assert vulkan.name == "linux_vulkan" and vulkan.arch == native.arch
    assert vulkan.machines == native.machines and not vulkan.default
    assert folder == Path("build/linux_vulkan/third_party")


@pytest.mark.parametrize("vr", [False, True], ids=["desktop", "openxr"])
def test_vulkan_vr_build_has_runtime_and_guest_imports(tree, monkeypatch, vr):
    """Both sides of the guest ABI and the runtime must agree on the renderer
    and VR, including Vulkan types used by the OpenXR graphics binding."""
    from dataclasses import replace
    native = linux_guest_build.LINUX_GUESTS[0]
    target = replace(native, name="linux_vulkan", renderer="vulkan", default=False)
    monkeypatch.setattr(linux_guest_build, "native_linux_guest", lambda: native)
    monkeypatch.setattr(linux_guest_build, "guest_toolchain", lambda sln, cc: None)
    fetched = []
    monkeypatch.setattr(linux_guest_build, "fetch_openxr_headers", lambda path: fetched.append("openxr"))
    monkeypatch.setattr(linux_guest_build, "fetch_vulkan_headers", lambda path: fetched.append("vulkan"))
    linux_guest_build.VULKAN_DIR.mkdir(parents=True)
    for name in ("host_vulkan.c", "host_vulkan_vr.c", "xgpu_vulkan_vr_shaders.c"):
        (linux_guest_build.VULKAN_DIR / name).touch()
    guest = {}
    def image(n, sln, config, **settings):
        guest.update(settings)
        return {"host_table_c": "imports.c", "image": "guest.bin", "checked": "guest.checked"}
    monkeypatch.setattr(linux_guest_build, "generate_guest_image", image)
    output = io.StringIO()
    linux_guest_build.generate_linux_guest_build(Writer(output),
        SimpleNamespace(linux_vulkan=True, port_vr=vr), target)
    graph = output.getvalue()
    assert guest["arch"] == native.arch
    assert "-DHALO_VULKAN=1" in guest["platform_defines"]
    assert ("-DHALO_VR=1" in guest["platform_defines"]) == vr
    assert (Path("port/linux/host/vr_imports.list") in guest["extra_imports"]) == vr
    assert Path("port/linux/vulkan/host_imports.list") in guest["extra_imports"]
    assert ("port/linux/host/host_vr.c" in graph) == vr
    assert ("port/linux/host/host_gl_vr.c" in graph) == vr
    assert "-Ibuild/linux_vulkan/third_party/vulkan-headers" in graph
    assert "-Iport/linux/host" in graph
    assert fetched == (["openxr", "vulkan"] if vr else ["vulkan"])
    assert "build linux_vulkan: phony" in graph
    assert ("port/linux/vulkan/host_vulkan_vr.c" in graph) == vr
    assert ("port/linux/vulkan/xgpu_vulkan_vr_shaders.c" in graph) == vr
