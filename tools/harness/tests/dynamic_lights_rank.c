/*
DYNAMIC_LIGHTS_RANK.C

The scene's lights put in the order of what they show first, past the
rasterizer's 128 (port/linux/game/dynamic_lights.c dynamic_lights_rank); the
lights are made-up handles, described by a table.
*/

#include "harness.h"
#include <math.h>

#define MAXIMUM_LIGHTS_PER_WINDOW 128
#define MAXIMUM_RENDERED_LIGHTS 512
#define MAXIMUM_LIGHTS_PER_MAP 4096
#define MAX(a, b) ((a) > (b) ? (a) : (b))
typedef struct { real red, green, blue; } real_rgb_color;
#define _real_epsilon 0.0001f
#define square_root(x) sqrtf(x)

static struct { struct { real_point3d position; real_vector3d forward; } camera; } render;

#include "under_test.inc"

static struct { real_point3d position; real radius, intensity; } table[MAXIMUM_RENDERED_LIGHTS + 1];

static void describe(long light, real_point3d *position, real *radius, real *intensity)
{
	CHECK(light >= 1000 && light < 1000 + MAXIMUM_RENDERED_LIGHTS + 1, "a handle of the list: %ld", light);
	*position = table[light - 1000].position;
	*radius = table[light - 1000].radius;
	*intensity = table[light - 1000].intensity;
}

/* count lights in a row ahead of the camera (looking along +x), each the
same: far, small and dim, handles 1000 on */
static void fill(long *lights, short count)
{
	short i;

	memset(&render, 0, sizeof(render));
	render.camera.forward.i = 2.0f;
	for (i = 0; i < count; i++)
	{
		lights[i] = 1000 + i;
		table[i].position = (real_point3d){ 50.0f, (real)i * 0.01f, 0.0f };
		table[i].radius = 1.0f;
		table[i].intensity = 0.25f;
	}
}

static short position_of(long const *lights, short count, long light)
{
	short i;

	for (i = 0; i < count; i++)
	{
		if (lights[i] == light)
			return i;
	}
	return -1;
}

int main(int argc, char **argv)
{
	static long lights[MAXIMUM_RENDERED_LIGHTS + 1];
	char const *case_name;
	short i;

	CHECK(argc == 2, "one named case");
	case_name = argv[1];
	if (!strcmp(case_name, "untouched-below"))
	{
		fill(lights, MAXIMUM_LIGHTS_PER_WINDOW);
		table[127].position.x = 1.0f;
		dynamic_lights_rank(lights, MAXIMUM_LIGHTS_PER_WINDOW, describe);
		for (i = 0; i < MAXIMUM_LIGHTS_PER_WINDOW; i++)
			CHECK(lights[i] == 1000 + i, "128 or fewer keep the game's order: %d is %ld", i, lights[i]);
		return 0;
	}
	if (!strcmp(case_name, "near-and-wide-first"))
	{
		fill(lights, 300);
		/* a near small light, a far wide one, a bright far one: all ahead
		of the 128th, the rest after */
		table[250].position.x = 2.0f;
		table[260].radius = 40.0f;
		table[270].intensity = 1.0f;
		table[270].position.x = 20.0f;
		dynamic_lights_rank(lights, 300, describe);
		CHECK(position_of(lights, 300, 1250) < 3 && position_of(lights, 300, 1260) < 3 &&
			position_of(lights, 300, 1270) < 3, "the near, the wide and the bright first: %d %d %d",
			position_of(lights, 300, 1250), position_of(lights, 300, 1260), position_of(lights, 300, 1270));
		CHECK(position_of(lights, 300, 1260) < position_of(lights, 300, 1250),
			"a far light reaching the camera before a near small one");
		for (i = 0; i < 300; i++)
			CHECK(position_of(lights, 300, 1000 + i) >= 0, "each light once: %d", i);
		return 0;
	}
	if (!strcmp(case_name, "behind-last"))
	{
		fill(lights, 200);
		/* the first 150 near but behind the camera's plane; the rest ahead */
		for (i = 0; i < 150; i++)
			table[i].position.x = -5.0f;
		dynamic_lights_rank(lights, 200, describe);
		for (i = 0; i < 50; i++)
			CHECK(lights[i] >= 1150, "the lights ahead before those behind the camera: %d is %ld", i, lights[i]);
		/* one reaching across the plane is not behind it */
		fill(lights, 200);
		for (i = 0; i < 150; i++)
			table[i].position.x = -5.0f;
		table[0].radius = 5.5f;
		dynamic_lights_rank(lights, 200, describe);
		CHECK(lights[0] == 1000, "a light behind the camera reaching past its plane ranks as any");
		return 0;
	}
	if (!strcmp(case_name, "stable-ties"))
	{
		fill(lights, 200);
		for (i = 0; i < 200; i++)
			table[i].position.y = 0.0f;
		dynamic_lights_rank(lights, 200, describe);
		for (i = 0; i < 200; i++)
			CHECK(lights[i] == 1000 + i, "equal lights keep the game's order: %d is %ld", i, lights[i]);
		return 0;
	}
	if (!strcmp(case_name, "unknown-bright"))
	{
		fill(lights, 200);
		table[199].intensity = 0.0f;
		dynamic_lights_rank(lights, 200, describe);
		CHECK(lights[0] == 1199, "a light not yet drawn counts as bright: first is %ld", lights[0]);
		return 0;
	}
	if (!strcmp(case_name, "camera-inside"))
	{
		fill(lights, 200);
		table[150].position.x = 0.5f;
		table[150].radius = 3.0f;
		table[160].position.x = 0.0f;
		table[160].radius = 0.0f;
		dynamic_lights_rank(lights, 200, describe);
		CHECK(lights[0] == 1150, "the light the camera is in first: %ld", lights[0]);
		CHECK(position_of(lights, 200, 1160) >= 0, "a light of no radius at the camera is ranked, not lost");
		return 0;
	}
	if (!strcmp(case_name, "over-capacity-untouched"))
	{
		fill(lights, MAXIMUM_RENDERED_LIGHTS + 1);
		table[MAXIMUM_RENDERED_LIGHTS].position.x = 1.0f;
		dynamic_lights_rank(lights, MAXIMUM_RENDERED_LIGHTS + 1, describe);
		CHECK(lights[0] == 1000, "more than the list holds is left alone");
		return 0;
	}
	if (!strcmp(case_name, "drawn-wins"))
	{
		/* light 1150 is dim and far at the tick, but drawn near and bright
		last frame (between ticks): ranked as drawn */
		real_point3d near = { 1.0f, 0.0f, 0.0f };
		real_rgb_color bright = { 0.0f, 1.0f, 0.0f };

		fill(lights, 200);
		dynamic_lights_drawn(1150, &near, 2.0f, &bright);
		dynamic_lights_rank(lights, 200, describe);
		CHECK(lights[0] == 1150, "the light as drawn first: %ld", lights[0]);
		return 0;
	}
	if (!strcmp(case_name, "drawn-faded-low"))
	{
		/* drawn black (faded out between ticks): not ranked as unknown */
		real_point3d near = { 1.0f, 0.0f, 0.0f };
		real_rgb_color black = { 0.0f, 0.0f, 0.0f };

		fill(lights, 200);
		dynamic_lights_drawn(1150, &near, 2.0f, &black);
		dynamic_lights_rank(lights, 200, describe);
		CHECK(position_of(lights, 200, 1150) == 199, "a light drawn faded out last: %d", position_of(lights, 200, 1150));
		return 0;
	}
	if (!strcmp(case_name, "drawn-reused-slot"))
	{
		/* another light (another handle) in the same slot was drawn: this
		one ranks as the game has it */
		real_point3d near = { 1.0f, 0.0f, 0.0f };
		real_rgb_color bright = { 1.0f, 1.0f, 1.0f };

		fill(lights, 200);
		dynamic_lights_drawn(1150 + 0x10000, &near, 2.0f, &bright);
		dynamic_lights_rank(lights, 200, describe);
		CHECK(lights[0] == 1000, "a slot's other light is not this one: first is %ld", lights[0]);
		return 0;
	}
	CHECK(FALSE, "unknown case %s", case_name);
	return 1;
}
