/*
VR_GRENADE.C

The VR mode's grenade thrown with the left hand (HALO_VR; vr.grenade_throw =
"gesture", port/linux/README.md "VR"): the game's side of the gesture that
port/linux/src/vr.c sees, for player_control.c, units.c and
first_person_weapons.c.

The game throws a grenade in an animation: the throw button starts the
unit's throw animation (unit_throw_grenade_begin), its third frame puts the
grenade in the unit's left hand (unit_throw_grenade_move_to_hand), and its
key frame lets it go (unit_throw_grenade_release), from just before the
eyes, along the aim, at the unit's grenade velocity; let go before then (the
animation cut short), it is thrown more weakly, by the share of the ticks to
the key frame it was held. The first-person weapon plays a throw animation
of its own beside it (first_person_weapons.c), which lowers the weapon and
marks no release.

The hand's swing presses the throw button (vr.c, as the left trigger),
through the game's input and the netcode as any throw is; the hand's
release then times the throw: the animations (the unit's, which others see,
and the first-person one, released on the unit's key frame) wait on the
frame before the key frame while the hand still holds the grenade, and go
on to it at once when the hand lets go sooner, the grenade put in the hand
first if it is not yet there. The grenade then leaves from the hand, along
the hand's throw, as hard as the hand threw it (never harder than the
game's throw). With the hands the controllers' (vr.hands), the first-person
throw is not played: the left hand is the player's own, and the weapon
stays in the right. While the hand throws, the player's facing turns along
the throw (player_control.c), which the netcode carries: a host's or
another player's copy of the throw, released when its own animation lets
go, goes the same way.

From the grip taking it until the game throws it, the grenade (the current
type's projectile's model) is drawn in the left hand, in the first-person
pass, whatever the hands; the game's own grenade hangs from the hand of the
player's body, which is not drawn in first person. The thrown grenade
starts where the held one was drawn last, and is drawn from there
(render_interpolation_object_from), not from the body's hand.

At game.sim_rate 60 or 120 (sim_rate.h) the hand's release does not wait
for a tick: the fast phase's VR slot (_sim_substep_vr, vr_grenade_substep)
lets the grenade go at the first sub-step after the frame the hand let go
in, from the hand, along its throw, as the tick would (vr_grenade_release's
place and velocity, the game's own release's way for a player: from the
eye of that sub-step, the biped having moved through it), and gives it the
head start and the hold of a projectile made at that sub-step
(projectile_substep_launch, projectiles.c): a whole number of 30ths of a
second after the hand let go it is where a grenade let go of at 30 is that
long after. The throw animation goes on to the frame before its release, as
the next tick's would at 30 (the hand having let go), so the body and the
first-person weapon play the throw as at 30. A release the tick finds first
(the hand let go before the grenade was in it, or in the frame of the
tick's own work) is the tick's, as ever, and has its head start in the
tick's update. At 30 the slot does nothing: the throw is the tick's alone,
as it always was. (A network game, or a film, is at 30.)
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "items/projectiles.h"
#include "items/weapons.h"
#include "models/model_definitions.h"
#include "models/models.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "units/unit_definitions.h"
#include "units/units.h"

#include "sim_rate.h"
#include "../src/vr.h"

#include <float.h>
#include <math.h>
#include <stdio.h>

/* the grenade held in the left hand (vr_grenade_draw): its middle from the
controller's grip, in metres along where the hand points, to its right (the
left palm's side) and up; the hand's fingers close about it there */
#define HELD_FORWARD 0.02f
#define HELD_RIGHT 0.03f
#define HELD_UP -0.01f
/* its size, of the game's: the game's grenades are large for a human hand
(a frag about 20 cm), but smaller here, the one thrown would grow as it
leaves the hand */
#define HELD_SCALE 0.6f
/* drawn at its finest detail (as many pixels as its finest level wants) */
#define HELD_DETAIL_PIXELS 1000.0f
/* the plasma grenade (the game's second type) lit by its own light, as a
live one is (its projectile's attached light, blue): added to the light
about the hand */
static real_rgb_color const plasma_glow = { 0.30f, 0.55f, 1.00f };
#define PLASMA_GLOW_STRENGTH 0.8f

void platform_log(char const *format, ...);
char const *config_string(char const *name);
/* vr_render.c's */
boolean vr_render_left_hand(real_matrix4x3 *hand, real *units);
/* render_interpolation.c's */
void render_interpolation_object_from(long object_index, real_point3d const *position);

static struct
{
	/* the unit whose throw (its throw animation playing) is known, else
	NONE, and whether the hand times it (else it is the button's); whether
	the hand has let go of its grenade, the unit's throw animation's key
	frame, and whether the animations' holds were logged (debug.vr_throw_log:
	the unit's, the first-person one's) */
	long unit_index;
	boolean hand;
	boolean released;
	short key_frame_index;
	boolean hold_logged[2];
	/* the grenade drawn in the hand last (vr_grenade_draw), in the world:
	where the thrown one starts */
	boolean drawn_valid;
	real_matrix4x3 drawn;
	/* debug.vr_test_grenades: the unit given them */
	long test_unit_index;
} vr_grenade = { NONE, FALSE, FALSE, 0, { FALSE, FALSE }, FALSE, { 0 }, NONE };

static void vr_grenade_substep(short substep_index, short substep_count, real seconds);

/* the first player's unit, which the VR mode plays */
static long vr_grenade_player_unit(void)
{
	long player_index = local_player_get_player_index(0);

	if (!halo_vr_running() || player_index == NONE)
		return NONE;
	return player_get(player_index)->unit_index;
}

/* whether the unit's throw is the hand's: the VR player's, begun while the
hand swings or has let go, until its throw is over; the gesture as it is */
static boolean vr_grenade_hand_throw(long unit_index, struct halo_vr_throw *throw_state)
{
	struct unit_datum *unit;
	int phase = halo_vr_throw(throw_state);

	if (unit_index == NONE || unit_index != vr_grenade_player_unit())
		return FALSE;
	unit = unit_get(unit_index);
	switch (unit->unit.grenade_throw_state)
	{
	case _unit_grenade_throw_idle:
		vr_grenade.unit_index = NONE;
		break;
	case _unit_grenade_throw_wind_up:
	case _unit_grenade_throw_in_hand:
		if (vr_grenade.unit_index != unit_index)
		{
			vr_grenade.unit_index = unit_index;
			vr_grenade.hand = phase != HALO_VR_THROW_NONE;
			vr_grenade.released = FALSE;
			vr_grenade.key_frame_index = 0;
			vr_grenade.hold_logged[0] = vr_grenade.hold_logged[1] = FALSE;
			if (throw_state->log)
				platform_log("vr grenade: the game's throw began, tick %ld, %s", game_time_get(),
					phase == HALO_VR_THROW_SWING ? "the hand swinging" : phase == HALO_VR_THROW_RELEASED ?
					"the hand having let go" : "of the button");
		}
		break;
	default:
		break;
	}
	return vr_grenade.unit_index == unit_index && vr_grenade.hand;
}

/* player_control.c, each frame, for the first local player's unit (aiming:
the controller aims it): whether the game would throw a grenade now, for
the hand to take one; the hand's throw over with the game's; and while the
hand throws, the facing along the throw (TRUE, yaw and pitch) */
boolean vr_grenade_facing(long unit_index, boolean aiming, real *yaw, real *pitch)
{
	struct halo_vr_throw throw_state;
	struct unit_datum *unit;
	long weapon_index;
	short grenade_type;
	boolean hand;

	/* (the fast phase's VR slot: the hand's release at its sub-step) */
	sim_substep_register(_sim_substep_vr, vr_grenade_substep);
	if (unit_index == NONE)
	{
		halo_vr_throw_ready(FALSE);
		return FALSE;
	}
	unit = unit_get(unit_index);
	/* debug.vr_test_grenades, "<frags> <plasma grenades>": the unit's, as it
	first aims (for tests of the grenade held in the hand) */
	if (aiming && unit_index != vr_grenade.test_unit_index)
	{
		int counts[2];

		vr_grenade.test_unit_index = unit_index;
		if (sscanf(config_string("debug.vr_test_grenades"), "%d %d", &counts[0], &counts[1]) == 2)
		{
			unit->unit.grenade_counts[0] = (char)counts[0];
			unit->unit.grenade_counts[1] = (char)counts[1];
			platform_log("vr grenade test: %d frag and %d plasma grenades", counts[0], counts[1]);
		}
	}
	weapon_index = unit_inventory_get_weapon(unit_index, unit->unit.current_weapon_index);
	grenade_type = unit_get_current_grenade_type(unit_index);
	/* (the rules of unit_throw_grenade_begin: one to throw, on foot, a
	weapon that lets the hand go; the animation's state it checks then) */
	halo_vr_throw_ready(aiming && unit->object.parent_object_index == NONE && grenade_type != NONE &&
		unit_get_grenade_count(unit_index, grenade_type) > 0 &&
		unit->unit.grenade_throw_state == _unit_grenade_throw_idle &&
		(weapon_index == NONE || !weapon_prevents_grenade_throwing(weapon_index)));
	hand = vr_grenade_hand_throw(unit_index, &throw_state);
	if (hand && throw_state.phase != HALO_VR_THROW_NONE &&
		unit->unit.grenade_throw_state == _unit_grenade_throw_ending)
	{
		/* (thrown, or let go of as the animation was cut short: by a
		flinch, a death) */
		halo_vr_throw_done();
		throw_state.phase = HALO_VR_THROW_NONE;
	}
	if (!aiming || throw_state.phase == HALO_VR_THROW_NONE)
		return FALSE;
	*yaw = throw_state.yaw;
	*pitch = throw_state.pitch;
	return TRUE;
}

/* units.c, as the unit's throw begins: whether the first-person weapon
plays its throw: not the hand's throw with the hands the controllers'
(vr.hands): the left hand is the player's own, which throws, and the weapon
stays in the right (the throw lowers it out of the hand) */
boolean vr_grenade_first_person_throw(long unit_index)
{
	struct halo_vr_throw throw_state;

	if (!vr_grenade_hand_throw(unit_index, &throw_state) || !throw_state.hands)
		return TRUE;
	if (throw_state.log)
		platform_log("vr grenade: the first-person weapon stays in the %s hand", halo_vr_left_handed() ? "left" : "right");
	return FALSE;
}

/* units.c and first_person_weapons.c: one of the throw's animations (the
unit's, or the first-person weapon's of the unit's player), at frame_index,
about to go on a frame, its key frame the release: FALSE holds it where it
is, the hand still holding the grenade a frame before the release; the hand
having let go before then, frame_index is moved to the frame before the key
frame, which the animation then goes on to (TRUE). The first-person throws
mark no key frame: theirs is the unit's (both begin on the same tick, and
the game lets the grenade go at the unit's). */
boolean vr_grenade_throw_advance(long unit_index, short *frame_index, short key_frame_index, boolean first_person)
{
	struct halo_vr_throw throw_state;
	char const *which = first_person ? "first-person" : "unit's";

	if (!vr_grenade_hand_throw(unit_index, &throw_state))
		return TRUE;
	if (!first_person)
		vr_grenade.key_frame_index = key_frame_index;
	else if (key_frame_index <= 0)
		key_frame_index = vr_grenade.key_frame_index;
	if (key_frame_index <= *frame_index)
		return TRUE;
	if (throw_state.phase == HALO_VR_THROW_RELEASED)
		vr_grenade.released = TRUE;
	if (vr_grenade.released)
	{
		if (*frame_index + 1 < key_frame_index)
		{
			if (throw_state.log)
				platform_log("vr grenade: tick %ld: the %s animation on from frame %d to its release, %d",
					game_time_get(), which, *frame_index, key_frame_index);
			*frame_index = key_frame_index - 1;
		}
		return TRUE;
	}
	if (throw_state.phase == HALO_VR_THROW_SWING && *frame_index + 1 >= key_frame_index)
	{
		if (throw_state.log && !vr_grenade.hold_logged[first_person])
		{
			platform_log("vr grenade: tick %ld: the %s animation held at frame %d, before its release, %d, "
				"until the hand lets go", game_time_get(), which, *frame_index, key_frame_index);
			vr_grenade.hold_logged[first_person] = TRUE;
		}
		return FALSE;
	}
	return TRUE;
}

/* first_person_weapons.c, as the first-person weapon is drawn (flags: its
model's): the grenade the left hand holds, the current type's projectile's
model in the hand, from when the grip takes it until the game throws it,
whatever the hands (vr.hands). Let go of, it stays where the hand let it go
until the game throws it from there (vr_grenade_release). (The game's own
grenade, in the hand of the player's body from the throw's third frame, is
not drawn in first person, as the body is not.) */
void vr_grenade_draw(unsigned long flags)
{
	struct halo_vr_throw throw_state;
	struct game_globals *globals;
	struct game_globals_grenade *grenade;
	struct model *model;
	real_matrix4x3 node_matrices[MAXIMUM_NODES_PER_MODEL];
	real function_values[NUMBER_OF_OUTGOING_OBJECT_FUNCTIONS];
	struct render_lighting lighting;
	long unit_index = vr_grenade_player_unit();
	long model_index;
	short grenade_type, function_index;

	halo_vr_throw(&throw_state);
	if (unit_index == NONE || !throw_state.held)
	{
		vr_grenade.drawn_valid = FALSE;
		return;
	}
	grenade_type = unit_get_current_grenade_type(unit_index);
	globals = scenario_get_game_globals();
	if (grenade_type < 0 || grenade_type >= globals->grenades.count)
		return;
	grenade = TAG_BLOCK_GET_ELEMENT(&globals->grenades, grenade_type, struct game_globals_grenade);
	if (grenade->projectile.index == NONE)
		return;
	model_index = object_definition_get(grenade->projectile.index)->object.model.index;
	if (model_index == NONE)
		return;
	model = model_definition_get(model_index);
	if (model->nodes.count <= 0 || model->nodes.count > MAXIMUM_NODES_PER_MODEL)
		return;
	/* (let go of: where it was drawn last) */
	if (throw_state.phase != HALO_VR_THROW_RELEASED || !vr_grenade.drawn_valid)
	{
		real_matrix4x3 hand;
		real units;
		/* (a left-hander's in the right hand, whose palm is to its left) */
		real right = halo_vr_left_handed() ? -HELD_RIGHT : HELD_RIGHT;

		if (!vr_render_left_hand(&hand, &units))
			return;
		hand.position.x += (hand.forward.i * HELD_FORWARD - hand.left.i * right + hand.up.i * HELD_UP) * units;
		hand.position.y += (hand.forward.j * HELD_FORWARD - hand.left.j * right + hand.up.j * HELD_UP) * units;
		hand.position.z += (hand.forward.k * HELD_FORWARD - hand.left.k * right + hand.up.k * HELD_UP) * units;
		vr_grenade.drawn = hand;
		vr_grenade.drawn_valid = TRUE;
	}
	/* the model's nodes in their default pose about the hand */
	model_get_node_matrices(model, node_matrices, &vr_grenade.drawn.position, &vr_grenade.drawn.forward,
		&vr_grenade.drawn.up);
	if (HELD_SCALE != 1.0f)
	{
		short node_index;

		for (node_index = 0; node_index < model->nodes.count; node_index++)
		{
			real_point3d *position = &node_matrices[node_index].position;

			node_matrices[node_index].scale *= HELD_SCALE;
			position->x = vr_grenade.drawn.position.x + (position->x - vr_grenade.drawn.position.x) * HELD_SCALE;
			position->y = vr_grenade.drawn.position.y + (position->y - vr_grenade.drawn.position.y) * HELD_SCALE;
			position->z = vr_grenade.drawn.position.z + (position->z - vr_grenade.drawn.position.z) * HELD_SCALE;
		}
	}
	/* (its shader's functions as a live grenade's: the plasma grenade's
	glow; with none, its self-illumination is off) */
	for (function_index = 0; function_index < NUMBER_OF_OUTGOING_OBJECT_FUNCTIONS; function_index++)
		function_values[function_index] = 1.0f;
	lighting = *object_get_cached_render_lighting(unit_index, FLT_MAX);
	if (grenade_type == 1)
	{
		lighting.ambient_color.red += plasma_glow.red * PLASMA_GLOW_STRENGTH;
		lighting.ambient_color.green += plasma_glow.green * PLASMA_GLOW_STRENGTH;
		lighting.ambient_color.blue += plasma_glow.blue * PLASMA_GLOW_STRENGTH;
	}
	render_model(model_index, HELD_DETAIL_PIXELS, node_matrices, NULL, NULL, function_values, &lighting,
		&vr_grenade.drawn.position, 0.0f, NULL, unit_index, 0, flags);
}

/* the hand's throw, let go of (throw_state), for the unit's grenade: where
it leaves from, the hand (from the unit's eye now, as from the recentred
head, or where the frames drew it in the hand last: they draw the eye
between ticks, and the hand later than the tick's), and its velocity (world
units a tick) along the hand's throw, as hard as the hand threw it of the
game's throw's, game_velocity (never harder: the power is at most 1); FALSE
with no direction to throw it in */
static boolean vr_grenade_hand_release(long unit_index, struct halo_vr_throw const *throw_state,
	real_vector3d const *game_velocity, real_point3d *hand, real_vector3d *velocity, real *speed)
{
	real_point3d eye;
	real_vector3d direction;
	real units, c, s;

	c = (real)cos(throw_state->body_yaw);
	s = (real)sin(throw_state->body_yaw);
	units = throw_state->world_units_per_metre;
	/* (the head's place in the world is the eye's, as the view's is,
	vr_render.c) */
	unit_get_camera_position(unit_index, &eye);
	hand->x = eye.x + (throw_state->position[0] * c - throw_state->position[1] * s) * units;
	hand->y = eye.y + (throw_state->position[0] * s + throw_state->position[1] * c) * units;
	hand->z = eye.z + throw_state->position[2] * units;
	if (vr_grenade.drawn_valid)
		*hand = vr_grenade.drawn.position;
	direction.i = throw_state->direction[0] * c - throw_state->direction[1] * s;
	direction.j = throw_state->direction[0] * s + throw_state->direction[1] * c;
	direction.k = throw_state->direction[2];
	if (normalize3d(&direction) <= 0.0f)
		return FALSE;
	*speed = magnitude3d(game_velocity) * throw_state->power;
	scale_vector3d(&direction, *speed, velocity);
	return TRUE;
}

/* the hand's grenade thrown: the hand's throw over */
static void vr_grenade_thrown(struct halo_vr_throw const *throw_state, real speed)
{
	short substep_index = sim_substep_index();

	vr_grenade.released = TRUE;
	vr_grenade.drawn_valid = FALSE;
	halo_vr_throw_done();
	if (throw_state->log)
	{
		platform_log("vr grenade: tick %ld%s%d: thrown from the hand (%.2f %.2f %.2f m from the head: forward, "
			"left, up), %.2f world units a second (power %.2f), pitched %.0f degrees up", game_time_get(),
			substep_index != NONE ? " sub-step " : "", substep_index != NONE ? substep_index : 0,
			throw_state->position[0], throw_state->position[1], throw_state->position[2], speed * TICKS_PER_SECOND,
			throw_state->power, throw_state->pitch * 180.0f / _pi);
	}
}

/* units.c, as the unit lets its grenade go with the velocity the game
gives it (world units a tick), in the tick: the hand's throw (TRUE), the
grenade moved to the hand and its velocity turned along the throw, as hard
as the hand threw it of the game's; FALSE when it is not the hand's (the
game's own, or cut short before the hand let go). At 60 and 120 (a release
the fast phase did not see first: vr_grenade_substep) its head start is to
come in the tick's update, as a projectile made in the tick's own work has
it (projectile_substep_launch) */
boolean vr_grenade_release(long unit_index, long grenade_index, real_vector3d *initial_velocity)
{
	struct halo_vr_throw throw_state;
	real_vector3d game_velocity = *initial_velocity;
	real_point3d hand;
	real speed;

	if (!vr_grenade_hand_throw(unit_index, &throw_state) || throw_state.phase != HALO_VR_THROW_RELEASED ||
		!vr_grenade_hand_release(unit_index, &throw_state, &game_velocity, &hand, initial_velocity, &speed))
	{
		return FALSE;
	}
	object_translate(grenade_index, &hand, NULL);
	render_interpolation_object_from(grenade_index, &hand);
	projectile_substep_launch(grenade_index);
	vr_grenade_thrown(&throw_state, speed);
	return TRUE;
}

/* the fast phase's VR slot (_sim_substep_vr, sim_rate.h; at 30 its one
sub-step leaves the throw to the tick): the hand having let go of the
grenade the VR player's unit holds in its throw, it is let go of now, at
this sub-step, as unit_throw_grenade_release lets a player's go (not early:
the hand times it), from the hand along its throw (vr_grenade_hand_release,
with the eye of this sub-step: the slot runs after the bipeds'), its head
start and its hold those of a projectile made now
(projectile_substep_launch). The throw animation is put on the frame before
its release, which the next tick's update goes on to, as the next tick's
would at 30 (vr_grenade_throw_advance); the release there finds the grenade
gone. A grenade not yet in the hand is the tick's (its throw puts it there
first) */
static void vr_grenade_substep(short substep_index, short substep_count, real seconds)
{
	struct halo_vr_throw throw_state;
	struct unit_datum *unit;
	struct unit_definition *unit_definition;
	struct object_datum *grenade;
	real_vector3d game_velocity, velocity;
	real_point3d hand, camera;
	real speed;
	long unit_index, grenade_index;

	(void)substep_index;
	(void)seconds;
	if (substep_count <= 1 || (unit_index = vr_grenade_player_unit()) == NONE)
		return;
	unit = unit_get(unit_index);
	grenade_index = unit->unit.grenade_object_index;
	if (unit->unit.grenade_throw_state != _unit_grenade_throw_in_hand || grenade_index == NONE ||
		unit->unit.animation.state != _unit_state_throw_grenade ||
		!vr_grenade_hand_throw(unit_index, &throw_state) || throw_state.phase != HALO_VR_THROW_RELEASED)
	{
		return;
	}
	/* (the game's throw, a player's: along the aim, at the unit's grenade
	velocity) */
	unit_definition = unit_definition_get(unit->definition_index);
	scale_vector3d(&unit->unit.aiming_vector, unit_definition->unit.grenade_velocity / TICKS_PER_SECOND,
		&game_velocity);
	if (!vr_grenade_hand_release(unit_index, &throw_state, &game_velocity, &hand, &velocity, &speed))
		return;
	object_detach(grenade_index);
	object_translate(grenade_index, &hand, NULL);
	render_interpolation_object_from(grenade_index, &hand);
	grenade = object_get(grenade_index);
	subtract_vectors3d(&velocity, &grenade->object.translational_velocity, &velocity);
	projectile_accelerate(grenade_index, &velocity);
	unit->unit.grenade_throw_state = _unit_grenade_throw_ending;
	unit->unit.grenade_object_index = NONE;
	if (vr_grenade.key_frame_index > 0 && unit->object.animation.state.frame_index + 1 < vr_grenade.key_frame_index)
		unit->object.animation.state.frame_index = vr_grenade.key_frame_index - 1;
	vr_grenade_thrown(&throw_state, speed);
	unit_get_camera_position(unit_index, &camera);
	if (!object_force_inside_bsp(grenade_index, &camera))
	{
		/* (gone at once, as a projectile deleted in a sub-step is) */
		object_delete_immediately(grenade_index);
		return;
	}
	projectile_substep_launch(grenade_index);
}

#endif
