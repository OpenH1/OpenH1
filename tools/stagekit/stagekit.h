/*
STAGEKIT.H

What StageKit's parts share (README.md): the tags being built (cache.c),
collision geometry (collision.c), what every map has (stage.c), the content
scenarios place (props.c), and a scenario itself (scenarios/).
*/

#ifndef __STAGEKIT_H
#define __STAGEKIT_H
#pragma once

/* ---------- headers */

#include <stdio.h>
#include <string.h>

#include "cseries.h"
#include "ai/actor_definitions.h"
#include "ai/actor_types.h"
#include "ai/ai_scenario_definitions.h"
#include "bitmaps/bitmap_group.h"
#include "devices/device_machines.h"
#include "game/game_globals.h"
#include "hs/hs_scenario_definitions.h"
#include "interface/weapon_hud_interface_definition.h"
#include "items/projectile_definitions.h"
#include "items/weapon_definitions.h"
#include "models/model_animation_definitions.h"
#include "models/model_definitions.h"
#include "physics/collision_bsp_definitions.h"
#include "physics/collision_model_definitions.h"
#include "physics/physics_definitions.h"
#include "sound/sound_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "text/font_group.h"
#include "units/biped_definitions.h"


/* (cseries.h and the port's stdio.h name their own; the C library's here) */
#undef printf
#undef fprintf
#undef snprintf
#undef sprintf
#undef fopen
#undef strlen
#undef strcpy
#undef strcmp
#undef memcpy
#undef memset

/* ---------- constants */

/* the chamber: a box from (-CHAMBER_HALF_WIDTH, -CHAMBER_HALF_WIDTH, 0) to
(CHAMBER_HALF_WIDTH, CHAMBER_HALF_WIDTH, CHAMBER_HEIGHT), in world units */
#define CHAMBER_HALF_WIDTH 500.f
#define CHAMBER_HEIGHT 100.f

#define FOURCC(a, b, c, d) (((unsigned long)(a) << 24) | ((unsigned long)(b) << 16) | ((c) << 8) | (d))

/* ---------- structures */

/* the map being built, as a scenario sees it */
struct stage
{
	struct scenario *scenario;
	struct structure_bsp *bsp;
	/* the players' biped (stage.c), which a scenario may place for the AI,
	and its definition, which a scenario may give more (prop_biped_body) */
	long biped_handle;
	struct biped_definition *biped;
};

/* a move of an AI's command list (prop_ai_walker): a heading in degrees (0
along x, 90 along y) and how far along it, in world units */
struct ai_walker_move
{
	real heading;
	real distance;
};

/* a projectile's flight: its speed (world units a second), the engine's
gravity's scale, and its fuse in seconds (0: it goes off where it strikes);
with a fuse, on a surface of a material (stage_chamber_material) it bounces:
it keeps bounce of its speed into the surface and loses friction of its
speed along it, at each strike */
struct projectile_shape
{
	real speed;
	real gravity_scale;
	real fuse;
	real bounce;
	real friction;
};

struct stagekit_scenario
{
	char const *name;
	char const *description;
	/* places what the scenario adds to the stage (stage.c) */
	void (*build)(struct stage *stage);
};

/* ---------- prototypes/CACHE.C */

/* the tag cache at the game's address for a Custom Edition map's tags; the
structure BSP's room at its top */
void cache_open(void);
void *cache_allocate(unsigned long size);
void *cache_block(struct tag_block *block, long count, unsigned long element_size);
/* one more element at the block's end, zeroed */
void *cache_block_add(struct tag_block *block, unsigned long element_size);
void *cache_bsp_allocate(unsigned long size);
void *cache_bsp_block(struct tag_block *block, long count, unsigned long element_size);
/* a new tag of the group (and the groups it inherits from), its data zeroed */
long cache_tag_new(unsigned long group, unsigned long parent, unsigned long grandparent, char const *name,
	unsigned long size, void **data);
void cache_tag_no_data(long handle);
void cache_reference(struct tag_reference *reference, long handle);
/* a reference to no tag, of the group it names */
void cache_no_reference(struct tag_reference *reference, unsigned long group);
void cache_set_scenario(long handle);
/* where the structure BSP is in the file, how big, and where in memory */
void cache_bsp_place(struct scenario_structure_bsp_reference *reference);
/* the map */
void cache_write(char const *path, long *tag_count);

/* ---------- prototypes/COLLISION.C */

/* a box's collision: hollow (walls facing in, open inside: the chamber) or
solid (faces out, open outside: an obstacle), its blocks made by block_new */
void box_build(struct collision_bsp *collision, void *(*block_new)(struct tag_block *, long, unsigned long),
	real_rectangle3d const *bounds, boolean solid);

/* ---------- prototypes/STAGE.C */

/* the scenario, the chamber, the globals and interface, the player's biped,
no scripts and a player start, with scenario's content in between */
void stage_build(struct stage *stage, struct stagekit_scenario const *scenario);

/* the chamber's walls, floor and ceiling of a material type (game_globals.h),
which projectiles respond to as their material responses say; none without */
void stage_chamber_material(struct stage *stage, short material_type);

/* ---------- prototypes/PROPS.C */

/* a player start at position (on what stands there), facing along x */
void player_start(struct stage *stage, real_point3d const *position);

/* a model that draws nothing (one node, region and permutation), made once */
long prop_empty_model(void);

/* a scenery box standing at position (bounds in its own space), solid to anything */
void prop_obstacle(struct stage *stage, char const *name, real_rectangle3d const *bounds,
	real_point3d const *position);

/* a platform (bounds in its own space) at position that rises rise in seconds
when its position is set to 1 (device_set_position <name> 1) and comes down
when set to 0, as the game's lifts do; elevator: what stands on it rides it */
void prop_elevator(struct stage *stage, char const *name, real_rectangle3d const *platform,
	real_point3d const *position, real rise, real seconds);

/* the players' biped given a body: a collision model, a box round its capsule,
as the game's bipeds all have one (what finds bodies by their collision finds
it: a lift's riders, device_machines.c), else it has none */
void prop_biped_body(struct stage *stage);

/* an AI's biped: the players' biped standing at position facing along x,
named name, which the console makes an actor of the variant variant_name
(ai_attach_free <name> <variant_name>). The floor is walkable for the AI's
paths */
void prop_ai_walker(struct stage *stage, char const *name, char const *variant_name, real_point3d const *position);

/* a command list named name that an AI walks (ai_command_list_by_unit <unit>
<name>): each move in turn, as far as it goes along its heading, by the AI's
throttle */
void prop_ai_command_list(struct stage *stage, char const *name, struct ai_walker_move const *moves,
	short move_count);

/* a projectile of the shape, which draws nothing */
long prop_projectile(char const *name, struct projectile_shape const *shape);

/* a weapon of one trigger that fires the projectile rate_of_fire times a
second while it is held, from no magazine (it never runs out) */
long prop_weapon(char const *name, long projectile_handle, real rate_of_fire);

/* the players start with the weapon in their hands (the scenario's first
starting profile) */
void player_loadout(struct stage *stage, long weapon_handle);

/* a scenery box (bounds in its own space) at position, turned by yaw, pitch
and roll in degrees (a ramp: a box pitched up), solid to anything */
void prop_turned_obstacle(struct stage *stage, char const *name, real_rectangle3d const *bounds,
	real_point3d const *position, real yaw, real pitch, real roll);

/* a car (tools/scorecard's vehicle scenarios): four wheels it drives on, steers
with and is held up by, a body above them, one seat a player drives it from,
as a game's jeep is; its tag made once (a scenario's values, car_*) */
long prop_car(void);

/* the vehicle placed at position, facing yaw degrees from x, its console name
name */
void prop_vehicle(struct stage *stage, char const *name, long vehicle_handle, real_point3d const *position,
	real yaw);

/* ---------- prototypes/MAIN.C */

/* a value of the scenario's, as the command line set it (stagekit <scenario>
<map file> <name>=<value>,...: the scorecard's hidden scenarios vary them),
else value */
real stage_parameter(char const *name, real value);

#endif // __STAGEKIT_H
