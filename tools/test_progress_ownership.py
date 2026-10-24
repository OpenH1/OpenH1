"""Extract production ownership gates; no game assets, GPU or copied predicates."""
from pathlib import Path
import subprocess
from test_load_trace import build

ROOT = Path(__file__).resolve().parents[1]


def function(path, signature):
    text = (ROOT / path).read_text()
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def run(tmp_path, source):
    subprocess.run([str(build(tmp_path, source))], check=True, timeout=15)


def test_progress_preserves_math_clock_and_audio_while_skipping_gpu(tmp_path):
    text = function('source/interface/progress_bar.c', 'static void progress_bar_render(\n\treal progress)\n{')
    # All production code through the guard is compiled, not reimplemented.
    prefix = text[:text.index('\tIDirect3DDevice8_GetTransform')]
    run(tmp_path, r'''
#include <assert.h>
#include <string.h>
typedef float real;
typedef struct { real red, green, blue; } real_rgb_color;
typedef int D3DBaseTexture;
typedef int D3DSurface;
#define NUMBER_OF_PROGRESS_BAR_SOUNDS 4
static struct { int sounds[4]; } progress_bar_globals = {{1,2,3,4}};
static real wobble_phase, last_t;
static unsigned long current_time;
static int owned, gpu, clocks, queries, audio, volume[4];
static double fake_sin(double value) { return 0.5 + value * 0.001; }
static double fake_pow(double value, double exponent) { return value * exponent; }
#define sin fake_sin
#define pow fake_pow
static unsigned long GetTickCount(void) { clocks++; return 123456; }
static int halo_loading_owned(void) { queries++; return owned; }
static int progress_calls; static real progress_told;
static void halo_loading_progress(float value) { progress_calls++; progress_told = value; }
static void IDirectSoundBuffer_SetVolume(int sound, long value) { audio++; volume[sound-1]=(int)value; }
''' + prefix + r'''
    (void)back_buffer_texture; (void)back_buffer; (void)stage; (void)color;
    gpu++;
}
int main(void) {
    int original[4], i;
    real original_wobble, original_progress;
    owned=0; wobble_phase=0.3f; last_t=0.2f;
    progress_bar_render(0.6f);
    assert(gpu==1 && clocks==1 && queries==1 && audio==4 && current_time==123456);
    memcpy(original,volume,sizeof(original)); original_wobble=wobble_phase; original_progress=last_t;
    owned=1; wobble_phase=0.3f; last_t=0.2f;
    progress_bar_render(0.6f);
    assert(gpu==1 && clocks==2 && queries==2 && audio==8 && current_time==123456);
    assert(wobble_phase==original_wobble && last_t==original_progress);
    /* the presenter is told the smoothed progress the draws would fade with */
    assert(progress_calls==2 && progress_told==last_t);
    for(i=0;i<4;i++) assert(volume[i]==original[i]);
    owned=0; progress_bar_render(0.7f); assert(gpu==2);
    return 0;
}
''')


def test_current_query_falls_back_after_revocation_and_ready(tmp_path):
    query = function('port/linux/src/sdl_platform.c', 'int halo_loading_owned(void)')
    run(tmp_path, r'''
#include <assert.h>
static int loading_presentation_active, backend_owned, queried;
static int xgpu_backend_loading_owned(void) { queried++; return backend_owned; }
''' + query + r'''
int main(void) {
    backend_owned=1; assert(!halo_loading_owned() && !queried);
    loading_presentation_active=1; assert(halo_loading_owned() && queried==1);
    backend_owned=0; assert(!halo_loading_owned() && queried==2);
    backend_owned=1; assert(halo_loading_owned() && queried==3);
    loading_presentation_active=2; assert(!halo_loading_owned() && queried==3);
    loading_presentation_active=0; assert(!halo_loading_owned() && queried==3);
    return 0;
}
''')


def test_stream_reply_uses_ownership_not_scheduling_interval(tmp_path):
    text = (ROOT / 'port/android/host/host_render_stream.c').read_text()
    case = text[text.index('\tcase XGPU_STREAM_LOADING_OWNED:'):text.index('\tcase XGPU_STREAM_LOADING:\n')]
    run(tmp_path, r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define XGPU_STREAM_LOADING_OWNED 1
struct xgpu_stream_reply_record { uint32_t reply; };
struct backend { int (*loading_owned)(void *); };
static int holds, admitted, calls;
static uint32_t result;
static int host_render_stream_loading_holds(void) { return holds; }
static int callback(void *context) { (void)context; calls++; return admitted; }
static void answer(uint32_t reply, const void *value, size_t size) {
    assert(reply==42 && size==sizeof(result)); memcpy(&result,value,size);
}
static void decode(const struct backend *backend) {
    struct xgpu_stream_reply_record record={42};
    const unsigned char *start=(const unsigned char *)&record;
    void *context=0;
    switch(XGPU_STREAM_LOADING_OWNED) {
''' + case + r'''
    }
}
int main(void) {
    struct backend backend={callback};
    holds=1; admitted=0; decode(&backend); assert(!result && calls==1);
    admitted=1; decode(&backend); assert(result && calls==2);
    holds=0; decode(&backend); assert(!result && calls==2);
    holds=1; backend.loading_owned=0; decode(&backend); assert(!result && calls==2);
    return 0;
}
''')


def test_vr_waiter_failure_or_session_loss_revokes_ownership(tmp_path):
    query = function('port/linux/host/host_vr.c', 'int host_vr_loading_owned(void)')
    run(tmp_path, r'''
#include <assert.h>
#define VR_SWAPCHAIN_HUD 0
static struct { int lock,on; } render_thread;
static struct { int active,ready,suspended,waiter_started,waiter_failed,waiter_stop; } loading;
static struct { int running,session,swapchains[1]; } vr;
static void pthread_mutex_lock(int *lock) { assert(!*lock); *lock=1; }
static void pthread_mutex_unlock(int *lock) { assert(*lock); *lock=0; }
''' + query + r'''
int main(void) {
    render_thread.on=loading.active=loading.waiter_started=vr.running=vr.session=vr.swapchains[0]=1;
    assert(host_vr_loading_owned());
    loading.waiter_failed=1; assert(!host_vr_loading_owned()); loading.waiter_failed=0;
    loading.waiter_started=0; assert(!host_vr_loading_owned()); loading.waiter_started=1;
    loading.suspended=1; assert(!host_vr_loading_owned()); loading.suspended=0;
    loading.ready=1; assert(!host_vr_loading_owned()); loading.ready=0;
    loading.waiter_stop=1; assert(!host_vr_loading_owned()); loading.waiter_stop=0;
    vr.session=0; assert(!host_vr_loading_owned());
    assert(!render_thread.lock);
    return 0;
}
''')


def test_metal_requires_started_presenter_and_live_hold(tmp_path):
    query = function('port/macos/host_metal.m', 'int host_metal_loading_owned(void *context)')
    run(tmp_path, r'''
#include <assert.h>
#define atomic_load(value) (*(value))
static struct { int lock,active,loading_pipeline; struct { int active,ready; } loading; } presentation;
static void pthread_mutex_lock(int *lock) { assert(!*lock); *lock=1; }
static void pthread_mutex_unlock(int *lock) { assert(*lock); *lock=0; }
''' + query + r'''
int main(void) {
    presentation.loading.active=1; assert(!host_metal_loading_owned(0));
    presentation.loading_pipeline=1; assert(!host_metal_loading_owned(0));
    presentation.active=1; assert(host_metal_loading_owned(0));
    presentation.loading.ready=1; assert(!host_metal_loading_owned(0));
    presentation.loading.ready=0; presentation.active=0; assert(!host_metal_loading_owned(0));
    assert(!presentation.lock); return 0;
}
''')


def test_vulkan_requires_image_and_window_resources(tmp_path):
    query = function('port/linux/vulkan/xgpu_vulkan.c', 'int xgpu_vulkan_loading_resources(int window)')
    run(tmp_path, r'''
#include <assert.h>
static struct { int loading_image,surface,swapchain; } vk;
''' + query + r'''
int main(void) {
    assert(!xgpu_vulkan_loading_resources(0));
    vk.surface=vk.swapchain=1; assert(!xgpu_vulkan_loading_resources(1));
    vk.loading_image=1; assert(xgpu_vulkan_loading_resources(1));
    vk.swapchain=0; assert(!xgpu_vulkan_loading_resources(1));
    assert(xgpu_vulkan_loading_resources(0));
    vk.loading_image=0; assert(!xgpu_vulkan_loading_resources(0));
    return 0;
}
''')
