/*
VR_DEATH.C

The VR mode's view of the player's death (HALO_VR; vr.death_view,
port/linux/README.md "VR"), for vr_render.c, and its automated test.

When the player dies, the game's director turns to its dead camera
(dead_camera.c): from inside the body it pulls out over three seconds to
between 2 and 6 world units from it (6 to 18 m, nearer where walls are), in
a direction picked at random, 27 to 63 degrees above it, looking at it and
following it as it falls, until the game reverts to the last checkpoint
(main.c: 92 ticks after the players are all dead) or, in a multiplayer
game, the player respawns (three seconds at least); in network co-op it
follows a living teammate from behind (coop_spectate.c). The VR mode took
that camera as the eye and its heading as the view's (the player no longer
aims): on b30 the view jumped 1.9 m as the player died, flew 10 m out and
7 m up, at up to half a metre a frame, turning 74 degrees, and cut back to
the player's new eye in full view. In a headset that is the world carried
off and turned about the head.

With vr.death_view = "face" (and "still") the eye stays where it was as the
player died, and the view's heading where the player turned it: the head
looks about from there as ever, and nothing else moves the eye or turns the
view, but a blink: it darkens, turns or moves in the dark, and lightens.
The body falls before and under the eye, drawn as everyone else sees it (it
is no longer the player's: render_objects.c draws it whatever the camera),
its head shrunk into its neck while it is at the eye, and the view dimmed
while the eye is within the body, its armour's insides lit by its shields'
last flash: the death begins with a dark beat, the body falling away out of
it. The world dims and reddens, and goes dark just before the game reverts
or respawns the player; their new eye comes up out of the dark. The visor
goes as the HUD does, with a last red flare (vr_render.c, vr_visor.c). With
"face", a body that lands out of sight (a blast throws it back) turns the
view to it in a blink, once, the eye where it is. Watching someone else (a
teammate network co-op's spectating follows, or another player in a
multiplayer game after 15 seconds), the eye is put where the game's camera
is once it has settled on them, looking where it looks, in a blink, held
there, and put again in a blink when the game's camera has moved some
metres from it. The game itself (its camera, the sounds' listener, the
respawn and the revert, what the netcode sends) is not changed.

debug.vr_test_death kills the first player some seconds into the level
(as the script's unit_kill does, or by a frag grenade's blast before them,
or killed and then watching the nearest living biped, as co-op's
spectating watches a teammate), and logs, each frame from a second before until some seconds after they are
back, how far the view moved and turned, how dark it was, and where the
body was from the eye, and each phase's sums (alive, dead, back): with
debug.vr_test_head = "0", the motion the player did not make.

Called from vr_render.c each VR frame, from the main loop (main.c) and in
the eye pass (render.c).
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "game/game_globals.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "models/model_definitions.h"
#include "objects/damage.h"
#include "objects/damage_effect_definitions.h"
#include "units/units.h"
#include "scenario/scenario.h"
#include "tag_files/tag_groups.h"
#include "tag_files/tag_files.h"
#include "cutscene/cinematics.h"
#include "camera/dead_camera.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* the platform layer's (port/linux/src/port_config.c) */
const char *config_string(char const *name);
unsigned long config_changes(void);
void platform_log(char const *format, ...);
/* vr.c's, d3d8_gl.c's */
int halo_vr_aiming(void);
void halo_vr_test_screenshot(void);
void halo_vr_eye_tint(float red, float green, float blue);
/* vr_seat.c's, vr_render.c's, render_interpolation.c's */
boolean vr_seat_shown(real_point3d *position);
real vr_seat_darkness(void);
boolean vr_render_eye_pass(void);
real render_interpolation_fraction(void);
/* director.c's, main.c's */
struct dead_camera *director_vr_dead_camera(void);
long main_vr_revert_ticks_left(void);

#define METRES_PER_WORLD_UNIT 3.048f
#define DEGREES_PER_RADIAN (180.0f / 3.14159265f)
/* the world as the player dies: dimmed and reddened to this colour (a
multiple of the world's own), over this time (seconds) */
#define DEAD_TINT_RED 0.68f
#define DEAD_TINT_GREEN 0.34f
#define DEAD_TINT_BLUE 0.30f
#define DEAD_TINT_SECONDS 1.2f
/* and dark over this time before the revert or the respawn (ticks; all
dark the tick before), and lit again after this long in the dark (ticks)
should neither come */
#define DEAD_DARK_TICKS 15
#define DEAD_DARK_HOLD_TICKS 45
/* and both lifted over this time once the player is back (seconds), the
dark held this long first (seconds: the new eye's first frames) */
#define DEAD_LIFT_SECONDS 0.3f
#define DEAD_BACK_DARK_SECONDS 0.1f
#define DEAD_LIFT_STEP_SECONDS (1.0f / 30.0f)
/* a blink: the view darkens over this time (seconds), the eye moves or
the view turns, and it lightens over this (vr_seat.c's) */
#define BLINK_DARKENING_SECONDS 0.1f
#define BLINK_LIGHTENING_SECONDS 0.15f
/* vr.death_view = "face": once the body has landed (slower than this,
metres a second, from this long after the death, or at the latest this
long after: seconds), the view turns to it in a blink if it is farther
than this across the ground (metres) and more than this from where the
head looks (degrees) */
#define FACE_SECONDS 0.8f
#define FACE_LATEST_SECONDS 1.6f
#define FACE_SPEED 0.5f
#define FACE_DISTANCE 1.0f
#define FACE_ANGLE 50.0f
/* watching someone else: the eye is put where the game's camera is once
it has settled on them (it moves slower than this, metres a second, from
this long after it turned to them, or at the latest this long after:
seconds; the dead camera pulls away from them over three seconds), and
again once it is this far from the eye (world units: 4 m), at most so
often (seconds) */
#define WATCH_SETTLE_SECONDS 0.5f
#define WATCH_LATEST_SECONDS 2.5f
#define WATCH_SETTLED_SPEED 1.0f
#define WATCH_DISTANCE (4.0f / METRES_PER_WORLD_UNIT)
#define WATCH_SECONDS 2.0f
/* the dead body's head is shrunk into its neck while it is nearer the eye
than this (metres), and grows back until it is this far */
#define BODY_HEAD_HIDDEN 0.30f
#define BODY_HEAD_GROWN 0.60f
#define MAXIMUM_BODY_NODES 64
/* and the view is dimmed, by as much as this, while any of the body's
nodes is nearer the eye than this (metres), less until this, coming in and
going over these time constants (seconds): the eye within the falling body
sees the insides of its armour, lit by its shields' last flash */
#define BODY_NEAR_DIM 0.85f
#define BODY_NEAR 0.25f
#define BODY_CLEAR 0.55f
#define BODY_NEAR_IN 0.04f
#define BODY_NEAR_OUT 0.10f

enum
{
	_death_view_face,
	_death_view_still,
	_death_view_game
};

enum
{
	_death_phase_alive,
	_death_phase_dead,
	_death_phase_back,
	NUMBER_OF_DEATH_PHASES
};

static char const *const death_phase_names[NUMBER_OF_DEATH_PHASES] =
{
	"alive",
	"dead",
	"back",
};

static struct
{
	/* dead: the eye held (before the head's offset), the turn added to
	the view's heading (to the body, or watching someone, to look where the
	game's camera does), whether the view has been turned to the body (or
	needs not be), the unit watched and whether the eye has been put at the
	game's camera for it, how long dead (seconds of the game's time) and
	since the eye was put (or the unit watched changed: seconds) */
	boolean dead;
	real_point3d eye;
	real turn;
	boolean faced;
	long watched_unit_index;
	boolean watching;
	boolean placed;
	boolean camera_valid;
	real_point3d camera;
	real seconds;
	real put_seconds;
	/* a blink: under way (darkening), its darkness (0 to 1), and the eye
	and the turn it is to put in the dark */
	boolean blinking;
	real blink;
	real_point3d blink_eye;
	real blink_turn;
	/* the tint (0 to 1), the dimming while the body is at the eye (0 to
	1), the darkness before the revert (0 to 1), and the ticks the darkness
	has lasted */
	real tint;
	real near;
	real back_dark_seconds;
	real dark;
	long dark_ticks;
	long dark_tick;
	/* the head's place this frame (the centre camera's) */
	real_point3d head;

	boolean checked;
	/* debug.vr_test_death: when to die (seconds), and how ("kill",
	"grenade", "watch": killed, then watching another) */
	boolean testing;
	real death_seconds;
	char how[16];
	boolean killed;
	boolean watched;
	long start_tick;
	/* the log: the frame, whether it logs, the time back, the last frame's
	camera, its phase, and the phase's sums */
	long frame;
	boolean logging;
	long back_frames;
	boolean last_valid;
	real_point3d last_position;
	real_vector3d last_forward;
	real_vector3d last_up;
	real last_darkness;
	short phase;
	long phase_frames;
	real phase_distance;
	real phase_turn;
	real phase_largest_move;
	real phase_largest_turn;
	real phase_seen_distance;
	real phase_seen_turn;
	long phase_body_seen;
} vr_death;

/* ---------- the view */

/* whether a node's name has part in it, whatever its case (the game's
characters name theirs "bip01 head", "Bip01 Head") */
static boolean name_has(char const *name, char const *part)
{
	char lower[TAG_STRING_LENGTH + 1];
	short index;

	for (index = 0; index < TAG_STRING_LENGTH && name[index]; index++)
		lower[index] = (char)(name[index] >= 'A' && name[index] <= 'Z' ? name[index] - 'A' + 'a' : name[index]);
	lower[index] = 0;
	return strstr(lower, part) != NULL;
}

/* vr.death_view: the eye stays where the player died, turned to the body
in a blink if it lands out of sight ("face") or never turned ("still"),
else the game's camera ("game") */
static short death_view(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static short view;

	if (read_at != config_changes())
	{
		char const *setting = config_string("vr.death_view");

		read_at = config_changes();
		view = strcmp(setting, "game") == 0 ? _death_view_game :
			strcmp(setting, "still") == 0 ? _death_view_still : _death_view_face;
	}
	return view;
}

/* the first player's dead body (its unit, while they are dead), else NONE */
static long dead_body(void)
{
	long player_index = local_player_get_player_index(0);
	struct player_datum *player;

	if (player_index == NONE)
		return NONE;
	player = player_get(player_index);
	if (player->unit_index != NONE || player->dead_unit_index == NONE || !object_try_and_get(player->dead_unit_index))
		return NONE;
	return player->dead_unit_index;
}

/* the ticks left before the game brings the dead first player back: the
revert (all players dead, main.c), or a multiplayer game's respawn; else
NONE (unknown: network co-op's respawn waits until it is safe) */
static long ticks_left(void)
{
	long player_index = local_player_get_player_index(0);
	long revert = main_vr_revert_ticks_left();

	if (revert != NONE)
		return revert;
	if (game_engine_running() && player_index != NONE && player_get(player_index)->respawn_timer > 0)
		return player_get(player_index)->respawn_timer;
	return NONE;
}

/* not dead (or the view not the death's): just back, the view all dark,
held so a moment, and the dark and the tint lifted over a moment (the new
eye comes up out of the dark) */
static void death_reset(real seconds)
{
	/* (the frame the player is back in can be long, a checkpoint loaded:
	the lift is seen, however long the frames) */
	seconds = MIN(seconds, DEAD_LIFT_STEP_SECONDS);
	if (vr_death.dead)
	{
		vr_death.dark = 1.0f;
		vr_death.back_dark_seconds = DEAD_BACK_DARK_SECONDS;
	}
	if (vr_death.back_dark_seconds > 0.0f)
	{
		vr_death.back_dark_seconds -= seconds;
		seconds = 0.0f;
	}
	vr_death.dead = FALSE;
	vr_death.turn = 0.0f;
	vr_death.watching = FALSE;
	vr_death.watched_unit_index = NONE;
	vr_death.camera_valid = FALSE;
	vr_death.blinking = FALSE;
	vr_death.blink = MAX(0.0f, vr_death.blink - seconds / BLINK_LIGHTENING_SECONDS);
	vr_death.tint = MAX(0.0f, vr_death.tint - seconds / DEAD_LIFT_SECONDS);
	vr_death.near = 0.0f;
	vr_death.dark = MAX(0.0f, vr_death.dark - seconds / DEAD_LIFT_SECONDS);
	vr_death.dark_ticks = 0;
}

/* a blink, unless one is under way: the view darkens, the eye is put
there and the view turned by as much in the dark, and it lightens */
static void death_blink(real_point3d const *eye, real turn, char const *why)
{
	/* (not as the view darkens before the player is back) */
	if (vr_death.blinking || vr_death.dark > 0.0f)
		return;
	vr_death.blinking = TRUE;
	vr_death.blink_eye = *eye;
	vr_death.blink_turn = turn;
	if (vr_death.testing)
	{
		platform_log("vr death test: frame %ld: blink (%s): the eye %.3f m, the view turned %.1f degrees", vr_death.frame,
			why, distance3d(&vr_death.eye, eye) * METRES_PER_WORLD_UNIT,
			remainderf(turn - vr_death.turn, 2.0f * _pi) * DEGREES_PER_RADIAN);
	}
}

/* each VR frame (vr_render.c), before the seat's blinks: while the first
player is dead and the game's camera is the dead's, the eye's place before
the head's offset and the view's heading, TRUE. In: the game's camera (its
position and forward); out: the eye and the heading (radians). playing:
the view could be the player's (not a cinematic's); head_yaw: the head's
turn from the view's heading, body_yaw: the view's heading as the player
turned it (radians) */
boolean vr_death_eye(boolean playing, real seconds, real head_yaw, real body_yaw, real_vector3d const *camera_forward,
	real_point3d *position, real *yaw)
{
	struct dead_camera *camera;
	long player_index = local_player_get_player_index(0);
	short view = death_view();

	if (!playing || view == _death_view_game || !(camera = director_vr_dead_camera()))
	{
		death_reset(seconds);
		return FALSE;
	}
	if (!vr_death.dead)
	{
		/* (just dead: the eye where it was last frame) */
		vr_death.dead = TRUE;
		vr_death.seconds = 0.0f;
		vr_death.turn = 0.0f;
		vr_death.faced = view != _death_view_face;
		if (!vr_seat_shown(&vr_death.eye))
			vr_death.eye = *position;
		if (vr_death.testing)
		{
			platform_log("vr death test: frame %ld: dead, the eye held at %.3f %.3f %.3f m; the game's camera %.3f m from it",
				vr_death.frame, vr_death.eye.x * METRES_PER_WORLD_UNIT, vr_death.eye.y * METRES_PER_WORLD_UNIT,
				vr_death.eye.z * METRES_PER_WORLD_UNIT, distance3d(&vr_death.eye, position) * METRES_PER_WORLD_UNIT);
		}
	}
	if (!game_time_get_paused())
	{
		vr_death.seconds += seconds;
		vr_death.put_seconds += seconds;
	}

	/* watching someone else (not the player's own body): once the game's
	camera has settled on them (it pulls away from them over three
	seconds), the eye is put where it is, looking where it looks, in a
	blink, and held; and put again once the game's camera has moved some
	metres away */
	vr_death.watching = camera->unit_index != NONE &&
		(player_index == NONE || camera->unit_index != player_get(player_index)->dead_unit_index);
	if (vr_death.watching)
	{
		real speed = vr_death.camera_valid && seconds > 0.0f ?
			distance3d(&vr_death.camera, position) * METRES_PER_WORLD_UNIT / seconds : 0.0f;

		if (camera->unit_index != vr_death.watched_unit_index)
		{
			vr_death.watched_unit_index = camera->unit_index;
			vr_death.placed = FALSE;
			vr_death.put_seconds = 0.0f;
		}
		if ((!vr_death.placed && vr_death.put_seconds >= WATCH_SETTLE_SECONDS &&
				(speed < WATCH_SETTLED_SPEED || vr_death.put_seconds >= WATCH_LATEST_SECONDS)) ||
			(vr_death.placed && vr_death.put_seconds >= WATCH_SECONDS &&
				distance3d(&vr_death.eye, position) > WATCH_DISTANCE))
		{
			death_blink(position, (real)atan2(camera_forward->j, camera_forward->i) - head_yaw - body_yaw,
				"watching another, the eye put at the game's camera");
			vr_death.placed = TRUE;
			vr_death.put_seconds = 0.0f;
		}
	}
	else if (!vr_death.faced)
	{
		/* the body landed out of sight (thrown back, as by a blast before
		the player): the view turned to it in a blink, once, the eye where
		it is (not at the player's feet: looking down sees it there) */
		long body_index = dead_body();
		struct object_datum *body = body_index == NONE ? NULL : object_get(body_index);

		if (!body)
		{
			vr_death.faced = TRUE;
		}
		else if (vr_death.seconds >= FACE_SECONDS &&
			(magnitude3d(&body->object.translational_velocity) * TICKS_PER_SECOND * METRES_PER_WORLD_UNIT < FACE_SPEED ||
				vr_death.seconds >= FACE_LATEST_SECONDS))
		{
			real_vector3d to;
			real off;

			vr_death.faced = TRUE;
			vector_from_points3d(&vr_death.head, &body->object.bounding_sphere_center, &to);
			off = remainderf((real)atan2(to.j, to.i) - (body_yaw + vr_death.turn + head_yaw), 2.0f * _pi);
			if ((real)sqrt(to.i * to.i + to.j * to.j) * METRES_PER_WORLD_UNIT > FACE_DISTANCE &&
				fabsf(off) > FACE_ANGLE * _pi / 180.0f)
			{
				death_blink(&vr_death.eye, vr_death.turn + off, "the body out of sight");
			}
		}
	}

	/* the blink: the view darkens with the eye and the turn held, both
	change in the dark, and it lightens */
	if (vr_death.blinking)
	{
		vr_death.blink += seconds / BLINK_DARKENING_SECONDS;
		if (vr_death.blink >= 1.0f)
		{
			vr_death.blink = 1.0f;
			vr_death.blinking = FALSE;
			vr_death.eye = vr_death.blink_eye;
			vr_death.turn = vr_death.blink_turn;
		}
	}
	else
	{
		vr_death.blink = MAX(0.0f, vr_death.blink - seconds / BLINK_LIGHTENING_SECONDS);
	}
	vr_death.camera_valid = TRUE;
	vr_death.camera = *position;
	*position = vr_death.eye;
	*yaw = body_yaw + vr_death.turn;

	/* the body at the eye: the view dimmed until it has fallen away */
	{
		long body_index = vr_death.watching ? NONE : dead_body();
		real target = 0.0f;

		if (body_index != NONE)
		{
			struct object_datum *body = object_get(body_index);
			real_matrix4x3 const *nodes = object_get_node_matrices(body_index);
			long count = body->object.node_matrices.size / (long)sizeof(real_matrix4x3);
			real nearest = BODY_CLEAR;
			long index;

			for (index = 0; nodes && index < count; index++)
				nearest = MIN(nearest, distance3d(&nodes[index].position, &vr_death.head) * METRES_PER_WORLD_UNIT);
			target = 1.0f - MAX(0.0f, nearest - BODY_NEAR) / (BODY_CLEAR - BODY_NEAR);
			target = target * target * (3.0f - 2.0f * target);
		}
		vr_death.near += (target - vr_death.near) *
			(1.0f - (real)exp(-seconds / (target > vr_death.near ? BODY_NEAR_IN : BODY_NEAR_OUT)));
	}

	/* the tint as the player dies, and the dark before they are back */
	{
		real t = MIN(1.0f, vr_death.seconds / DEAD_TINT_SECONDS);
		long left = ticks_left();
		long tick = game_time_get();

		vr_death.tint = t * t * (3.0f - 2.0f * t);
		if (left != NONE && left <= DEAD_DARK_TICKS)
		{
			real fraction = game_time_get_paused() ? 0.0f : render_interpolation_fraction();

			vr_death.dark = MIN(1.0f, MAX(vr_death.dark,
				1.0f - ((real)left - 1.0f - fraction) / (real)(DEAD_DARK_TICKS - 1)));
		}
		if (vr_death.dark >= 1.0f && tick != vr_death.dark_tick)
		{
			vr_death.dark_ticks += tick - vr_death.dark_tick > 0 ? tick - vr_death.dark_tick : 0;
			if (vr_death.dark_ticks > DEAD_DARK_HOLD_TICKS)
			{
				/* (neither came: lit again, with the tint) */
				vr_death.dark = 0.0f;
				vr_death.dark_ticks = 0;
			}
		}
		vr_death.dark_tick = tick;
	}
	return TRUE;
}

/* while the view is the death's (vr_death_eye), the turn added to the
view's heading as the player turned it, TRUE (vr_render.c's HUD camera: as
the eyes') */
boolean vr_death_turn(real *turn)
{
	*turn = vr_death.turn;
	return vr_death.dead;
}

/* whether the view is the death's (vr_render.c: the visor's last flare) */
boolean vr_death_viewing(void)
{
	return vr_death.dead;
}

/* render_objects.c, each object drawn in the eye pass: the nodes to draw
it with. The dead first player's body, while the view is the death's, is
seen from where its head was, which is where its head still is as it
begins to fall: from within the helmet, and the chest's armour, as
shells, all about the eyes. So its head is shrunk into its neck while it is
near the eye, and grows back as the body falls away. */
real_matrix4x3 const *vr_death_body_nodes(long object_index, real_matrix4x3 const *nodes)
{
	static real_matrix4x3 matrices[MAXIMUM_BODY_NODES];
	struct object_datum *object;
	struct object_definition *definition;
	struct model const *model;
	long count;
	short head = NONE;
	short index;
	real distance, grown;

	if (!vr_death.dead || !nodes || object_index != dead_body() || !vr_render_eye_pass())
		return nodes;
	object = object_get(object_index);
	definition = object_definition_get(object->definition_index);
	if (definition->object.model.index == NONE)
		return nodes;
	model = model_definition_get(definition->object.model.index);
	count = object->object.node_matrices.size / (long)sizeof(real_matrix4x3);
	if (count <= 0 || count > MAXIMUM_BODY_NODES || model->nodes.count < count)
		return nodes;
	for (index = 0; index < count && head == NONE; index++)
	{
		char const *name = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node)->name;

		if (name_has(name, "head"))
			head = index;
	}
	if (head == NONE)
		return nodes;
	distance = distance3d(&nodes[head].position, &vr_death.head) * METRES_PER_WORLD_UNIT;
	grown = (distance - BODY_HEAD_HIDDEN) / (BODY_HEAD_GROWN - BODY_HEAD_HIDDEN);
	if (grown >= 1.0f)
		return nodes;
	grown = grown <= 0.0f ? 0.001f : MAX(0.001f, grown * grown * (3.0f - 2.0f * grown));
	for (index = 0; index < count; index++)
	{
		short up;

		matrices[index] = nodes[index];
		for (up = index; up != NONE && up != head;
			up = TAG_BLOCK_GET_ELEMENT(&model->nodes, up, struct model_node)->parent_node_index);
		if (up == head)
		{
			real_matrix4x3 *matrix = &matrices[index];

			matrix->position.x = nodes[head].position.x + (matrix->position.x - nodes[head].position.x) * grown;
			matrix->position.y = nodes[head].position.y + (matrix->position.y - nodes[head].position.y) * grown;
			matrix->position.z = nodes[head].position.z + (matrix->position.z - nodes[head].position.z) * grown;
			matrix->scale *= grown;
		}
	}
	return matrices;
}

/* the eye pass, after the game's screen flash and the seat's blink
(render.c): the world dimmed and reddened while dead, dimmed further while
the body is at the eye, dark in a blink and before the player is back, all
of each eye (d3d8_gl.c's tint, as they are presented) */
void vr_death_draw(void)
{
	real tint = vr_death.tint;
	real light = 1.0f - vr_death.dark;
	real blink = vr_death.blink;

	if (tint <= 0.0f && light >= 1.0f && vr_death.near <= 0.0f && blink <= 0.0f)
		return;
	light = light * light * (3.0f - 2.0f * light) * (1.0f - BODY_NEAR_DIM * vr_death.near) *
		(1.0f - blink * blink * (3.0f - 2.0f * blink));
	halo_vr_eye_tint((1.0f + (DEAD_TINT_RED - 1.0f) * tint) * light, (1.0f + (DEAD_TINT_GREEN - 1.0f) * tint) * light,
		(1.0f + (DEAD_TINT_BLUE - 1.0f) * tint) * light);
}

/* ---------- the test */

static void vr_death_read_settings(void)
{
	char const *setting = config_string("debug.vr_test_death");

	vr_death.checked = TRUE;
	vr_death.start_tick = NONE;
	strcpy(vr_death.how, "kill");
	if (sscanf(setting, "%f %15s", &vr_death.death_seconds, vr_death.how) >= 1)
	{
		vr_death.testing = TRUE;
		platform_log("vr death test: the first player dies %.1f s in (%s)", vr_death.death_seconds, vr_death.how);
	}
}

/* the first player's unit, else NONE */
static long first_player_unit(void)
{
	long player_index = local_player_get_player_index(0);

	return player_index == NONE ? NONE : player_get(player_index)->unit_index;
}

/* a frag grenade's blast half a metre before the player, at the waist,
their shields gone and their health all but gone first: a death that
throws the body back */
static void vr_death_test_blast(long unit_index)
{
	long effect_index = tag_loaded(DAMAGE_EFFECT_DEFINITION_TAG, "weapons\\frag grenade\\explosion");
	struct object_datum *unit = object_get(unit_index);
	struct damage_data damage;
	real_vector3d forward;

	if (effect_index == NONE)
	{
		platform_log("vr death test: no frag grenade's explosion in this map: killed instead");
		unit_kill(unit_index);
		return;
	}
	object_deplete_shield(unit_index);
	unit->object.body_vitality = MIN(unit->object.body_vitality, 0.02f);
	forward.i = unit->object.forward.i;
	forward.j = unit->object.forward.j;
	forward.k = 0.0f;
	normalize3d(&forward);
	damage_data_new(&damage, effect_index);
	damage.origin.x = unit->object.bounding_sphere_center.x + forward.i * (0.5f / METRES_PER_WORLD_UNIT);
	damage.origin.y = unit->object.bounding_sphere_center.y + forward.j * (0.5f / METRES_PER_WORLD_UNIT);
	damage.origin.z = unit->object.bounding_sphere_center.z - (0.3f / METRES_PER_WORLD_UNIT);
	damage.epicenter = damage.origin;
	scenario_location_from_point(&damage.location, &damage.origin);
	area_of_effect_cause_damage(&damage, NONE);
}

/* "watch": the dead camera turned to the nearest living biped but the
player's, as network co-op's spectating turns it to a teammate */
static void vr_death_test_watch(void)
{
	struct dead_camera *camera = director_vr_dead_camera();
	long body_index = dead_body();
	struct object_iterator bipeds;
	long nearest_index = NONE;
	real nearest_distance = 0.0f;

	if (!camera || body_index == NONE)
		return;
	object_iterator_new(&bipeds, _object_mask_biped, 0);
	while (object_iterator_next(&bipeds))
	{
		struct object_datum *biped = object_get(bipeds.index);
		real distance = distance_squared3d(&biped->object.position, &object_get(body_index)->object.position);

		if (bipeds.index != body_index && !TEST_FLAG(biped->object.damage_flags, _object_dead_bit) &&
			(nearest_index == NONE || distance < nearest_distance))
		{
			nearest_index = bipeds.index;
			nearest_distance = distance;
		}
	}
	if (nearest_index == NONE)
	{
		platform_log("vr death test: no living biped to watch");
		return;
	}
	/* (pulled away from them over half a second, not three, so that it
	settles before the level reverts) */
	camera->unit_index = nearest_index;
	camera->timer = 0.5f;
	platform_log("vr death test: frame %ld: the dead camera watches %s, %.1f m away", vr_death.frame,
		tag_get_name(object_get(nearest_index)->definition_index), (real)sqrt(nearest_distance) * METRES_PER_WORLD_UNIT);
}

/* each frame: the first player dies on time (counted from when they first
aim: a level's clock does not start at 0, and its skipped cinematics play
fast), once a run */
void vr_death_test_update(void)
{
	long unit_index;
	long time;

	if (!vr_death.checked)
		vr_death_read_settings();
	if (!vr_death.testing || !game_in_progress())
		return;
	if (vr_death.killed)
	{
		if (!vr_death.watched && vr_death.dead && vr_death.seconds >= 0.1f && strcmp(vr_death.how, "watch") == 0)
		{
			vr_death.watched = TRUE;
			vr_death_test_watch();
		}
		return;
	}
	unit_index = first_player_unit();
	time = game_time_get();
	if (vr_death.start_tick == NONE || time < vr_death.start_tick)
	{
		if (unit_index != NONE && halo_vr_aiming() && !cinematic_in_progress())
		{
			vr_death.start_tick = time;
			platform_log("vr death test: the first player aims, %.1f s into the level", (real)time / TICKS_PER_SECOND);
		}
		return;
	}
	time -= vr_death.start_tick;
	if (time >= (long)((vr_death.death_seconds - 1.0f) * TICKS_PER_SECOND))
		vr_death.logging = TRUE;
	if (time >= (long)(vr_death.death_seconds * TICKS_PER_SECOND) && unit_index != NONE)
	{
		vr_death.killed = TRUE;
		platform_log("vr death test: frame %ld: the first player is killed (%s)", vr_death.frame, vr_death.how);
		if (strcmp(vr_death.how, "grenade") == 0)
			vr_death_test_blast(unit_index);
		else
			unit_kill(unit_index);
	}
}

/* the first player's phase: alive (before the death), dead, or back */
static short vr_death_phase(void)
{
	if (!vr_death.killed)
		return _death_phase_alive;
	if (first_player_unit() == NONE)
		return _death_phase_dead;
	return vr_death.phase == _death_phase_alive ? _death_phase_alive : _death_phase_back;
}

static real angle_between(real_vector3d const *a, real_vector3d const *b)
{
	real cosine = dot_product3d(a, b);

	return (real)acos(cosine > 1.0f ? 1.0f : cosine < -1.0f ? -1.0f : cosine) * DEGREES_PER_RADIAN;
}

static void vr_death_log_phase_end(void)
{
	if (vr_death.phase_frames == 0)
		return;
	platform_log("vr death log: %s: %ld frames, moved %.3f m (at most %.1f mm a frame), %.3f m of it seen, turned "
		"%.1f degrees (at most %.2f a frame), %.1f of it seen; the body in view %ld frames",
		death_phase_names[vr_death.phase], vr_death.phase_frames, vr_death.phase_distance,
		vr_death.phase_largest_move * 1000.0f, vr_death.phase_seen_distance, vr_death.phase_turn,
		vr_death.phase_largest_turn, vr_death.phase_seen_turn, vr_death.phase_body_seen);
}

/* each VR frame's centre camera (vr_render.c): the head's place this
frame (vr_death_body_nodes'), and its motion logged while the test logs;
half_across, half_up: the view's half angles (radians), to say whether the
body is in it */
void vr_death_camera(real_point3d const *position, real_vector3d const *forward, real_vector3d const *up,
	real half_across, real half_up)
{
	short phase;

	vr_death.head = *position;
	vr_death.frame++;
	if (!vr_death.logging)
		return;
	phase = vr_death_phase();
	if (phase == _death_phase_back && vr_death.phase != _death_phase_back)
		vr_death.back_frames = 0;
	if (phase == _death_phase_alive && vr_death.killed && ++vr_death.back_frames > 5 * 72)
	{
		vr_death.logging = FALSE;
		platform_log("vr death test: the first player did not die");
		return;
	}
	if (phase == _death_phase_back && ++vr_death.back_frames > 4 * 72)
	{
		vr_death_log_phase_end();
		vr_death.logging = FALSE;
		platform_log("vr death test: the log ends");
		return;
	}
	if (phase != _death_phase_alive && (phase == _death_phase_dead || vr_death.back_frames <= 108) &&
		vr_death.frame % (phase == _death_phase_dead && vr_death.phase_frames < 36 ? 3 : 12) == 0)
	{
		/* (with debug.screenshot_directory: six a second from the death to
		a second and a half after they are back, 24 in its first half
		second) */
		halo_vr_test_screenshot();
	}
	if (vr_death.last_valid)
	{
		real move = distance3d(position, &vr_death.last_position) * METRES_PER_WORLD_UNIT;
		real turn = MAX(angle_between(forward, &vr_death.last_forward), angle_between(up, &vr_death.last_up));
		real darkness = MAX(MAX(vr_seat_darkness(), vr_death.dark), vr_death.blink);
		char body[128] = "";
		long body_index = dead_body();

		if (phase != vr_death.phase)
		{
			vr_death_log_phase_end();
			vr_death.phase_frames = 0;
			vr_death.phase_distance = 0.0f;
			vr_death.phase_turn = 0.0f;
			vr_death.phase_largest_move = 0.0f;
			vr_death.phase_largest_turn = 0.0f;
			vr_death.phase_seen_distance = 0.0f;
			vr_death.phase_seen_turn = 0.0f;
			vr_death.phase_body_seen = 0;
		}
		vr_death.phase_frames++;
		vr_death.phase_distance += move;
		vr_death.phase_turn += turn;
		vr_death.phase_largest_move = MAX(vr_death.phase_largest_move, move);
		vr_death.phase_largest_turn = MAX(vr_death.phase_largest_turn, turn);
		if (darkness < 1.0f && vr_death.last_darkness < 1.0f)
		{
			/* (seen: from a frame not in full darkness to another) */
			vr_death.phase_seen_distance += move;
			vr_death.phase_seen_turn += turn;
		}
		if (body_index != NONE)
		{
			/* (the body's middle and head from the eye: how far, across and
			up in the view, and whether in it) */
			real_point3d head;
			real_vector3d to, left;
			real across, above, ahead, distance;
			boolean seen;

			unit_get_head_position(body_index, &head);
			vector_from_points3d(position, &object_get(body_index)->object.bounding_sphere_center, &to);
			distance = magnitude3d(&to);
			cross_product3d(up, forward, &left);
			ahead = dot_product3d(&to, forward);
			across = (real)atan2(-dot_product3d(&to, &left), ahead);
			above = (real)atan2(dot_product3d(&to, up), ahead);
			seen = ahead > 0.0f && fabsf(across) < half_across && fabsf(above) < half_up;
			if (seen && darkness < 1.0f)
				vr_death.phase_body_seen++;
			sprintf(body, "; the body %.2f m away, %.0f degrees right, %.0f up%s, its head %.2f m below the eye",
				distance * METRES_PER_WORLD_UNIT, across * DEGREES_PER_RADIAN, above * DEGREES_PER_RADIAN,
				seen ? " (in view)" : "", (position->z - head.z) * METRES_PER_WORLD_UNIT);
		}
		platform_log("vr death log: frame %ld %s: moved %.1f mm, turned %.2f degrees; tint %.2f, dimmed %.2f, dark %.2f%s",
			vr_death.frame, death_phase_names[phase], move * 1000.0f, turn, vr_death.tint,
			BODY_NEAR_DIM * vr_death.near, darkness, body);
	}
	vr_death.phase = phase;
	vr_death.last_valid = TRUE;
	vr_death.last_position = *position;
	vr_death.last_forward = *forward;
	vr_death.last_up = *up;
	vr_death.last_darkness = MAX(MAX(vr_seat_darkness(), vr_death.dark), vr_death.blink);
}

#endif
