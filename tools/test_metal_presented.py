"""Compile the production Metal presentation accounting without a GPU."""
from pathlib import Path
import subprocess

from test_load_trace import build

ROOT = Path(__file__).resolve().parents[1]


def test_loading_hitch_has_no_game_compile_totals(tmp_path):
    source = (ROOT / 'port/macos/host_metal.m').read_text()
    begin = source.index('static void presented(double when,')
    end = source.index('\nstatic void pacing_log(', begin)
    function = source[begin:end]
    executable = build(tmp_path, r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#define HOST_LOG_INFO 1
#define HITCH_SECONDS 0.033
struct xgpu_metal_compile_totals {
    unsigned shaders, pipelines;
    double shader_seconds, pipeline_seconds;
};
static struct {
    double last_presented, started, stats_interval;
    float refresh_rate;
    _Atomic uint64_t missed, shown, interval_sum_us, intervals;
    _Atomic uint64_t interval_minimum_us, interval_maximum_us, late, hitches;
    struct xgpu_metal_compile_totals compiled_before;
} presentation;
static int logs;
static void host_logf(int priority, const char *format, ...) {
    (void)priority; (void)format; logs++;
}
''' + function + r'''
int main(void) {
    struct xgpu_metal_compile_totals first = {2, 3, .1, .2};
    struct xgpu_metal_compile_totals next = {4, 5, .3, .4};
    presentation.stats_interval = 1.0;
    presentation.refresh_rate = 60.0f;
    presented(1.0, 1.0, &first);
    presented(1.05, 1.02, NULL);
    assert(presentation.shown == 2 && presentation.intervals == 1);
    assert(presentation.late == 1 && presentation.hitches == 1 && presentation.missed == 1);
    assert(presentation.interval_maximum_us >= 49999 && !logs);
    assert(presentation.compiled_before.shaders == 2);
    presented(1.10, 1.10, &next);
    assert(presentation.hitches == 2 && logs == 1);
    assert(presentation.compiled_before.shaders == 4);
    presented(0.0, 1.15, NULL);
    assert(presentation.shown == 3 && presentation.intervals == 2);
    return 0;
}
''')
    subprocess.run([str(executable)], check=True, timeout=15)
