"""Loading-screen preparation restores the following draw's actual state."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parent.parent


def test_gpu_prepare_state(tmp_path):
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("needs a native C compiler")
    text = (ROOT / "port/linux/src/d3d8_device.c").read_text()
    start = text.index("void halo_gpu_draw_preserved(")
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    function = text[start:end]
    source = tmp_path / "prepare_state.c"
    source.write_text(r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef unsigned int DWORD;
typedef int BOOL;
typedef int D3DPRIMITIVETYPE;
typedef struct { int value; } D3DBaseTexture;
struct vertex_shader_object { int value; };
#define D3DRS_MAX 183
#define D3DTSS_MAXSTAGES 4
#define D3DTSS_MAX 32
#define XGPU_VERTEX_ATTRIBUTE_COUNT 16
static DWORD D3D__RenderState[D3DRS_MAX];
static DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];
static struct state {
    D3DBaseTexture *textures[D3DTSS_MAXSTAGES];
    struct vertex_shader_object *vertex_shader, *program_slots[8];
    unsigned long program_address;
    float attributes[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
    BOOL immediate_active;
    D3DPRIMITIVETYPE immediate_type;
    unsigned long immediate_written, immediate_count;
    /* Draw resources created by the callback must survive restoration. */
    unsigned int cache_generation;
    void *render_target;
} device;
static unsigned int invalidations, calls;
static void xgpu_backend_invalidate(void) { invalidations++; }
""" + function + r"""
static void prepare(void *data)
{
    unsigned int stage;
    assert(data == &calls);
    calls++;
    memset(D3D__RenderState, 0xee, sizeof(D3D__RenderState));
    memset(D3D__TextureState, 0xdd, sizeof(D3D__TextureState));
    for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
        device.textures[stage] = (D3DBaseTexture *)(uintptr_t)(100 + stage);
    device.vertex_shader = (struct vertex_shader_object *)(uintptr_t)201;
    device.program_slots[0] = (struct vertex_shader_object *)(uintptr_t)202;
    device.program_address = 0;
    memset(device.attributes, 0, sizeof(device.attributes));
    device.immediate_type = 9;
    device.immediate_count = 4;
    device.immediate_written = 0xff;
    device.cache_generation++;
}
int main(void)
{
    struct state expected;
    DWORD render_states[D3DRS_MAX], texture_states[D3DTSS_MAXSTAGES][D3DTSS_MAX];
    unsigned int i, j;
    for (i = 0; i < D3DRS_MAX; i++) D3D__RenderState[i] = 0x12340000 + i;
    for (i = 0; i < D3DTSS_MAXSTAGES; i++) {
        device.textures[i] = (D3DBaseTexture *)(uintptr_t)(i + 1);
        for (j = 0; j < D3DTSS_MAX; j++) D3D__TextureState[i][j] = i * 100 + j;
    }
    for (i = 0; i < XGPU_VERTEX_ATTRIBUTE_COUNT; i++)
        for (j = 0; j < 4; j++) device.attributes[i][j] = i * 4 + j + .5f;
    device.vertex_shader = (struct vertex_shader_object *)(uintptr_t)42;
    device.program_slots[0] = (struct vertex_shader_object *)(uintptr_t)43;
    device.program_slots[3] = (struct vertex_shader_object *)(uintptr_t)44;
    device.program_address = 3;
    device.immediate_type = 7;
    device.immediate_count = 12;
    device.immediate_written = 0x15;
    device.render_target = (void *)(uintptr_t)99;
    expected = device;
    expected.cache_generation++;
    memcpy(render_states, D3D__RenderState, sizeof(render_states));
    memcpy(texture_states, D3D__TextureState, sizeof(texture_states));
    halo_gpu_draw_preserved(prepare, &calls);
    assert(calls == 1 && invalidations == 1);
    assert(!memcmp(&device, &expected, sizeof(device)));
    assert(!memcmp(render_states, D3D__RenderState, sizeof(render_states)));
    assert(!memcmp(texture_states, D3D__TextureState, sizeof(texture_states)));
    /* Never interrupt a partially recorded immediate primitive. */
    device.immediate_active = 1;
    halo_gpu_draw_preserved(prepare, &calls);
    assert(calls == 1 && invalidations == 1 && device.immediate_active == 1);
    device.immediate_active = 0;
    halo_gpu_draw_preserved(0, 0);
    assert(calls == 1 && invalidations == 1);
    return 0;
}
""")
    binary = tmp_path / "prepare_state"
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
