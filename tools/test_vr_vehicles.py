"""The VR mode's vehicles: the eye's way into a seat and out of it (port/linux/game/vr_seat.c),
and steering with the sticks while seated (port/linux/src/vr.c), compiled from the sources
against a fake world.

- Getting in and out of a vehicle by the action (X) with vr.seat_motion = "still" (the default)
  stays in the first person: the view never darkens, the eye never moves faster than the ride's
  limit (3 m/s) however the game's climbing animation jumps the body's head, it ends in the seat
  (where the head is once seated, held there in the vehicle as it drives), and ends back at the
  first person's eye once out. With "blink" (the older way) it darkens, as before.
- Back from a death (a respawn, a checkpoint's revert), the eye is the new one's from the first frame: not the
  death's eye (its cam's vantage, or where the player died) held in view while a blink darkens, outside the head
  as the helmet's view switches on (port/linux/game/vr_death.c calls vr_seat_eye_cut).
- With the hands off a Ghost's handlebars or a Warthog's wheel, a stick pushed sideways steers:
  the Ghost turns toward the push at up to 90 degrees a second (the game's own turn toward the
  facing, update_alien_scout_physics), the Warthog's wheels turn toward their lock, the other way
  as it reverses.
"""
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SEAT = ROOT / "port/linux/game/vr_seat.c"
VR = ROOT / "port/linux/src/vr.c"


def function(source, name):
    match = re.search(r"^(?:static )?[\w *]+\b" + name + r"\(\s*[^;{]*\)\s*\{", source, re.M)
    if not match:
        raise ValueError(f"function not found: {name}")
    start = source.index("{", match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def between(source, start, end):
    first = source.index(start)
    return source[first:source.index(end, first)]


def defines(source, *names):
    out = []
    for name in names:
        match = re.search(r"^#define " + name + r"\b.*$", source, re.M)
        if not match:
            raise ValueError(f"constant not found: {name}")
        out.append(match.group(0))
    return "\n".join(out) + "\n"


def compiler():
    found = next((shutil.which(name) for name in (os.environ.get("CC", "clang"), "clang", "cc") if shutil.which(name)),
                 None)
    if not found:
        pytest.skip("a C compiler is needed")
    return found


def build(tmp_path, source, name):
    c = tmp_path / f"{name}.c"
    exe = tmp_path / name
    c.write_text(source)
    subprocess.run([compiler(), "-std=gnu99", "-O1", "-w", "-Werror=implicit-function-declaration", str(c), "-o",
                    str(exe), "-lm"], check=True)
    return exe


# ---------- the eye's way into a seat and out (vr_seat.c)

SEAT_WORLD = r"""
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int boolean;
typedef float real;
typedef struct {real x,y,z;} real_point3d;
typedef struct {real i,j,k;} real_vector3d;
typedef struct {real scale; real_vector3d forward,left,up; real_point3d position;} real_matrix4x3;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define PIN(x,lo,hi) ((x)<(lo)?(lo):((x)>(hi)?(hi):(x)))
enum {_unit_state_seated = 1, _unit_state_entering_seat, _unit_state_exiting_seat};
enum {_director_perspective_first_person = 0, _director_perspective_following = 1};
struct unit_datum {struct {long parent_object_index;} object; struct {short parent_seat_index; struct {short state;} animation;} unit;};

static struct unit_datum the_unit;
static real_matrix4x3 vehicle_node = {1.0f, {1,0,0}, {0,1,0}, {0,0,1}, {0,0,0}};
static real_point3d the_head;
static const char *seat_motion_setting = "still";
static unsigned long changes = 1;
static real vignette_asked;
static int vehicle_gone;

static const char *config_string(const char *name) {(void)name; return seat_motion_setting;}
static unsigned long config_changes(void) {return changes;}
#define platform_log(...) ((void)0)
static struct unit_datum *unit_get(long index) {(void)index; return &the_unit;}
static void *object_try_and_get(long index) {return index == 7 && !vehicle_gone ? &vehicle_node : NULL;}
static real_matrix4x3 const *object_get_node_matrix(long index, short node) {(void)index; (void)node; return &vehicle_node;}
static void unit_get_head_position(long index, real_point3d *head) {(void)index; *head = the_head;}
static short director_get_perspective(short local) {(void)local; return _director_perspective_first_person;}
static short vr_cabin_seat_eye(long index, real_point3d *eye) {(void)index; (void)eye; return NONE;}
static boolean vr_cabin_feed(short index) {return index != NONE;}
static boolean vr_climb_exiting(void) {return FALSE;}
static boolean vr_climb_active(void) {return FALSE;}
static void halo_vr_comfort_vignette(float amount) {vignette_asked = amount;}
static real dot(real_vector3d const *a, real_vector3d const *b) {return a->i*b->i + a->j*b->j + a->k*b->k;}
static real magnitude_squared3d(real_vector3d const *v) {return dot(v, v);}
static real distance3d(real_point3d const *a, real_point3d const *b)
{real x = a->x-b->x, y = a->y-b->y, z = a->z-b->z; return sqrtf(x*x + y*y + z*z);}
static void vector_from_points3d(real_point3d const *a, real_point3d const *b, real_vector3d *v)
{v->i = b->x-a->x; v->j = b->y-a->y; v->k = b->z-a->z;}
static void scale_vector3d(real_vector3d const *v, real s, real_vector3d *out) {out->i = v->i*s; out->j = v->j*s; out->k = v->k*s;}
static void point_from_line3d(real_point3d const *p, real_vector3d const *v, real t, real_point3d *out)
{out->x = p->x + v->i*t; out->y = p->y + v->j*t; out->z = p->z + v->k*t;}
static void matrix4x3_transform_vector(real_matrix4x3 const *m, real_vector3d const *v, real_vector3d *out)
{real_vector3d r; r.i = m->forward.i*v->i + m->left.i*v->j + m->up.i*v->k; r.j = m->forward.j*v->i + m->left.j*v->j + m->up.j*v->k;
 r.k = m->forward.k*v->i + m->left.k*v->j + m->up.k*v->k; *out = r;}
static void matrix4x3_inverse_transform_vector(real_matrix4x3 const *m, real_vector3d const *v, real_vector3d *out)
{real_vector3d r; r.i = dot(&m->forward, v); r.j = dot(&m->left, v); r.k = dot(&m->up, v); *out = r;}
static void matrix4x3_transform_point(real_matrix4x3 const *m, real_point3d const *p, real_point3d *out)
{real_vector3d v = {p->x, p->y, p->z}; matrix4x3_transform_vector(m, &v, &v);
 out->x = v.i + m->position.x; out->y = v.j + m->position.y; out->z = v.k + m->position.z;}
static void matrix4x3_inverse_transform_point(real_matrix4x3 const *m, real_point3d const *p, real_point3d *out)
{real_vector3d v = {p->x - m->position.x, p->y - m->position.y, p->z - m->position.z};
 matrix4x3_inverse_transform_vector(m, &v, &v); out->x = v.i; out->y = v.j; out->z = v.k;}
"""

SEAT_STUBS = r"""
static void feed_ride(long unit_index, long vehicle_index, real_point3d const *was, real_point3d const *head,
    real_point3d const *cockpit_eye, real_point3d *eye, real seconds) {abort();}
static boolean ride_arrived(long vehicle_index, real_point3d const *place) {abort();}
"""

SEAT_MAIN = r"""
#define DT (1.0f / 60.0f)
static real_point3d eye_was;
static int frames, dark_frames;
static real largest_step, darkest;

/* a frame: the game's camera (the first person's eye) and the unit seated (else NONE) */
static real_point3d frame(long seated, real_point3d camera)
{
    real_point3d eye = camera;
    vr_seat_eye(seated, TRUE, DT, &eye);
    if (frames++ > 0 && !vehicle_moving)
        largest_step = MAX(largest_step, distance3d(&eye_was, &eye));
    if (vr_seat_darkness() > 0.0f)
        dark_frames++;
    darkest = MAX(darkest, vr_seat_darkness());
    eye_was = eye;
    return eye;
}

int main(int argc, char **argv)
{
    real_point3d stand = {0.0f, 0.0f, 0.6f}, seat = {1.0f, 0.3f, 0.5f}, out = {2.0f, 0.0f, 0.6f}, eye, start;
    int i, moving = argc > 2 && !strcmp(argv[2], "moving");
    real limit = RIDE_FASTEST / METRES_PER_WORLD_UNIT * DT * 1.001f;

    seat_motion_setting = argv[1];
    the_unit.object.parent_object_index = 7;
    the_unit.unit.parent_seat_index = 0;
    /* on foot */
    for (i = 0; i < 30; i++)
        eye = frame(NONE, stand);
    /* climbing in: the game's animation, the head in half-metre steps every few frames, and a bob */
    the_unit.unit.animation.state = _unit_state_entering_seat;
    for (i = 0; i < 54; i++)
    {
        real t = (real)(i / 6 * 6) / 54.0f;
        the_head.x = stand.x + (seat.x - stand.x) * t;
        the_head.y = stand.y + (seat.y - stand.y) * t;
        the_head.z = stand.z + (seat.z - stand.z) * t + 0.08f * (real)((i / 3) % 2);
        eye = frame(7, stand);
    }
    /* seated: the seat's animation moves the head about; the eye stays where it was seated, in the
    vehicle, as the vehicle drives on */
    the_unit.unit.animation.state = _unit_state_seated;
    the_head = seat;
    eye = frame(7, stand);
    for (i = 0; i < 120; i++)
    {
        the_head.z = seat.z + 0.05f * sinf((real)i);
        if (moving && i >= 60)
        {
            vehicle_moving = TRUE;
            vehicle_node.position.x += 3.0f * DT;
        }
        eye = frame(7, stand);
        if (i == 59)
            printf("seated %f %f %f shown %d\n", eye.x, eye.y, eye.z, vr_seat_seated_shown());
    }
    if (moving)
        printf("moved seated %f %f %f\n", eye.x - vehicle_node.position.x, eye.y - vehicle_node.position.y,
            eye.z - vehicle_node.position.z);
    vehicle_moving = FALSE;
    frames = 0;
    /* climbing out: from the seat to beside the vehicle (wherever it drove to), in steps, the head in
    the world */
    the_unit.unit.animation.state = _unit_state_exiting_seat;
    point_from_line3d(&seat, (real_vector3d *)&vehicle_node.position, 1.0f, &start);
    point_from_line3d(&out, (real_vector3d *)&vehicle_node.position, 1.0f, &out);
    for (i = 0; i < 30; i++)
    {
        real t = (real)(i / 5 * 5) / 30.0f;
        the_head.x = start.x + (out.x - start.x) * t;
        the_head.y = start.y + (out.y - start.y) * t;
        the_head.z = start.z + (out.z - start.z) * t;
        eye = frame(7, out);
    }
    /* out, standing */
    for (i = 0; i < 90; i++)
        eye = frame(NONE, out);
    printf("out %f %f %f\n", eye.x - out.x, eye.y - out.y, eye.z - out.z);
    printf("largest step %f limit %f dark frames %d darkest %f blinks %ld glides %ld\n", largest_step, limit,
        dark_frames, darkest, vr_seat.blinks, vr_seat.glides);
    return 0;
}
"""


def seat_source(text=None):
    text = text or SEAT.read_text()
    source = SEAT_WORLD + "static int vehicle_moving;\n"
    source += between(text, "#define METRES_PER_WORLD_UNIT", "/* ---------- the view */")
    source += SEAT_STUBS
    for name in ("seat_motion", "seat_still", "seat_blinks", "vehicle_to_local", "vehicle_from_local",
                 "shown_position", "ride_spring", "glide_left", "glide_to_frame", "glide_from_frame",
                 "vr_seat_eye", "vr_seat_seated_shown", "vr_seat_darkness"):
        source += function(text, name) + "\n"
    return source + SEAT_MAIN


def run_seat(tmp_path, motion, *extra, text=None, name="seat"):
    exe = build(tmp_path, seat_source(text), name)
    result = subprocess.run([str(exe), motion, *extra], check=True, capture_output=True, text=True).stdout
    values = {}
    for line in result.splitlines():
        words = line.split()
        if words[0] == "seated":
            values["seated"] = [float(w) for w in words[1:4]]
            values["shown"] = int(words[5])
        elif words[0] == "moved":
            values["moved"] = [float(w) for w in words[2:5]]
        elif words[0] == "out":
            values["out"] = [float(w) for w in words[1:4]]
        elif words[0] == "largest":
            values.update(step=float(words[2]), limit=float(words[4]), dark=int(words[7]), darkest=float(words[9]),
                          blinks=int(words[11]), glides=int(words[13]))
    return values


def test_getting_in_and_out_stays_in_the_first_person(tmp_path):
    v = run_seat(tmp_path, "still")
    # never dark, never a blink: the eye glides
    assert v["dark"] == 0 and v["blinks"] == 0
    assert v["glides"] >= 2
    # never faster than the ride's limit, though the animation jumps the head 0.16 world units (half a
    # metre) at a time
    assert v["step"] <= v["limit"] + 1e-6
    # in the seat: where the head was once seated, not bobbing with the seat's animation
    assert v["seated"] == pytest.approx([1.0, 0.3, 0.5], abs=1e-3)
    assert v["shown"] == 1
    # and out: the first person's eye, exactly
    assert v["out"] == pytest.approx([0.0, 0.0, 0.0], abs=1e-4)


def test_the_seated_eye_rides_with_the_vehicle(tmp_path):
    v = run_seat(tmp_path, "still", "moving")
    assert v["moved"] == pytest.approx([1.0, 0.3, 0.5], abs=1e-3)
    assert v["dark"] == 0
    assert v["out"] == pytest.approx([0.0, 0.0, 0.0], abs=1e-4)


def test_blink_setting_still_blinks(tmp_path):
    v = run_seat(tmp_path, "blink")
    assert v["blinks"] >= 2 and v["darkest"] == pytest.approx(1.0)
    assert v["glides"] == 0
    # (held where the head was as the blink ended: the seat's animation bobs it 5 cm about the seat)
    assert v["seated"] == pytest.approx([1.0, 0.3, 0.5], abs=0.051)


def test_negative_control_without_the_settling_glide_it_blinks(tmp_path):
    text = SEAT.read_text()
    mutated = text.replace("vr_seat.glide = _glide_settling;", "vr_seat.glide = _glide_none;")
    assert mutated != text
    v = run_seat(tmp_path, "still", text=mutated, name="seat_mutated")
    assert v["dark"] > 0


def test_negative_control_without_following_the_head_jumps(tmp_path):
    text = SEAT.read_text()
    # (the head's path not followed and smoothed: only the jump into it closed, the climb's steps seen)
    mutated = text.replace("boolean following = (source == _seat_source_getting_in",
                           "boolean following = FALSE && (source == _seat_source_getting_in")
    assert mutated != text
    v = run_seat(tmp_path, "still", text=mutated, name="seat_unfollowed")
    assert v["step"] > v["limit"]


# ---------- back from a death (vr_death.c, vr_seat.c)

RESPAWN_MAIN = r"""
#define DT (1.0f / 90.0f)

int main(int argc, char **argv)
{
    /* the death cam's vantage (3 m from the body) the eye while dead, and the new unit's eye back */
    real_point3d vantage = {4.0f, 1.0f, 1.2f}, head = {10.0f, -3.0f, 0.6f}, eye;
    real farthest = 0.0f, darkest = 0.0f;
    int i, cut = argc > 1 && !strcmp(argv[1], "cut");

    perspective_now = 3;
    for (i = 0; i < 30; i++)
    {
        eye = vantage;
        vr_seat_eye(NONE, TRUE, DT, &eye);
    }
    /* back: the game's camera the new first person's eye, at once */
    perspective_now = _director_perspective_first_person;
    if (cut)
        vr_seat_eye_cut();
    for (i = 0; i < 45; i++)
    {
        eye = head;
        vr_seat_eye(NONE, TRUE, DT, &eye);
        farthest = MAX(farthest, distance3d(&eye, &head));
        darkest = MAX(darkest, vr_seat_darkness());
        if (i == 0)
            printf("first %f\n", distance3d(&eye, &head));
    }
    printf("farthest %f darkest %f\n", farthest, darkest);
    return 0;
}
"""


def run_respawn(tmp_path, *args, text=None, name="respawn"):
    text = text or SEAT.read_text()
    source = "static short perspective_now;\n" + SEAT_WORLD.replace(
        "return _director_perspective_first_person;}", "return perspective_now;}")
    source += "static int vehicle_moving;\n"
    source += between(text, "#define METRES_PER_WORLD_UNIT", "/* ---------- the view */")
    source += SEAT_STUBS
    for name_ in ("seat_motion", "seat_still", "seat_blinks", "vehicle_to_local", "vehicle_from_local",
                  "shown_position", "ride_spring", "glide_left", "glide_to_frame", "glide_from_frame",
                  "vr_seat_eye", "vr_seat_eye_cut", "vr_seat_darkness"):
        source += function(text, name_) + "\n"
    exe = build(tmp_path, source + RESPAWN_MAIN, name)
    out = subprocess.run([str(exe), *args], check=True, capture_output=True, text=True).stdout.split()
    return {"first": float(out[1]), "farthest": float(out[3]), "darkest": float(out[5])}


def test_back_from_a_death_the_eye_is_the_new_ones_at_once(tmp_path):
    v = run_respawn(tmp_path, "cut")
    assert v["first"] == pytest.approx(0.0, abs=1e-5) and v["farthest"] == pytest.approx(0.0, abs=1e-5)
    # (the death's own dark and switching bring it up: no blink of the seat's)
    assert v["darkest"] == 0.0


def test_without_the_cut_the_deaths_eye_is_held_in_view(tmp_path):
    # (the negative control: as it was, the blink holds the death cam's vantage as it darkens)
    v = run_respawn(tmp_path, name="respawn_held")
    assert v["first"] > 1.0 and v["darkest"] > 0.0


def test_negative_control_the_cut_that_keeps_the_eye_fails(tmp_path):
    text = SEAT.read_text()
    body = function(text, "vr_seat_eye_cut")
    mutated = text.replace(body, body.replace("vr_seat.shown_valid = FALSE;", ""))
    assert mutated != text
    v = run_respawn(tmp_path, "cut", text=mutated, name="respawn_mutated")
    assert v["first"] > 1.0


def test_back_from_a_death_cuts_the_seats_eye():
    death_reset = function((ROOT / "port/linux/game/vr_death.c").read_text(), "death_reset")
    # (both ways back: the death cam's, "crt", and the held eye's, "face" and "still")
    assert death_reset.count("vr_seat_eye_cut();") == 2


# ---------- steering with the sticks (vr.c)

STEER_WORLD = r"""
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
enum {HALO_VR_SEAT_RIDE, HALO_VR_SEAT_BARS, HALO_VR_SEAT_WHEEL, HALO_VR_SEAT_TURRET};
"""

STEER_MAIN = r"""
/* the game's turn of the Ghost toward the facing, each tick (vr.c's HANDLEBAR_* comment:
update_alien_scout_physics): the lesser of sqrt(2 a e) and e / 2 for an error of e */
static float ghost_rate(float error)
{
    float a = HANDLEBAR_GHOST_TURN_ACCELERATION, e = fabsf(error), tick = sqrtf(2.0f * a * e);
    if (tick > e / 2.0f)
        tick = e / 2.0f;
    return tick * HANDLEBAR_TICKS_PER_SECOND;
}

int main(void)
{
    float lock_left = 0.52f, lock_right = 0.5f;
    printf("dead %g %g\n", stick_facing(HALO_VR_SEAT_BARS, 0.15f, -0.19f, lock_left, lock_right, 1.0f),
        stick_facing(HALO_VR_SEAT_WHEEL, 0.0f, 0.0f, lock_left, lock_right, 1.0f));
    printf("barsright %g rate %g\n", stick_facing(HALO_VR_SEAT_BARS, 0.0f, 1.0f, lock_left, lock_right, 1.0f),
        ghost_rate(stick_facing(HALO_VR_SEAT_BARS, 0.0f, 1.0f, lock_left, lock_right, 1.0f)));
    printf("barsleft %g rate %g\n", stick_facing(HALO_VR_SEAT_BARS, -0.6f, 0.0f, lock_left, lock_right, 1.0f),
        ghost_rate(stick_facing(HALO_VR_SEAT_BARS, -0.6f, 0.0f, lock_left, lock_right, 1.0f)));
    printf("wheel left %g right %g reversing %g\n",
        stick_facing(HALO_VR_SEAT_WHEEL, -1.0f, 0.0f, lock_left, lock_right, 1.0f),
        stick_facing(HALO_VR_SEAT_WHEEL, 0.0f, 1.0f, lock_left, lock_right, 1.0f),
        stick_facing(HALO_VR_SEAT_WHEEL, -1.0f, 0.0f, lock_left, lock_right, -1.0f));
    printf("stronger %g ride %g\n", stick_facing(HALO_VR_SEAT_WHEEL, -0.5f, 1.0f, lock_left, lock_right, 1.0f),
        stick_facing(HALO_VR_SEAT_RIDE, 1.0f, 1.0f, lock_left, lock_right, 1.0f));
    printf("barsfacing %g %g\n", bars_facing(0.0f), bars_facing(0.3f));
    return 0;
}
"""


def steer_source():
    text = VR.read_text()
    source = STEER_WORLD
    source += defines(text, "HANDLEBAR_DEADZONE", "HANDLEBAR_TURN_RATE", "HANDLEBAR_TURN_FASTEST",
                      "HANDLEBAR_GHOST_TURN_ACCELERATION", "HANDLEBAR_TICKS_PER_SECOND", "STICK_STEER_DEADZONE")
    for name in ("stick_steer_push", "turn_facing", "bars_facing", "stick_facing"):
        source += function(text, name) + "\n"
    return source + STEER_MAIN


def test_sticks_steer_a_seat_without_the_hands_on_its_controls(tmp_path):
    exe = build(tmp_path, steer_source(), "steer")
    lines = {line.split()[0]: line.split() for line in
             subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.splitlines()}
    assert float(lines["dead"][1]) == 0.0 and float(lines["dead"][2]) == 0.0
    # a full push right turns the Ghost right (the facing to its right: negative) at 90 degrees a second
    right = lines["barsright"]
    assert float(right[1]) < 0.0
    assert float(right[3]) == pytest.approx(1.5708, rel=0.05)
    # half a push left (0.6, past a 0.2 deadzone), left at half that
    left = lines["barsleft"]
    assert float(left[1]) > 0.0
    assert float(left[3]) == pytest.approx(0.7854, rel=0.05)
    # the Warthog's wheels to their lock, the other way as it reverses
    wheel = lines["wheel"]
    assert float(wheel[2]) == pytest.approx(0.52)
    assert float(wheel[4]) == pytest.approx(-0.5)
    assert float(wheel[6]) == pytest.approx(-0.52)
    # the stronger push of the two sticks; a passenger's seat is not steered
    assert float(lines["stronger"][1]) == pytest.approx(-0.5)
    assert float(lines["stronger"][3]) == 0.0
    # the handlebars' own steering is as it was: straight bars, straight; turned, a turn
    assert float(lines["barsfacing"][1]) == 0.0
    assert float(lines["barsfacing"][2]) > 0.0


def test_handlebars_facing_unchanged():
    text = VR.read_text()
    assert "float error = turn_facing(fabsf(turn) * HANDLEBAR_TURN_RATE);" in function(text, "bars_facing")


def test_left_stick_does_not_strafe_while_it_steers():
    gamepad = function(VR.read_text(), "halo_vr_gamepad")
    assert "if (!seat_stick_steers() && !seat_drives())\n\t\tstick_merge(&pad->sThumbLX" in gamepad
    # (the throttle, forward and back, always reaches the game)
    assert "\tstick_merge(&pad->sThumbLY, vr.input.left_stick[1]);" in gamepad
