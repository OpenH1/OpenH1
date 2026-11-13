/*
GRUNT_BIRTHDAY_PARTY.C

The Grunt Birthday Party skull's (skulls.c): a grunt killed by a shot to its
head bursts its head into confetti, with the grunts' cheer, as Halo: Combat
Evolved Anniversary's does.

No map has confetti, and the port ships no art: the confetti is made of the
game's own pieces as a campaign map's tags load, and added to its table as
the menus' tags are (menu_tags.c), before them:
- a particle ('part') drawn with a small bitmap the campaign maps all have,
  the snow's white flecks (effects\particles\solid\bitmaps\snow impact: ten
  sprites, each a fleck of its own shape, one taken at random), lit by
  itself (so its colors stay bright in the dark), falling and drifting as a
  leaf does (the map's point physics, effects\point physics\leaf), for a
  second or two, then fading;
- an effect ('effe') with one event that bursts three sets of it out of the
  head, upwards and outwards, spinning, each set tinted at random between
  two bright colors (red to yellow, green to cyan, blue to pink).
They are checked as a map's tags are (tag_validate_added_tags), and not
made where the map lacks what they need (another bitmap or physics of the
map's is taken first: gravel, metal chips; wood, stones), nor where the
validator refuses them: the cheer then plays alone. The cheer is the grunts'
own (sound\dialog\grunt\...\celebration), where the map has it (every
campaign map with grunts).

The tags are made only where the skull may play in the map's game: a
campaign game of this machine's with it chosen, a demo recorded with it,
and a network co-op client, whose host's skulls come after its map has
loaded (network_skulls.c). Their indices follow the map's own tags', before
the menus', so a saved game's effects find them again in a game that has
them; one resumed without them has its effects and particles of tags it
lacks let go (effects.c, particles.c: game_state.c's after-load procs).

A grunt is a unit that one of the map's grunt actor variants places: the
same on every machine, whose clients have no actors.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "ai/actor_definitions.h"
#include "ai/actor_types.h"
#include "bitmaps/bitmap_group.h"
#include "cache/cache_files.h"
#include "effects/effect_definitions.h"
#include "effects/effects.h"
#include "effects/particles.h"
#include "game/game.h"
#include "objects/objects.h"
#include "scenario/scenario_definitions.h"
#include "sound/game_sound.h"
#include "tag_files/tag_groups.h"
#include "demo.h"
#include "grunt_birthday_party.h"
#include "skulls.h"
#include "tag_schema.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* cache_files.c's (port) */
void *cache_files_tag_instances(long *count);
void cache_files_set_tag_instances(void *instances, long count);
boolean tag_index_is_group(long tag_index, long group_tag);
char *tag_get_name(long tag_index);

/* ---------- constants */

enum
{
	GROUP_BITMAP = 'bitm',
	GROUP_POINT_PHYSICS = 'pphy',
	GROUP_SOUND = 'snd!',
	GROUP_SCENARIO = 'scnr',
	GROUP_ACTOR = 'actr',
	GROUP_ACTOR_VARIANT = 'actv',

	/* the units the map's grunts are (a map's grunt variants share a few) */
	MAXIMUM_GRUNT_UNITS = 32,
	/* the confetti's colors, a set of particles each */
	CONFETTI_COLORS = 3,
	/* the tags made: the particle and the effect */
	CONFETTI_TAGS = 2,

	/* particles.c's particle definition flags */
	_confetti_particle_starts_on_random_frame_bit = 2,
	_confetti_particle_dies_in_water_bit = 7,
	_confetti_particle_self_illuminated_bit = 9,
	/* effects.c's: a particle's orientation at random */
	_confetti_effect_particle_random_orientation_bit = 1,
	/* the shaders' type of an effect's (rasterizer's _shader_type_effect) */
	CONFETTI_SHADER_TYPE_EFFECT = 1,
};

#define CONFETTI_NAME "skulls\\grunt birthday party\\confetti"
#define CONFETTI_BURST_NAME "skulls\\grunt birthday party\\confetti burst"
#define CONFETTI_LOCATION "head"
#define GRUNT_CHEER "sound\\dialog\\grunt\\conditional\\combat2\\postcombatactions\\celebration"
#define TAG_NAME_SIZE 256

/* the bitmaps and point physics the confetti may be made with, the first
the map has */
static char const *const confetti_bitmaps[] =
{
	"effects\\particles\\solid\\bitmaps\\snow impact",
	"effects\\particles\\solid\\bitmaps\\gravel",
	"effects\\particles\\solid\\bitmaps\\metal chips",
};
static char const *const confetti_physics[] =
{
	"effects\\point physics\\leaf",
	"effects\\point physics\\wood",
	"effects\\point physics\\stones",
};

/* ---------- structures */

/* effects.c's (and tag_schema_effects.c's) */
struct confetti_effect_location
{
	char marker_name[32];
};

struct confetti_effect_particles
{
	short environment;
	short disposition;
	short camera_mode;
	short unused006;
	short location_index;
	short unused00a;
	real_euler_angles2d direction;
	real_vector3d offset;
	real_vector3d runtime_direction;
	long unused02c[10];
	struct tag_reference particle;
	unsigned long flags;
	short distribution_function;
	short unused06a;
	short count_lower_bound;
	short count_upper_bound;
	real distribution_radius_lower_bound;
	real distribution_radius_upper_bound;
	long unused078[3];
	real velocity_lower_bound;
	real velocity_upper_bound;
	real velocity_cone_angle;
	real angular_velocity_lower_bound;
	real angular_velocity_upper_bound;
	long unused098[2];
	real radius_lower_bound;
	real radius_upper_bound;
	long unused0a8[2];
	real_argb_color tint_lower_bound;
	real_argb_color tint_upper_bound;
	long unused0d0[4];
	unsigned long scale_a_flags;
	unsigned long scale_b_flags;
};

typedef char verify_confetti_effect_location_size[sizeof(struct confetti_effect_location) == 0x20 ? 1 : -1];
typedef char verify_confetti_effect_particles_size[sizeof(struct confetti_effect_particles) == 0xE8 ? 1 : -1];
typedef char verify_confetti_effect_particles_count_offset[
	offsetof(struct confetti_effect_particles, count_lower_bound) == 0x6C ? 1 : -1];
typedef char verify_confetti_effect_particles_tint_offset[
	offsetof(struct confetti_effect_particles, tint_lower_bound) == 0xB0 ? 1 : -1];

/* the confetti's tags, all in one block (the validator's region): their
roots, blocks and names */
struct confetti_tags
{
	byte validator_header[TAG_VALIDATE_ADDED_TAGS_HEADER_SIZE];
	struct particle_definition particle;
	struct effect_definition effect;
	struct confetti_effect_location location;
	struct effect_event_definition event;
	struct confetti_effect_particles particles[CONFETTI_COLORS];
	char particle_name[sizeof(CONFETTI_NAME)];
	char effect_name[sizeof(CONFETTI_BURST_NAME)];
	char bitmap_name[TAG_NAME_SIZE];
	char physics_name[TAG_NAME_SIZE];
	char no_name[1];
};

typedef char verify_confetti_tags_fit[
	sizeof(struct confetti_tags) <= TAG_VALIDATE_ADDED_TAGS_MAXIMUM_SIZE ? 1 : -1];

/* ---------- globals */

static struct
{
	/* the map's table, and the one with the confetti's tags after it */
	struct cache_file_tag_instance *original_instances;
	long original_count;
	struct cache_file_tag_instance *instances;
	struct confetti_tags *tags;
	long particle_index;
	long effect_index;
	long cheer_index;
	long grunt_units[MAXIMUM_GRUNT_UNITS];
	short grunt_unit_count;
} grunt_birthday_party = { NULL, 0, NULL, NULL, NONE, NONE, NONE, { 0 }, 0 };

/* the confetti's colors: each particle's tint, at random between two (red,
green and blue: red to yellow, green to sky blue, blue to pink) */
static real const confetti_tints[CONFETTI_COLORS][2][3] =
{
	{ { 1.0f, 0.12f, 0.12f }, { 1.0f, 0.9f, 0.05f } },
	{ { 0.1f, 1.0f, 0.2f }, { 0.05f, 0.75f, 1.0f } },
	{ { 0.3f, 0.35f, 1.0f }, { 1.0f, 0.25f, 0.8f } },
};

/* ---------- private code */

/* (the map's own table back, where it is still loaded) */
static void confetti_release(
	boolean restore)
{
	if (grunt_birthday_party.instances && restore)
		cache_files_set_tag_instances(grunt_birthday_party.original_instances, grunt_birthday_party.original_count);
	/* (the game's free takes no NULL) */
	if (grunt_birthday_party.instances)
		free(grunt_birthday_party.instances);
	if (grunt_birthday_party.tags)
		free(grunt_birthday_party.tags);
	grunt_birthday_party.instances = NULL;
	grunt_birthday_party.tags = NULL;
	grunt_birthday_party.original_instances = NULL;
	grunt_birthday_party.original_count = 0;
	grunt_birthday_party.particle_index = NONE;
	grunt_birthday_party.effect_index = NONE;
}

/* the first of names the map has a tag of group_tag of */
static long first_loaded(
	long group_tag,
	char const *const *names,
	short count)
{
	short index;

	for (index = 0; index < count; index++)
	{
		long tag_index = tag_loaded(group_tag, names[index]);

		if (tag_index != NONE)
			return tag_index;
	}
	return NONE;
}

/* whether the map's game may play the skull: its scenario a campaign's,
and the skull chosen here, recorded in the demo played, or this machine a
client, whose host's skulls come later */
static boolean grunt_birthday_party_may_play(
	void)
{
	struct cache_file_tag_instance *instances;
	long count, index;
	unsigned long recorded;
	struct scenario const *scenario = NULL;

	instances = cache_files_tag_instances(&count);
	for (index = 0; instances && index < count && !scenario; index++)
	{
		if (instances[index].group_tag == GROUP_SCENARIO && instances[index].base_address)
			scenario = instances[index].base_address;
	}
	if (!scenario || scenario->type != _scenario_type_solo)
		return FALSE;
	if (demo_playback_skulls(&recorded))
		return TEST_FLAG(recorded, _skull_grunt_birthday_party);
	if (game_connection() == _game_connection_network_client)
		return TRUE;
	return TEST_FLAG(skulls_chosen(), _skull_grunt_birthday_party);
}

/* the units the map's grunt actor variants place */
static void grunt_units_find(
	void)
{
	struct tag_iterator iterator;
	long variant_index;

	grunt_birthday_party.grunt_unit_count = 0;
	tag_iterator_new(&iterator, GROUP_ACTOR_VARIANT);
	while ((variant_index = tag_iterator_next(&iterator)) != NONE)
	{
		struct actor_variant_definition const *variant = actor_variant_definition_get(variant_index);
		long unit_index = variant->unit_reference.index;
		short index;

		if (!tag_index_is_group(variant->actor_reference.index, GROUP_ACTOR) ||
			actor_definition_get(variant->actor_reference.index)->type != _actor_grunt ||
			!tag_index_is_group(unit_index, OBJECT_DEFINITION_TAG))
		{
			continue;
		}
		for (index = 0; index < grunt_birthday_party.grunt_unit_count &&
			grunt_birthday_party.grunt_units[index] != unit_index; index++)
		{
		}
		if (index == grunt_birthday_party.grunt_unit_count && index < MAXIMUM_GRUNT_UNITS)
			grunt_birthday_party.grunt_units[grunt_birthday_party.grunt_unit_count++] = unit_index;
	}
}

/* the inverse of the largest of a bitmap's sprites in its first sequence,
in pixels (what the tools give a particle: render_particles.c draws a
particle's diameter as that many of its sprite's pixels); 0 for one with
none */
static real confetti_inverse_sprite_size(
	long bitmap_index)
{
	struct bitmap_group const *bitmap = bitmap_group_get(bitmap_index);
	struct bitmap_group_sequence const *sequence;
	real largest = 0.0f;
	long sprite_index;

	if (!bitmap || bitmap->sequences.count <= 0 || !bitmap->sequences.address)
		return 0.0f;
	sequence = TAG_BLOCK_GET_ELEMENT(&bitmap->sequences, 0, struct bitmap_group_sequence);
	for (sprite_index = 0; sprite_index < sequence->sprites.count && sequence->sprites.address; sprite_index++)
	{
		struct bitmap_group_sprite const *sprite =
			TAG_BLOCK_GET_ELEMENT(&sequence->sprites, sprite_index, struct bitmap_group_sprite);
		struct bitmap_data const *data;
		real width, height;

		if (sprite->bitmap_index < 0 || sprite->bitmap_index >= bitmap->bitmaps.count || !bitmap->bitmaps.address)
			continue;
		data = TAG_BLOCK_GET_ELEMENT(&bitmap->bitmaps, sprite->bitmap_index, struct bitmap_data);
		width = (real)floor((sprite->bounds.x1 - sprite->bounds.x0) * data->width + 0.5f);
		height = (real)floor((sprite->bounds.y1 - sprite->bounds.y0) * data->height + 0.5f);
		largest = MAX(largest, MAX(width, height));
	}
	return largest >= 1.0f ? 1.0f / largest : 0.0f;
}

static void confetti_reference(
	struct tag_reference *reference,
	long group_tag,
	char *name,
	long tag_index)
{
	reference->group_tag = group_tag;
	reference->name = name;
	reference->name_length = (long)strlen(name);
	reference->index = tag_index;
}

/* the particle: the flecks of bitmap_index, moved by physics_index */
static void confetti_particle_make(
	struct confetti_tags *tags,
	long bitmap_index,
	long physics_index,
	real inverse_sprite_size)
{
	struct particle_definition *particle = &tags->particle;

	particle->flags = FLAG(_confetti_particle_starts_on_random_frame_bit) |
		FLAG(_confetti_particle_dies_in_water_bit) | FLAG(_confetti_particle_self_illuminated_bit);
	snprintf(tags->bitmap_name, sizeof(tags->bitmap_name), "%s", tag_get_name(bitmap_index));
	snprintf(tags->physics_name, sizeof(tags->physics_name), "%s", tag_get_name(physics_index));
	confetti_reference(&particle->bitmap, GROUP_BITMAP, tags->bitmap_name, bitmap_index);
	confetti_reference(&particle->physics, GROUP_POINT_PHYSICS, tags->physics_name, physics_index);
	confetti_reference(&particle->collision_material_effects, 'foot', tags->no_name, NONE);
	confetti_reference(&particle->collision_effect, EFFECT_DEFINITION_TAG, tags->no_name, NONE);
	confetti_reference(&particle->effect, EFFECT_DEFINITION_TAG, tags->no_name, NONE);
	particle->life_span_lower_bound = 1.4f;
	particle->life_span_upper_bound = 2.4f;
	particle->fade_in_time = 0.0f;
	particle->fade_out_time = 0.6f;
	particle->minimum_pixels = 0.0f;
	particle->radius_lower_bound = 1.0f;
	particle->radius_upper_bound = 1.0f;
	/* (one fleck the whole of its life: no frames run) */
	particle->frames_per_second_lower_bound = 0.0f;
	particle->frames_per_second_upper_bound = 0.0f;
	particle->frames_per_second_contact_deterioration = 0.0f;
	particle->lod_falloff = 5.0f;
	particle->lod_cutoff = 4.0f;
	particle->first_sequence_index = 0;
	particle->initial_sequence_count = 1;
	particle->looping_sequence_count = 0;
	particle->final_sequence_count = 0;
	particle->runtime_oo_width = inverse_sprite_size;
	particle->sprite_orientation = 0;
	/* (alpha blended, its color the tint's) */
	particle->shader.shader.base.type = CONFETTI_SHADER_TYPE_EFFECT;
	particle->shader.framebuffer_blend_function = 0;
	particle->shader.framebuffer_fade_mode = 0;
	confetti_reference(&particle->shader.secondary_map, GROUP_BITMAP, tags->no_name, NONE);
}

/* the effect: one event, at the head, of the particle in every color */
static void confetti_effect_make(
	struct confetti_tags *tags,
	long particle_index)
{
	struct effect_definition *effect = &tags->effect;
	short color;

	effect->flags = 0;
	effect->loop_start_index = NONE;
	effect->loop_stop_index = NONE;
	snprintf(tags->location.marker_name, sizeof(tags->location.marker_name), "%s", CONFETTI_LOCATION);
	effect->locations.count = 1;
	effect->locations.address = &tags->location;
	effect->events.count = 1;
	effect->events.address = &tags->event;
	tags->event.particles.count = CONFETTI_COLORS;
	tags->event.particles.address = tags->particles;
	for (color = 0; color < CONFETTI_COLORS; color++)
	{
		struct confetti_effect_particles *particles = &tags->particles[color];

		particles->location_index = 0;
		/* (along the location's forward, up: the tools' direction of no
		angles) */
		particles->runtime_direction.i = 1.0f;
		confetti_reference(&particles->particle, PARTICLE_TAG, tags->particle_name, particle_index);
		particles->flags = FLAG(_confetti_effect_particle_random_orientation_bit);
		particles->count_lower_bound = 18;
		particles->count_upper_bound = 24;
		particles->distribution_radius_lower_bound = 0.02f;
		particles->distribution_radius_upper_bound = 0.05f;
		particles->velocity_lower_bound = 0.8f;
		particles->velocity_upper_bound = 2.0f;
		particles->velocity_cone_angle = 1.1f;
		particles->angular_velocity_lower_bound = -3.0f * _pi;
		particles->angular_velocity_upper_bound = 3.0f * _pi;
		particles->radius_lower_bound = 0.025f;
		particles->radius_upper_bound = 0.04f;
		particles->tint_lower_bound.alpha = 1.0f;
		particles->tint_lower_bound.red = confetti_tints[color][0][0];
		particles->tint_lower_bound.green = confetti_tints[color][0][1];
		particles->tint_lower_bound.blue = confetti_tints[color][0][2];
		particles->tint_upper_bound.alpha = 1.0f;
		particles->tint_upper_bound.red = confetti_tints[color][1][0];
		particles->tint_upper_bound.green = confetti_tints[color][1][1];
		particles->tint_upper_bound.blue = confetti_tints[color][1][2];
	}
}

/* the confetti's tags made and added to the map's table: FALSE (and the
table as it was) where the map lacks what they need, or they are refused */
static boolean confetti_make(
	void)
{
	struct cache_file_tag_instance *instances;
	struct confetti_tags *tags;
	long count, index, salt = 0;
	long bitmap_index = first_loaded(GROUP_BITMAP, confetti_bitmaps, (short)NUMBEROF(confetti_bitmaps));
	long physics_index = first_loaded(GROUP_POINT_PHYSICS, confetti_physics, (short)NUMBEROF(confetti_physics));
	real inverse_sprite_size;

	if (bitmap_index == NONE || physics_index == NONE)
	{
		error(_error_silent, "grunt birthday party: the map has none of the confetti's %s",
			bitmap_index == NONE ? "bitmaps" : "point physics");
		return FALSE;
	}
	inverse_sprite_size = confetti_inverse_sprite_size(bitmap_index);
	instances = cache_files_tag_instances(&count);
	if (inverse_sprite_size <= 0.0f || !instances || count < 0 || count > UNSIGNED_SHORT_MAX - CONFETTI_TAGS)
		return FALSE;
	tags = calloc(1, sizeof(*tags));
	grunt_birthday_party.instances = malloc((count + CONFETTI_TAGS) * sizeof(*instances));
	grunt_birthday_party.tags = tags;
	if (!tags || !grunt_birthday_party.instances)
	{
		confetti_release(FALSE);
		return FALSE;
	}
	memcpy(grunt_birthday_party.instances, instances, count * sizeof(*instances));
	grunt_birthday_party.original_instances = instances;
	grunt_birthday_party.original_count = count;
	for (index = 0; index < count; index++)
	{
		long instance_salt = (unsigned long)instances[index].tag_index >> 16;

		if (instance_salt > salt)
			salt = instance_salt;
	}
	grunt_birthday_party.particle_index = ((salt + 1) << 16) | count;
	grunt_birthday_party.effect_index = ((salt + 2) << 16) | (count + 1);
	strcpy(tags->particle_name, CONFETTI_NAME);
	strcpy(tags->effect_name, CONFETTI_BURST_NAME);
	confetti_particle_make(tags, bitmap_index, physics_index, inverse_sprite_size);
	confetti_effect_make(tags, grunt_birthday_party.particle_index);
	for (index = 0; index < CONFETTI_TAGS; index++)
	{
		struct cache_file_tag_instance *instance = &grunt_birthday_party.instances[count + index];

		memset(instance, 0, sizeof(*instance));
		instance->group_tag = index ? EFFECT_DEFINITION_TAG : PARTICLE_TAG;
		instance->parent_group_tags[0] = NONE;
		instance->parent_group_tags[1] = NONE;
		instance->tag_index = index ? grunt_birthday_party.effect_index : grunt_birthday_party.particle_index;
		instance->name = index ? tags->effect_name : tags->particle_name;
		instance->base_address = index ? (void *)&tags->effect : (void *)&tags->particle;
	}
	cache_files_set_tag_instances(grunt_birthday_party.instances, count + CONFETTI_TAGS);
	if (!tag_validate_added_tags(grunt_birthday_party.instances, count + CONFETTI_TAGS, count, tags, sizeof(*tags),
		cache_file_loaded_map_name()))
	{
		error(_error_silent, "grunt birthday party: the confetti's tags are refused; the grunts only cheer");
		confetti_release(TRUE);
		return FALSE;
	}
	error(_error_silent, "grunt birthday party: confetti of %s (%ld corrections), tags %08lx and %08lx",
		tags->bitmap_name, tag_validate_added_tags_corrections(), (unsigned long)grunt_birthday_party.particle_index,
		(unsigned long)grunt_birthday_party.effect_index);
	return TRUE;
}

/* ---------- public code */

void grunt_birthday_party_tags_loaded(
	void)
{
	/* (none of another map's left: its table went with its tags) */
	confetti_release(FALSE);
	grunt_birthday_party.cheer_index = NONE;
	grunt_birthday_party.grunt_unit_count = 0;
	if (!grunt_birthday_party_may_play())
		return;
	grunt_units_find();
	if (!grunt_birthday_party.grunt_unit_count)
		return;
	grunt_birthday_party.cheer_index = tag_loaded(GROUP_SOUND, GRUNT_CHEER);
	confetti_make();
}

void grunt_birthday_party_tags_unloaded(
	void)
{
	confetti_release(TRUE);
	grunt_birthday_party.cheer_index = NONE;
	grunt_birthday_party.grunt_unit_count = 0;
}

boolean grunt_birthday_party_grunt(
	long unit_index)
{
	struct object_datum const *object = object_try_and_get(unit_index);
	short index;

	if (!object)
		return FALSE;
	for (index = 0; index < grunt_birthday_party.grunt_unit_count; index++)
	{
		if (grunt_birthday_party.grunt_units[index] == object->definition_index)
			return TRUE;
	}
	return FALSE;
}

boolean grunt_birthday_party_burst(
	long unit_index)
{
	struct object_marker marker;
	real_point3d head;
	long effect_index = NONE;
	long sound_index = NONE;
	struct object_datum const *object = object_try_and_get(unit_index);

	if (!object)
		return FALSE;
	/* (its head's marker, or where the unit is) */
	if (object_get_marker_by_name(unit_index, CONFETTI_LOCATION, &marker, 1) > 0)
		head = marker.matrix.position;
	else
		head = object->object.position;
	if (grunt_birthday_party.effect_index != NONE &&
		tag_index_is_group(grunt_birthday_party.effect_index, EFFECT_DEFINITION_TAG))
	{
		char const *names[1] = { CONFETTI_LOCATION };

		effect_index = effect_new_unattached_from_markers(grunt_birthday_party.effect_index, NONE, NULL, 1, names,
			&head, global_up3d, 1.0f, 1.0f, NULL, NULL, TRUE);
	}
	if (grunt_birthday_party.cheer_index != NONE && tag_index_is_group(grunt_birthday_party.cheer_index, GROUP_SOUND))
	{
		sound_index = object_impulse_sound_new(unit_index, grunt_birthday_party.cheer_index, NONE, global_origin3d,
			global_forward3d, 1.0f);
	}
	error(_error_silent, "grunt birthday party: %s's head at %.2f %.2f %.2f: confetti %08lx, cheer %08lx",
		tag_name_strip_path(tag_get_name(object->definition_index)), head.x, head.y, head.z,
		(unsigned long)effect_index, (unsigned long)sound_index);
	return effect_index != NONE || sound_index != NONE;
}

long grunt_birthday_party_effect(
	void)
{
	return grunt_birthday_party.effect_index;
}

long grunt_birthday_party_particle(
	void)
{
	return grunt_birthday_party.particle_index;
}
