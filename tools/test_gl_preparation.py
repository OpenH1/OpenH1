"""Fixed compositor constructors run before gameplay without drawing a picture."""
from pathlib import Path
import re
import subprocess

from test_load_trace import build

ROOT = Path(__file__).resolve().parents[1]


def test_fixed_compositor_preparation_without_runtime_images(tmp_path):
    text = (ROOT / "port/linux/src/xgpu_gl.c").read_text()
    functions = []
    for signature, result in [
        ("static void vr_fovea_composite(BOOL prepare_only)", ""),
        ("static GLuint vr_light_map(const struct vr_visor_hud_image *look, BOOL prepare_only)", "0"),
        ("static BOOL vr_copy_scope(GLuint source, GLuint image, int size, BOOL prepare_only)", "TRUE"),
    ]:
        start = text.index(signature)
        stop = text.index("\tif (prepare_only)\n\t\treturn" + (" " + result if result else "") + ";", start)
        stop = text.index(";", stop) + 1
        # Keep the actual constructor and all its guards, ending at the
        # explicit prepare-only return. Any fallthrough fails this probe.
        functions.append(text[start:stop] + "\n abort();\n}\n")
    functions = "\n".join(functions)
    calls = sorted(set(re.findall(r"\b(gl[A-Z]\w*)\s*\(", functions)))
    special = {"glCreateProgram", "glGetProgramiv", "glGetUniformLocation"}
    macros = "\n".join(f"#define {name}(...) ((void)0)" for name in calls if name not in special)
    source = r'''
#include <assert.h>
#include <stdlib.h>
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
typedef int BOOL;
typedef unsigned GLuint;
typedef int GLint;
typedef float GLfloat;
#define TRUE 1
#define FALSE 0
#define GL_VERTEX_SHADER 1
#define GL_FRAGMENT_SHADER 2
#define SCOPE_TEXT_LENGTH 28
struct vr_visor_hud_image { float react; int curve; };
struct halo_vr_scope_window { int unused; };
static struct {
    int foveated, eyes_drawn; GLuint eye_image;
    struct { struct { GLuint texture; } target; } eye_target;
} vr_gl;
static unsigned shaders, programs, waits;
static GLuint xgpu_compile_shader(int type, const char *source, const char *name) {
    assert(type && source && name); return ++shaders;
}
static void vr_eye_target(int eye) { (void)eye; abort(); }
#define glCreateProgram() (++programs)
#define glGetProgramiv(program, parameter, status) (*(status) = 1)
#define glGetUniformLocation(...) 1
#define gl_load_trace(...) ((void)0)
#define gl_compilation_wait(...) (++waits)
#define platform_log(...) ((void)0)
#define xgpu_gl_state_invalidate() ((void)0)
''' + macros + "\n" + functions + r'''
int main(void) {
    vr_gl.foveated = 1;
    /* No eye image, eye draw, HUD look, scope image or gameplay state. */
    vr_fovea_composite(TRUE);
    assert(vr_copy_scope(0, 0, 0, TRUE));
    assert(vr_light_map(NULL, TRUE) == 0);
    assert(shaders == 6 && programs == 3 && waits == 3);
    vr_fovea_composite(TRUE);
    assert(vr_copy_scope(0, 0, 0, TRUE));
    assert(vr_light_map(NULL, TRUE) == 0);
    assert(shaders == 6 && programs == 3 && waits == 3);
    return 0;
}
'''
    subprocess.run([str(build(tmp_path, source))], check=True, timeout=15)


def test_host_gl_forwards_preparation_barrier():
    text = (ROOT / "port/linux/host/host_gl_stream.c").read_text()
    assert ".prepare = stream_prepare" in text
    assert ".prepared = stream_prepared" in text
    assert "xgpu_backend_prepare();" in text
    assert "xgpu_backend_prepared() * 1000.0" in text
