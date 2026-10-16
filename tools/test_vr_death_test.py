"""The death test runs before this frame aims, using the completed frame."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

from test_death_timing import function

ROOT = Path(__file__).resolve().parents[1]


def test_death_timer_uses_completed_frame_aim(tmp_path):
    compiler = shutil.which(os.environ.get("CC", "cc"))
    if not compiler:
        pytest.skip("a C compiler is needed")
    source = r"""
#include <assert.h>
#include <string.h>
#define NONE (-1)
#define TRUE 1
#define TICKS_PER_SECOND 30
typedef float real;
static struct {int aimed, aiming;} vr;
static struct {
    int checked, testing, killed, watched, dead, logging;
    long start_tick, frame;
    float seconds, death_seconds;
    char how[16];
} vr_death;
static long tick=300, unit=1;
static int in_game=1, cinematic, kills;
static int halo_vr_aiming(void) {return vr.aiming;}
static void vr_death_read_settings(void) {vr_death.checked=1;}
static int game_in_progress(void) {return in_game;}
static void vr_death_test_watch(void) {}
static long first_player_unit(void) {return unit;}
static long game_time_get(void) {return tick;}
static int cinematic_in_progress(void) {return cinematic;}
static void vr_death_test_blast(long index) {assert(index==unit); kills++;}
static void unit_kill(long index) {assert(index==unit); kills++;}
#define platform_log(...) ((void)0)
"""
    source += function((ROOT / "port/linux/src/vr.c").read_text(), "halo_vr_aimed") + "\n"
    source += function((ROOT / "port/linux/game/vr_death.c").read_text(), "vr_death_test_update")
    source += r"""
int main(void) {
    vr_death.checked=1; vr_death.start_tick=NONE; vr_death.death_seconds=3;
    strcpy(vr_death.how,"kill");
    vr.aimed=1; vr.aiming=0; /* end_frame retained the completed aim */
    vr_death_test_update(); assert(vr_death.start_tick==NONE && !kills);
    vr_death.testing=1; cinematic=1;
    vr_death_test_update(); assert(vr_death.start_tick==NONE);
    cinematic=0; unit=NONE;
    vr_death_test_update(); assert(vr_death.start_tick==NONE);
    unit=1; vr.aimed=0;
    vr_death_test_update(); assert(vr_death.start_tick==NONE);
    vr.aimed=1;
    vr_death_test_update(); assert(vr_death.start_tick==300 && !kills);
    tick=389; vr_death_test_update(); assert(!kills);
    tick=390; vr_death_test_update(); assert(kills==1 && vr_death.killed);
    tick=500; vr_death_test_update(); assert(kills==1);
    return 0;
}
"""
    path = tmp_path / "test.c"
    path.write_text(source)
    binary = tmp_path / "test"
    subprocess.run([compiler, "-std=c99", str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
