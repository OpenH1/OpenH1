"""The host texture caches let a texture go after half a minute of time without being drawn, at any frame rate
(port/linux/vulkan/xgpu_vulkan_textures.c, port/macos/xgpu_metal_textures.c), and the Vulkan cache keeps the VR
effects' thicknesses while the guest keeps their texels. Each cache's real xgpu_*_textures_frame_end is compiled
against a fake clock and a few entries; no GPU, display or game data is needed.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from harness import function, mutated  # noqa: E402

VULKAN = "port/linux/vulkan/xgpu_vulkan_textures.c"
METAL = "port/macos/xgpu_metal_textures.c"

# the fake world: the cache's members frame_end reads, a clock, an entry per
# texture, and the write tracking's serial and generations
WORLD = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUCKET_COUNT 16
%(config)s
struct image { int destroyed; };
struct entry
{
    struct entry *next;
    /* (the Vulkan cache's image, the Metal cache's texture) */
    struct image *image, *texture, *thickness;
    uint32_t address, size, generation, watched_serial, thickness_generation, thickness_failed;
    uint64_t last_used;
};
static struct
{
    struct entry *buckets[BUCKET_COUNT];
    uint64_t frame;
    uint64_t sweep_frames[SWEEP_COUNT];
    unsigned long sweeps;
    double next_sweep;
    uint32_t thickness_epoch;
} cache;
static double clock_now;
static uint32_t watch_serial = 1, watch_generation = 1;
static double seconds(void) { return clock_now; }
static uint32_t host_memory_watch_serial(void) { return watch_serial; }
static uint32_t host_memory_watch_generation(uint32_t address, uint32_t size)
{
    (void)address;
    (void)size;
    return watch_generation;
}
static void image_destroy(struct image *image) { image->destroyed = 1; }
#define xgpu_vulkan_image_destroy image_destroy
#define xgpu_metal_texture_destroy image_destroy
%(code)s

#define CHECK(condition, ...) \
    do { if (!(condition)) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)

static struct image images[2];
#ifdef THICKNESS
static struct image thickness_image;
#endif
static struct entry *always, *sometimes;

static struct entry *add(int bucket, struct image *image)
{
    struct entry *entry = calloc(1, sizeof(*entry));

    entry->image = entry->texture = image;
    entry->last_used = cache.frame;
    entry->generation = entry->watched_serial = 1;
    entry->size = 4096;
    entry->next = cache.buckets[bucket];
    cache.buckets[bucket] = entry;
    return entry;
}

static int kept(const struct entry *wanted)
{
    int bucket;

    for (bucket = 0; bucket < BUCKET_COUNT; bucket++)
    {
        const struct entry *entry;

        for (entry = cache.buckets[bucket]; entry; entry = entry->next)
        {
            if (entry == wanted)
                return 1;
        }
    }
    return 0;
}

/* frames at a rate from a time to another (seconds), the one texture drawn
every frame and the other until seen_until: the seconds after it was last
drawn that it was let go (-1: kept) */
static double play(int rate, double from, double to, double seen_until)
{
    long frame;

    for (frame = (long)(from * rate); frame < (long)(to * rate); frame++)
    {
        clock_now = (double)frame / rate;
        if (kept(sometimes) && clock_now < seen_until)
            sometimes->last_used = cache.frame;
        always->last_used = cache.frame;
        %(frame_end)s();
        CHECK(kept(always), "the texture drawn every frame was let go at %%.1f s", clock_now);
        if (!kept(sometimes))
            return clock_now - seen_until;
    }
    return -1.0;
}

static void check_idle(double idle)
{
    /* (a look every 10 s lets go what was not drawn since the look 30 s
    before it: 30 to 40 s after) */
    CHECK(idle >= 30.0 && idle <= 41.0, "let go %%.1f s after it was last drawn, not 30 to 40", idle);
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "";

    always = add(3, &images[0]);
    sometimes = add(9, &images[1]);
    if (!strcmp(name, "fast"))
        /* at 360 frames a second, a texture out of sight for five seconds
        (1800 frames) is kept */
        check_idle(play(360, 0, 60, 1));
    else if (!strcmp(name, "steady"))
        /* at 60 frames a second, as it was in frames: half a minute */
        check_idle(play(60, 0, 80, 1));
    else if (!strcmp(name, "frame"))
        /* at the Steam Frame's 120, after the looks have gone round */
        check_idle(play(120, 0, 90, 25));
    else if (!strcmp(name, "pause"))
    {
        /* no frames for a hundred seconds (the headset taken off, the app in
        the background): what was drawn before is kept when they come back */
        double at;

        CHECK(play(120, 0, 25, 30) < 0, "let go before the pause");
        /* (not drawn again: let go after half a minute of frames) */
        at = play(120, 125, 200, 0);
        CHECK(at >= 0, "kept");
        check_idle(at - 125);
    }
#ifdef THICKNESS
    else if (!strcmp(name, "thickness_kept"))
    {
        /* an effect's bitmap with its thickness, baked as the map was
        prepared and not drawn since: kept while its pages are not written,
        though other pages are (the serial moves on) */
        sometimes->thickness = &thickness_image;
        sometimes->thickness_generation = 1;
        CHECK(play(120, 0, 60, 1) < 0, "the thickness let go with its pages as they were");
        watch_serial++;
        CHECK(play(120, 60, 120, 1) < 0, "the thickness let go though its pages were not written");
        CHECK(!thickness_image.destroyed && cache.thickness_epoch == 0, "the thickness destroyed");
    }
    else if (!strcmp(name, "thickness_written"))
    {
        /* kept, then its pages written (the next map's textures read into
        them): let go at the next look, its thickness destroyed and the epoch
        moved on, so that the stream looks it up again */
        double idle;

        sometimes->thickness = &thickness_image;
        sometimes->thickness_generation = 1;
        CHECK(play(120, 0, 60, 1) < 0, "the thickness let go with its pages as they were");
        watch_serial++;
        watch_generation++;
        idle = play(120, 60, 120, 1);
        CHECK(idle >= 59.0 && idle <= 70.0, "let go %%.1f s after it was last drawn, not at the next look", idle);
        CHECK(thickness_image.destroyed && cache.thickness_epoch == 1, "the thickness not destroyed with it");
    }
    else if (!strcmp(name, "thickness_stale"))
    {
        /* a thickness of texels read again since (to be baked again): let go
        as any texture */
        sometimes->thickness = &thickness_image;
        sometimes->thickness_generation = 1;
        sometimes->generation = 2;
        check_idle(play(120, 0, 60, 1));
        CHECK(thickness_image.destroyed && cache.thickness_epoch == 1, "the thickness not destroyed with it");
    }
#endif
    else
    {
        fprintf(stderr, "no case %%s\n", name);
        return 2;
    }
    return 0;
}
"""

CASES = ["fast", "steady", "frame", "pause"]
THICKNESS_CASES = ["thickness_kept", "thickness_written", "thickness_stale"]


def compiler():
    return os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")


def generated(path, fault=None):
    source = (ROOT / path).read_text()
    config = "\n".join(re.findall(r"^#define (?:SWEEP|IDLE)_\w+.*$", source, re.M))
    vulkan = path == VULKAN
    frame_end = "xgpu_vulkan_textures_frame_end" if vulkan else "xgpu_metal_textures_frame_end"
    code = function(source, frame_end)
    if vulkan:
        code = "\n".join([function(source, "thickness_forget"), function(source, "thickness_keeps"), code])
    if fault:
        code = mutated(code, *fault)
    return (("#define THICKNESS\n" if vulkan else "") +
            WORLD % {"config": config, "code": code, "frame_end": frame_end})


def run(tmp_path, path, case, fault=None):
    cc = compiler()
    if not cc:
        pytest.skip("needs a C compiler")
    name = Path(path).stem
    source = tmp_path / f"{name}.c"
    source.write_text(generated(path, fault))
    binary = tmp_path / name
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(source),
                    "-o", str(binary)], check=True)
    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=60)
    return result.returncode, result.stdout + result.stderr


@pytest.mark.parametrize("path", [VULKAN, METAL])
@pytest.mark.parametrize("case", CASES)
def test_idle_by_time(tmp_path, path, case):
    status, output = run(tmp_path, path, case)
    assert status == 0, output


@pytest.mark.parametrize("case", THICKNESS_CASES)
def test_thickness_kept_with_its_texels(tmp_path, case):
    status, output = run(tmp_path, VULKAN, case)
    assert status == 0, output


@pytest.mark.parametrize("path", [VULKAN, METAL])
@pytest.mark.parametrize("fault,case", [
    # a look every frame
    (("cache.next_sweep = now + SWEEP_SECONDS;", "cache.next_sweep = now;"), "fast"),
    # the look's own frame taken as the one half a minute before
    (("\tidle_since = cache.sweep_frames[cache.sweeps % SWEEP_COUNT];\n"
      "\tcache.sweep_frames[cache.sweeps % SWEEP_COUNT] = cache.frame;\n",
      "\tcache.sweep_frames[cache.sweeps % SWEEP_COUNT] = cache.frame;\n"
      "\tidle_since = cache.sweep_frames[cache.sweeps % SWEEP_COUNT];\n"), "fast"),
    # the looks not counted: the one before taken each time
    (("\tcache.sweeps++;\n", "\n"), "frame"),
])
def test_negative_control(tmp_path, path, fault, case):
    status, output = run(tmp_path, path, case, fault)
    assert status == 1, output


@pytest.mark.parametrize("fault,case", [
    # thicknesses let go with their textures
    (("!thickness_keeps(entry)", "1"), "thickness_kept"),
    # kept whatever the guest writes
    (("entry->watched_serial == host_memory_watch_serial() ||", "1 ||"), "thickness_written"),
    # kept though baked from texels read before
    (("entry->thickness_generation == entry->generation", "1"), "thickness_stale"),
    # the epoch not moved on as one is let go
    (("\t\tcache.thickness_epoch++;\n", "\n"), "thickness_written"),
])
def test_negative_control_thickness(tmp_path, fault, case):
    status, output = run(tmp_path, VULKAN, case, fault)
    assert status == 1, output
