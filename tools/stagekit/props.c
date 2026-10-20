/*
PROPS.C

The content scenarios place (README.md): each a function that makes its tags
and places them in the scenario. Each value says where it comes from
(README.md, "Values").
*/

/* ---------- headers */

#include "stagekit.h"

/* ---------- constants */

enum
{
	/* (device_machines.c names a door, 0, which opens by itself, and a
	platform, 2, which cycles by itself; any other has no behaviour of its own
	and moves only where it is set) */
	MACHINE_TYPE_SET = 1,
};

/* ---------- structures */

/* (damage.c keeps these to itself: its layout, which its size check fixes) */
struct damage_region
{
	char name[TAG_STRING_LENGTH+1];
	unsigned long flags;
	long unused0;
	real damage_threshold;
	long unused1[3];
	struct tag_reference destroyed_effect;
	struct tag_block permutations;
};

struct damage_permutation
{
	char name[TAG_STRING_LENGTH+1];
};

typedef char damage_region_size_check[sizeof(struct damage_region) == 0x54 ? 1 : -1];

/* (and these, which their files keep to themselves: what is set here, the rest
zero, each its tag's size) */
struct model_geometry
{
	unsigned long flags;
	unsigned char unused[0x20];
	struct tag_block parts;
};
struct animation_node
{
	char name[TAG_STRING_LENGTH+1];
	short next_sibling_node_index;
	short first_child_node_index;
	short parent_node_index;
	unsigned char unused[0x1A];
};
struct device_animations
{
	unsigned char unused[0x54];
	struct tag_block animations;
};
struct scenery_placement
{
	struct scenario_object_datum object;
	unsigned char scenery[0x48 - sizeof(struct scenario_object_datum)];
};
struct machine_placement
{
	struct scenario_machine_datum machine;
	unsigned char unused[0x40 - sizeof(struct scenario_machine_datum)];
};
typedef char model_geometry_size_check[sizeof(struct model_geometry) == 0x30 ? 1 : -1];
typedef char animation_node_size_check[sizeof(struct animation_node) == 0x40 ? 1 : -1];
typedef char device_animations_size_check[sizeof(struct device_animations) == 0x60 ? 1 : -1];
typedef char scenery_placement_size_check[sizeof(struct scenery_placement) == 0x48 ? 1 : -1];
typedef char machine_placement_size_check[sizeof(struct machine_placement) == 0x40 ? 1 : -1];

/* ---------- public code */

void player_start(
	struct stage *stage,
	real_point3d const *position)
{
	struct player_starting_location *start = cache_block_add(&stage->scenario->players, sizeof(*start));

	/* (a little above it: where a player is placed falls onto what is there) */
	start->position = *position;
	start->position.z += 0.1f;
	start->structure_bsp_reference_index = 0;
}

/* a model that draws nothing, made once: an object without a model is
invisible, nothing collides with an invisible object (objects.c, collisions.c),
and a unit's animations read its model unchecked (object_start_interpolation) */
long prop_empty_model(
	void)
{
	static long handle = NONE;
	struct model *render;
	struct model_node *root;
	struct model_region *region;
	struct model_region_permutation *permutation;
	void *data;

	if (handle != NONE)
		return handle;
	handle = cache_tag_new(FOURCC('m', 'o', 'd', '2'), NONE, NONE, "stagekit\\empty", sizeof(struct model), &data);
	render = data;
	root = cache_block(&render->nodes, 1, sizeof(*root));
	strcpy(root->name, "root");
	root->next_sibling_node_index = root->first_child_node_index = root->parent_node_index = NONE;
	root->default_rotation.w = 1.f;
	root->runtime_default_inverse_matrix.scale = 1.f;
	root->runtime_default_inverse_matrix.forward.i = 1.f;
	root->runtime_default_inverse_matrix.left.j = 1.f;
	root->runtime_default_inverse_matrix.up.k = 1.f;
	region = cache_block(&render->regions, 1, sizeof(*region));
	strcpy(region->name, "box");
	permutation = cache_block(&region->permutations, 1, sizeof(*permutation));
	strcpy(permutation->name, "default");
	/* (one geometry of no parts: nothing to draw) */
	cache_block(&render->geometries, 1, sizeof(struct model_geometry));
	return handle;
}

/* how far a box's farthest corner is from its object's origin */
static real box_reach(
	real_rectangle3d const *bounds)
{
	real dx = MAX(-bounds->x0, bounds->x1), dy = MAX(-bounds->y0, bounds->y1), dz = MAX(-bounds->z0, bounds->z1);

	return square_root(dx * dx + dy * dy + dz * dz);
}

/* a collision model of one node: a solid box (bounds in its object's space),
whose bsp is the one its region's permutation picks (one region, one permutation) */
static long box_collision_model(
	char const *name,
	real_rectangle3d const *bounds)
{
	struct collision_model *model;
	struct collision_node *node;
	struct collision_bsp *box;
	struct damage_region *region;
	struct damage_permutation *permutation;
	void *data;
	long handle = cache_tag_new(FOURCC('c', 'o', 'l', 'l'), NONE, NONE, name, sizeof(struct collision_model), &data);

	model = data;
	node = cache_block(&model->nodes, 1, sizeof(*node));
	strcpy(node->name, "box");
	region = cache_block(&model->resistance.regions, 1, sizeof(*region));
	permutation = cache_block(&region->permutations, 1, sizeof(*permutation));
	strcpy(region->name, "box");
	strcpy(permutation->name, "default");
	model->resistance.indirect_damage_material_index = NONE;
	node->region_index = 0;
	node->parent_node_index = node->next_sibling_node_index = node->first_child_node_index = NONE;
	box = cache_block(&node->bsps, 1, sizeof(*box));
	box_build(box, cache_block, bounds, TRUE);
	model->pathfinding_box = *bounds;
	return handle;
}

/* a scenery box placed, turned (radians) */
static void obstacle_place(
	struct stage *stage,
	char const *name,
	real_rectangle3d const *bounds,
	real_point3d const *position,
	real_euler_angles3d const *rotation)
{
	struct object_definition *scenery;
	struct scenario_object_palette_entry *palette;
	struct scenery_placement *placement;
	long model_handle, scenery_handle;
	void *data;

	model_handle = box_collision_model(name, bounds);

	scenery_handle = cache_tag_new(FOURCC('s', 'c', 'e', 'n'), FOURCC('o', 'b', 'j', 'e'), NONE, name,
		sizeof(struct object_definition), &data);
	scenery = data;
	scenery->object.type = _object_type_scenery;
	cache_reference(&scenery->object.model, prop_empty_model());
	cache_reference(&scenery->object.collision_model, model_handle);
	/* (round the box, a little past its farthest corner) */
	scenery->object.bounding_radius = box_reach(bounds) + 0.1f;

	palette = cache_block_add(&stage->scenario->scenery_palette, sizeof(*palette));
	cache_reference(&palette->reference, scenery_handle);
	placement = cache_block_add(&stage->scenario->scenery, sizeof(*placement));
	placement->object.palette_entry_index = (short)(stage->scenario->scenery_palette.count - 1);
	placement->object.name_index = NONE;
	placement->object.position = *position;
	placement->object.rotation = *rotation;
}

void prop_obstacle(
	struct stage *stage,
	char const *name,
	real_rectangle3d const *bounds,
	real_point3d const *position)
{
	real_euler_angles3d unturned = { { 0.f, 0.f, 0.f } };

	obstacle_place(stage, name, bounds, position, &unturned);
}

void prop_turned_obstacle(
	struct stage *stage,
	char const *name,
	real_rectangle3d const *bounds,
	real_point3d const *position,
	real yaw,
	real pitch,
	real roll)
{
	real_euler_angles3d rotation;

	rotation.yaw = DEGREES_TO_RADIANS(yaw);
	rotation.pitch = DEGREES_TO_RADIANS(pitch);
	rotation.roll = DEGREES_TO_RADIANS(roll);
	obstacle_place(stage, name, bounds, position, &rotation);
}

/* the name a scenario's console lines call an object it places by */
static short object_name_new(
	struct stage *stage,
	char const *name,
	short type,
	short placement_index)
{
	struct scenario_object_name *object_name = cache_block_add(&stage->scenario->object_names, sizeof(*object_name));

	if (strlen(name) >= sizeof(object_name->name))
	{
		fprintf(stderr, "stagekit: the object name %s is longer than %d characters\n", name,
			(int)sizeof(object_name->name) - 1);
		exit(1);
	}
	strcpy(object_name->name, name);
	object_name->runtime_object_type = type;
	object_name->runtime_scenario_datum_index = placement_index;
	return (short)(stage->scenario->object_names.count - 1);
}

void prop_elevator(
	struct stage *stage,
	char const *name,
	real_rectangle3d const *platform,
	real_point3d const *position,
	real rise,
	real seconds)
{
	enum { QUATERNION_ONE = 32767 };
	long frames = MAX(1, (long)(seconds * TICKS_PER_SECOND)) + 1;
	struct animation_graph *graph;
	struct device_animations *device;
	struct animation_graph_animation_index *slots;
	struct animation_node *node;
	struct animation *animation;
	struct machine_definition *machine;
	struct scenario_object_palette_entry *palette;
	struct machine_placement *placement;
	short *rotation;
	real *scale;
	real_point3d *translations;
	long graph_handle, machine_handle, frame;
	void *data;

	/* its rise: one animation of its one node, a frame a tick, which a device
	plays by its position (devices.c: the position slot) */
	graph_handle = cache_tag_new(FOURCC('a', 'n', 't', 'r'), NONE, NONE, name, sizeof(struct animation_graph), &data);
	graph = data;
	device = cache_block(&graph->device_animations, 1, sizeof(*device));
	slots = cache_block(&device->animations, 1, sizeof(*slots));
	slots[0].animation_index = 0;
	node = cache_block(&graph->nodes, 1, sizeof(*node));
	strcpy(node->name, "root");
	node->next_sibling_node_index = node->first_child_node_index = node->parent_node_index = NONE;
	animation = cache_block(&graph->animations, 1, sizeof(*animation));
	strcpy(animation->name, "position");
	/* (a device plays its animations over its nodes at rest: overlays, devices.c) */
	animation->type = _animation_overlay;
	animation->frame_count = (short)frames;
	animation->frame_size = sizeof(real_point3d);
	animation->node_count = 1;
	animation->next_animation_index = NONE;
	animation->sound_index = NONE;
	animation->nodes_with_translation_flags[0] = 1;
	/* (its rotation and scale, the same every frame: the identity) */
	animation->default_data.size = 4 * sizeof(short) + sizeof(real);
	rotation = animation->default_data.address = cache_allocate(animation->default_data.size);
	rotation[3] = QUATERNION_ONE;
	scale = (real *)(rotation + 4);
	*scale = 1.f;
	animation->data.size = frames * sizeof(real_point3d);
	translations = animation->data.address = cache_allocate(animation->data.size);
	for (frame = 0; frame < frames; frame++)
		translations[frame].z = rise * frame / (frames - 1);

	machine_handle = cache_tag_new(FOURCC('m', 'a', 'c', 'h'), FOURCC('d', 'e', 'v', 'i'), FOURCC('o', 'b', 'j', 'e'),
		name, sizeof(struct machine_definition), &data);
	machine = data;
	machine->object.type = _object_type_machine;
	/* (round the platform, wherever it is on its way) */
	machine->object.bounding_radius = box_reach(platform) + rise + 0.1f;
	cache_reference(&machine->object.model, prop_empty_model());
	cache_reference(&machine->object.collision_model, box_collision_model(name, platform));
	cache_reference(&machine->object.animation_graph, graph_handle);
	/* (moved only where it is set, a frame a tick: devices.c moves it by its
	runtime speeds, in position a tick, which a map carries made from its
	times, and reads nothing else of them) */
	machine->machine.type = MACHINE_TYPE_SET;
	machine->device.powered_position_transition_time = seconds;
	machine->device.depowered_position_transition_time = seconds;
	machine->device.runtime_maximum_powered_position_velocity = 1.f / (frames - 1);
	machine->device.runtime_maximum_powered_position_acceleration = 1.f / (frames - 1);
	machine->device.runtime_maximum_depowered_position_velocity = 1.f / (frames - 1);
	machine->device.runtime_maximum_depowered_position_acceleration = 1.f / (frames - 1);
	machine->machine.flags = FLAG(_machine_is_elevator_bit);
	machine->machine.elevator_node_index = 0;

	palette = cache_block_add(&stage->scenario->machine_palette, sizeof(*palette));
	cache_reference(&palette->reference, machine_handle);
	placement = cache_block_add(&stage->scenario->machines, sizeof(*placement));
	placement->machine.object.palette_entry_index = (short)(stage->scenario->machine_palette.count - 1);
	placement->machine.object.name_index = object_name_new(stage, name, _object_type_machine,
		(short)(stage->scenario->machines.count - 1));
	placement->machine.object.position = *position;
	placement->machine.device.power_group_index = NONE;
	placement->machine.device.position_group_index = NONE;
}

/* ---------- weapons and projectiles (tools/scorecard's scenarios) */

long prop_projectile(
	char const *name,
	struct projectile_shape const *shape)
{
	struct projectile_definition *projectile;
	void *data;
	long handle = cache_tag_new(FOURCC('p', 'r', 'o', 'j'), FOURCC('o', 'b', 'j', 'e'), NONE, name,
		sizeof(struct projectile_definition), &data);

	projectile = data;
	projectile->object.type = _object_type_projectile;
	cache_reference(&projectile->object.model, prop_empty_model());
	/* (StageKit's own: a point, a hundredth round) */
	projectile->object.bounding_radius = 0.01f;
	/* (the engine's own units: world units a tick; one with a fuse slows to
	nothing, else a bounce would leave at its final speed, projectiles.c) */
	projectile->projectile.initial_velocity = shape->speed / TICKS_PER_SECOND;
	projectile->projectile.final_velocity = shape->fuse > 0.f ? 0.f : shape->speed / TICKS_PER_SECOND;
	projectile->projectile.air_gravity_scale = shape->gravity_scale;
	projectile->projectile.water_gravity_scale = shape->gravity_scale;
	/* (it keeps its speed, slowed only by its bounces, and flies on as far as
	the chamber goes: a damage range of no length slows it none, and past it a
	projectile of one speed and no fuse or range disappears, projectiles.c,
	which takes the water's upper bound in the air too) */
	projectile->projectile.air_damage_range_lower_bound = 2.f * CHAMBER_HALF_WIDTH * 2.f;
	projectile->projectile.air_damage_range_upper_bound = 2.f * CHAMBER_HALF_WIDTH * 2.f;
	projectile->projectile.water_damage_range_lower_bound = 2.f * CHAMBER_HALF_WIDTH * 2.f;
	projectile->projectile.water_damage_range_upper_bound = 2.f * CHAMBER_HALF_WIDTH * 2.f;
	/* (a fuse from its making, in seconds; none: it goes off where it
	strikes, as it stops there: projectiles.c, a projectile slower than its
	minimum velocity detonates) */
	projectile->projectile.timer_lower_bound = shape->fuse;
	projectile->projectile.timer_upper_bound = shape->fuse;
	projectile->projectile.minimum_velocity = shape->fuse > 0.f ? 0.f : 0.0001f;
	if (shape->fuse > 0.f)
	{
		struct projectile_material_response_definition *responses = cache_block(
			&projectile->projectile.material_responses, NUMBER_OF_MATERIAL_TYPES, sizeof(*responses));
		short material;

		/* (on every material, a bounce: a reflection, its speed into the
		surface and along it each scaled, projectiles.c, whose "parallel" is
		along the surface's normal, its "perpendicular" along the surface;
		stopped on the ground, it rests there until its fuse runs out) */
		for (material = 0; material < NUMBER_OF_MATERIAL_TYPES; material++)
		{
			responses[material].default_response = _projectile_material_response_reflect;
			responses[material].parallel_friction = 1.f - shape->bounce;
			responses[material].perpendicular_friction = shape->friction;
		}
	}
	return handle;
}

long prop_weapon(
	char const *name,
	long projectile_handle,
	real rate_of_fire)
{
	struct weapon_definition *weapon;
	struct weapon_trigger_definition *trigger;
	void *data;
	long handle = cache_tag_new(FOURCC('w', 'e', 'a', 'p'), FOURCC('i', 't', 'e', 'm'), FOURCC('o', 'b', 'j', 'e'),
		name, sizeof(struct weapon_definition), &data);

	weapon = data;
	weapon->object.type = _object_type_weapon;
	cache_reference(&weapon->object.model, prop_empty_model());
	weapon->object.bounding_radius = 0.25f;
	/* (the biped's one weapon class, stage.c's animations) */
	strcpy(weapon->weapon.label, "unarmed");
	/* (a HUD as an unarmed player's: hud_weapon.c reads it unchecked) */
	cache_reference(&weapon->weapon.interface_definition.hud_interface,
		cache_tag_new(FOURCC('w', 'p', 'h', 'i'), NONE, NONE, name, sizeof(struct weapon_hud_interface_definition),
			&data));
	trigger = cache_block(&weapon->weapon.triggers, 1, sizeof(*trigger));
	/* (no magazine: it never runs out, weapons.c) */
	trigger->magazine_index = NONE;
	trigger->rounds_per_shot = 1;
	trigger->projectiles_per_shot = 1;
	trigger->initial_rate_of_fire = rate_of_fire;
	trigger->final_rate_of_fire = rate_of_fire;
	cache_reference(&trigger->projectile, projectile_handle);
	return handle;
}

void player_loadout(
	struct stage *stage,
	long weapon_handle)
{
	struct scenario_starting_profile *profile = cache_block_add(&stage->scenario->starting_profiles,
		sizeof(*profile));

	strcpy(profile->name, "stagekit");
	/* (as a map's profiles give: health and shields whole) */
	profile->starting_health_modifier = 1.f;
	profile->starting_shield_modifier = 1.f;
	cache_reference(&profile->primary_weapon.weapon, weapon_handle);
}

/* ---------- the AI (tools/scorecard's scenarios) */

/* (the scenario's placements of units keep these to themselves: their layout,
which the game's tag schema fixes) */
struct biped_placement
{
	struct scenario_object_datum object;
	unsigned char unit[0x78 - sizeof(struct scenario_object_datum)];
};
typedef char biped_placement_size_check[sizeof(struct biped_placement) == 0x78 ? 1 : -1];
typedef char hs_reference_size_check[sizeof(struct hs_reference) == 0x28 ? 1 : -1];
typedef char ai_command_list_size_check[sizeof(struct ai_command_list_definition) == 0x60 ? 1 : -1];
typedef char ai_command_size_check[sizeof(struct ai_command_definition) == 0x20 ? 1 : -1];

enum
{
	/* (path_structure_bsp.c's: a surface the AI's paths may cross) */
	PATHFINDING_SURFACE_WALKABLE = 0x40,
};

void prop_ai_walker(
	struct stage *stage,
	char const *name,
	char const *variant_name,
	real_point3d const *position)
{
	struct actor_definition *actor;
	struct actor_variant_definition *variant;
	struct hs_reference *reference;
	struct scenario_object_palette_entry *palette;
	struct biped_placement *placement;
	struct collision_bsp *collision = stage->bsp->collision_bsp.address;
	unsigned char *walkable;
	long actor_handle, variant_handle;
	void *data;

	/* the actor: of a type the AI has (one under NUMBER_OF_ACTOR_TYPES, not a
	swarm's), the rest zero: it sees and hears nothing, and looks idly at
	nothing (actor_looking.c) */
	actor_handle = cache_tag_new(FOURCC('a', 'c', 't', 'r'), NONE, NONE, "stagekit\\walker",
		sizeof(struct actor_definition), &data);
	actor = data;
	actor->type = _actor_marine;
	/* its variant, which the console names by its tag's whole name, found among
	the scenario's script references (hs_compile.c); no weapon or equipment */
	variant_handle = cache_tag_new(FOURCC('a', 'c', 't', 'v'), NONE, NONE, variant_name,
		sizeof(struct actor_variant_definition), &data);
	variant = data;
	cache_reference(&variant->actor_reference, actor_handle);
	cache_reference(&variant->unit_reference, stage->biped_handle);
	cache_no_reference(&variant->major_upgrade_reference, FOURCC('a', 'c', 't', 'v'));
	cache_no_reference(&variant->ranged_combat.reference, FOURCC('w', 'e', 'a', 'p'));
	cache_no_reference(&variant->items.equipment_reference, FOURCC('e', 'q', 'i', 'p'));
	reference = cache_block_add(&stage->scenario->hs_references, sizeof(*reference));
	cache_reference(&reference->reference, variant_handle);

	/* the biped it drives */
	palette = cache_block_add(&stage->scenario->biped_palette, sizeof(*palette));
	cache_reference(&palette->reference, stage->biped_handle);
	placement = cache_block_add(&stage->scenario->bipeds, sizeof(*placement));
	placement->object.palette_entry_index = (short)(stage->scenario->biped_palette.count - 1);
	placement->object.name_index = object_name_new(stage, name, _object_type_biped,
		(short)(stage->scenario->bipeds.count - 1));
	placement->object.position = *position;

	/* the chamber's floor walkable for the AI's paths, its walls and ceiling
	not (one byte a collision surface: the floor is the box's first, collision.c) */
	walkable = cache_bsp_block(&stage->bsp->pathfinding_surfaces, collision->surfaces.count, 1);
	walkable[0] = PATHFINDING_SURFACE_WALKABLE;
}

void prop_ai_command_list(
	struct stage *stage,
	char const *name,
	struct ai_walker_move const *moves,
	short move_count)
{
	struct ai_command_list_definition *list = cache_block_add(&stage->scenario->ai_command_lists, sizeof(*list));
	short index;

	if (strlen(name) >= sizeof(list->name))
	{
		fprintf(stderr, "stagekit: the command list name %s is too long\n", name);
		exit(1);
	}
	/* each move a direction (action_obey.c's move_direction, facing forward),
	walked until it has gone its distance along it; of any structure BSP */
	strcpy(list->name, name);
	list->manual_structure_bsp_reference_index = NONE;
	list->runtime_structure_bsp_reference_index = NONE;
	for (index = 0; index < move_count; index++)
	{
		struct ai_command_definition *command = cache_block_add(&list->commands, sizeof(*command));

		command->atom_type = _ai_atom_move_direction;
		command->atom_modifier = 0;
		command->parameter1 = moves[index].distance;
		command->parameter2 = moves[index].heading;
		command->point1_index = command->point2_index = NONE;
		command->animation_reference_index = command->script_reference_index = NONE;
		command->recording_reference_index = command->command_index = NONE;
		command->object_name_index = NONE;
	}
}

/* ---------- vehicles (tools/scorecard's vehicle scenarios) */

enum
{
	/* (vehicles.c's names a jeep 1: four wheels it drives on and steers with) */
	VEHICLE_TYPE_JEEP = 1,
	/* (physics.c's: a powered mass point pushes its wheels along at the
	vehicle's speed, the jeep's, on the ground) */
	POWERED_MASS_POINT_GROUND_FRICTION = FLAG(0),
	/* (physics.c's friction types: none, along a mass point's forward) */
	FRICTION_TYPE_POINT = 0,
	FRICTION_TYPE_FORWARD = 1,
	CAR_MASS_POINTS = 8,
};

/* (vehicles.c and physics.c keep these to themselves: their layouts, which
their size checks fix; the names are what vehicles.c reads them for) */
struct vehicle_definition
{
	struct unit_definition unit;
	unsigned long flags;
	short vehicle_type;
	short pad;
	/* world units a tick, forward and back, and their changes a tick */
	real maximum_forward_speed;
	real maximum_reverse_speed;
	real speed_acceleration;
	real speed_deceleration;
	/* degrees, left and right, and degrees a second */
	real maximum_left_turn;
	real maximum_right_turn;
	real wheel_circumference;
	real turn_rate;
	real blur_speed;
	short function_modes[4];
	unsigned char unused0[0xc];
	real maximum_left_slide;
	real maximum_right_slide;
	unsigned char unused1[8];
	real minimum_flip_roll;
	real maximum_flip_roll;
	unsigned char unused2[0x1c];
	real fixed_gun_pitch;
	unsigned char unused3[0x48];
	struct tag_reference suspension_sound;
	struct tag_reference crash_sound;
	struct tag_reference material_effects;
	struct tag_reference effect;
};
struct powered_mass_point_definition
{
	char name[32];
	unsigned long flags;
	real antigrav_strength;
	real antigrav_offset;
	real antigrav_height;
	real antigrav_damp_fraction;
	real antigrav_normal_k1;
	real antigrav_normal_k0;
	real unused[17];
};
struct vehicle_placement
{
	struct scenario_object_datum object;
	struct scenario_object_permutation permutation;
	real body_vitality;
	unsigned long flags;
	unsigned char unused[0x28];
};
typedef char vehicle_definition_size_check[sizeof(struct vehicle_definition) == 0x3F0 ? 1 : -1];
typedef char powered_mass_point_definition_size_check[sizeof(struct powered_mass_point_definition) == 0x80 ? 1 : -1];
typedef char vehicle_placement_size_check[sizeof(struct vehicle_placement) == 0x78 ? 1 : -1];

/* a 3 by 3 symmetric matrix's inverse */
static void matrix3x3_symmetric_inverse(
	real_matrix3x3 const *m,
	real_matrix3x3 *inverse)
{
	real a = m->n[0][0], b = m->n[0][1], c = m->n[0][2];
	real d = m->n[1][1], e = m->n[1][2], f = m->n[2][2];
	real ad = d*f - e*e, ab = c*e - b*f, ac = b*e - c*d;
	real determinant = a*ad + b*ab + c*ac;

	inverse->n[0][0] = ad / determinant;
	inverse->n[0][1] = inverse->n[1][0] = ab / determinant;
	inverse->n[0][2] = inverse->n[2][0] = ac / determinant;
	inverse->n[1][1] = (a*f - c*c) / determinant;
	inverse->n[1][2] = inverse->n[2][1] = (b*c - a*e) / determinant;
	inverse->n[2][2] = (a*d - b*b) / determinant;
}

/* the car's physics: its mass points, the four wheels (the front two steered
by the first powered mass point, the back two by the second, the jeep's, which
both drive) and the body's four corners above them; its centre of mass where
they balance, and its inertia from them (each a ball of its own radius) */
static long car_physics(
	real_point3d *center_of_mass)
{
	/* (StageKit's own round numbers: wheels 1.5 apart and 0.9 across, a
	third round, the body's corners above and a little outside them; a car
	of 4000, its wheels 400 each) */
	static struct
	{
		char const *name;
		short powered;
		real x, y, z, radius, mass;
	} const points[CAR_MASS_POINTS] =
	{
		{ "wheel front left", 0, 0.75f, 0.45f, 0.3f, 0.3f, 400.f },
		{ "wheel front right", 0, 0.75f, -0.45f, 0.3f, 0.3f, 400.f },
		{ "wheel back left", 1, -0.75f, 0.45f, 0.3f, 0.3f, 400.f },
		{ "wheel back right", 1, -0.75f, -0.45f, 0.3f, 0.3f, 400.f },
		{ "body front left", NONE, 0.8f, 0.4f, 0.7f, 0.35f, 600.f },
		{ "body front right", NONE, 0.8f, -0.4f, 0.7f, 0.35f, 600.f },
		{ "body back left", NONE, -0.8f, 0.4f, 0.7f, 0.35f, 600.f },
		{ "body back right", NONE, -0.8f, -0.4f, 0.7f, 0.35f, 600.f },
	};
	struct physics_definition *physics;
	struct mass_point_definition *mass_points;
	struct powered_mass_point_definition *powered;
	real_matrix3x3 *inertia;
	real_point3d center = { { 0.f, 0.f, 0.f } };
	real mass = 0.f;
	long handle;
	short index;
	void *data;

	handle = cache_tag_new(FOURCC('p', 'h', 'y', 's'), NONE, NONE, "stagekit\\car", sizeof(struct physics_definition),
		&data);
	physics = data;
	for (index = 0; index < CAR_MASS_POINTS; index++)
	{
		mass += points[index].mass;
		center.x += points[index].x * points[index].mass;
		center.y += points[index].y * points[index].mass;
		center.z += points[index].z * points[index].mass;
	}
	center.x /= mass;
	center.y /= mass;
	center.z /= mass;
	physics->radius = -1.f;
	physics->moment = 1.f;
	physics->mass = mass;
	physics->center_of_mass = center;
	*center_of_mass = center;
	physics->density = 1.f;
	physics->gravity_scale = 1.f;
	/* (StageKit's own, a share of a velocity a tick: its wheels' grip, a
	quarter; the ground's give, a fifth of a unit, and its damping) */
	physics->ground_friction = stage_parameter("car_grip", 0.25f);
	physics->ground_depth = 0.2f;
	physics->ground_damp_fraction = stage_parameter("car_damping", 0.06f);
	/* (ground of 30 to 45 degrees drives it less; cos 45, cos 60) */
	physics->ground_normal_k1 = square_root(0.5f);
	physics->ground_normal_k0 = 0.5f;
	physics->water_density = 1.f;
	physics->air_friction = 0.01f;

	mass_points = cache_block(&physics->mass_points, CAR_MASS_POINTS, sizeof(*mass_points));
	inertia = cache_block(&physics->inertial_matrix, 2, sizeof(*inertia));
	for (index = 0; index < CAR_MASS_POINTS; index++)
	{
		struct mass_point_definition *point = &mass_points[index];
		real x = points[index].x - center.x, y = points[index].y - center.y, z = points[index].z - center.z;
		real m = points[index].mass;
		real ball = 0.4f * m * points[index].radius * points[index].radius;

		strcpy(point->name, points[index].name);
		point->powered_mass_point_index = points[index].powered;
		point->model_node_index = 0;
		point->relative_mass = m / 400.f;
		point->mass = m;
		point->relative_density = 1.f;
		point->density = 1.f;
		point->position.x = points[index].x;
		point->position.y = points[index].y;
		point->position.z = points[index].z;
		point->forward.i = 1.f;
		point->up.k = 1.f;
		point->radius = points[index].radius;
		/* (a wheel grips as hard along as across; the body, as anything) */
		point->friction_type = points[index].powered == NONE ? FRICTION_TYPE_POINT : FRICTION_TYPE_FORWARD;
		point->friction_parallel_scale = 1.f;
		point->friction_perpendicular_scale = 1.f;
		/* (the inertia about the centre of mass: each point's, and its ball's) */
		inertia[0].n[0][0] += m * (y*y + z*z) + ball;
		inertia[0].n[1][1] += m * (x*x + z*z) + ball;
		inertia[0].n[2][2] += m * (x*x + y*y) + ball;
		inertia[0].n[0][1] -= m * x * y;
		inertia[0].n[0][2] -= m * x * z;
		inertia[0].n[1][2] -= m * y * z;
	}
	inertia[0].n[1][0] = inertia[0].n[0][1];
	inertia[0].n[2][0] = inertia[0].n[0][2];
	inertia[0].n[2][1] = inertia[0].n[1][2];
	matrix3x3_symmetric_inverse(&inertia[0], &inertia[1]);
	physics->xx_moment = inertia[0].n[0][0];
	physics->yy_moment = inertia[0].n[1][1];
	physics->zz_moment = inertia[0].n[2][2];

	powered = cache_block(&physics->powered_mass_points, 2, sizeof(*powered));
	strcpy(powered[0].name, "front");
	powered[0].flags = POWERED_MASS_POINT_GROUND_FRICTION;
	strcpy(powered[1].name, "back");
	powered[1].flags = POWERED_MASS_POINT_GROUND_FRICTION;
	return handle;
}

long prop_car(
	void)
{
	static long handle = NONE;
	struct vehicle_definition *car;
	struct animation_graph *animations;
	struct animation_graph_unit_seat *animation_seat;
	struct animation_graph_weapon_class *weapon_class;
	struct animation_graph_weapon_type *weapon_type;
	struct unit_seat *seat;
	/* (StageKit's own: from just ahead of its wheels to just behind, a little
	wider, from their axles to above the body) */
	real_rectangle3d body = { { -1.f, 1.f, -0.6f, 0.6f, 0.2f, 1.f } };
	real_point3d center_of_mass;
	long animations_handle;
	void *data;

	if (handle != NONE)
		return handle;
	/* (a unit's animations as the players' biped's: one seat, unarmed, which
	units.c reads of any unit; and a vehicle's, with nothing in them) */
	animations_handle = cache_tag_new(FOURCC('a', 'n', 't', 'r'), NONE, NONE, "stagekit\\car",
		sizeof(struct animation_graph), &data);
	animations = data;
	animation_seat = cache_block(&animations->unit_seats, 1, sizeof(*animation_seat));
	strcpy(animation_seat->label, "stand");
	weapon_class = cache_block(&animation_seat->weapon_classes, 1, sizeof(*weapon_class));
	strcpy(weapon_class->label, "unarmed");
	weapon_type = cache_block(&weapon_class->weapon_types, 1, sizeof(*weapon_type));
	strcpy(weapon_type->label, "unarmed");
	cache_block(&animations->vehicle_animations, 1, 0x74);

	handle = cache_tag_new(FOURCC('v', 'e', 'h', 'i'), FOURCC('u', 'n', 'i', 't'), FOURCC('o', 'b', 'j', 'e'),
		"stagekit\\car", sizeof(struct vehicle_definition), &data);
	car = data;
	car->unit.object.type = _object_type_vehicle;
	cache_reference(&car->unit.object.model, prop_empty_model());
	cache_reference(&car->unit.object.animation_graph, animations_handle);
	cache_reference(&car->unit.object.physics, car_physics(&center_of_mass));
	/* (a box round its body, in its own space, whose origin is its centre of
	mass: what bullets and bipeds strike, and what puts it where vehicles
	find each other, objects.c's collideable objects) */
	body.x0 -= center_of_mass.x;
	body.x1 -= center_of_mass.x;
	body.z0 -= center_of_mass.z;
	body.z1 -= center_of_mass.z;
	cache_reference(&car->unit.object.collision_model, box_collision_model("stagekit\\car", &body));
	/* (round the body, a little past its farthest point) */
	car->unit.object.bounding_radius = 1.5f;
	/* (the engine's own field of view, as the biped's) */
	car->unit.unit.camera_field_of_view = DEGREES_TO_RADIANS(70.f);
	/* (one seat, the driver's, whose animations the players' biped has:
	stage.c's one seat, "stand"; seen from the rider's own eyes, as a
	following camera reads a camera track StageKit has none of) */
	seat = cache_block(&car->unit.unit.seats, 1, sizeof(*seat));
	seat->flags = FLAG(_unit_seat_driver_bit);
	strcpy(seat->label, "stand");
	/* (vehicles.c's flags: a speed or a turn wakes it; a throttle against its
	speed brakes) */
	car->flags = FLAG(0) | FLAG(1) | FLAG(4);
	car->vehicle_type = VEHICLE_TYPE_JEEP;
	/* (StageKit's own: 7.5 world units a second forward, 3 back, a second and
	two thirds to its top speed; its wheels turn 30 degrees each way, in a
	quarter second) */
	car->maximum_forward_speed = stage_parameter("car_speed", 7.5f) / TICKS_PER_SECOND;
	car->maximum_reverse_speed = 3.f / TICKS_PER_SECOND;
	car->speed_acceleration = car->maximum_forward_speed / (stage_parameter("car_seconds", 1.6667f) * TICKS_PER_SECOND);
	car->speed_deceleration = 4.f * car->speed_acceleration;
	car->maximum_left_turn = 30.f;
	car->maximum_right_turn = -30.f;
	car->turn_rate = 120.f;
	car->wheel_circumference = 2.f;
	/* (no blur: never this fast) */
	car->blur_speed = 1000.f;
	return handle;
}

void prop_vehicle(
	struct stage *stage,
	char const *name,
	long vehicle_handle,
	real_point3d const *position,
	real yaw)
{
	struct scenario_object_palette_entry *palette;
	struct vehicle_placement *placement;

	palette = cache_block_add(&stage->scenario->vehicle_palette, sizeof(*palette));
	cache_reference(&palette->reference, vehicle_handle);
	placement = cache_block_add(&stage->scenario->vehicles, sizeof(*placement));
	placement->object.palette_entry_index = (short)(stage->scenario->vehicle_palette.count - 1);
	placement->object.name_index = object_name_new(stage, name, _object_type_vehicle,
		(short)(stage->scenario->vehicles.count - 1));
	placement->object.position = *position;
	placement->object.rotation.yaw = DEGREES_TO_RADIANS(yaw);
}
