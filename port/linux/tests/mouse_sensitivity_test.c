/*
MOUSE_SENSITIVITY_TEST.C

The mouse's turn in Counter-Strike 2's units (port/linux/include/
mouse_sensitivity.h), checked in numbers: N counts at sensitivity S turn
N * 0.022 * S degrees, side to side and (times the vertical multiplier) up
and down, the same in one event as in many (no acceleration); zoomed, CS2's
scale is the zoomed field of view over the unzoomed one and the game's one
over the magnification, at the game's own zooms (its 70 degree camera, the
pistol's 2x, the sniper rifle's 2x and 8x, drawn as view_fov.c draws them:
the camera's tangent times 0.85, display.fov at the stock view); and a tick
with both a stick and the mouse turns each by its own scaling.
tools/test_mouse_sensitivity.py builds and runs it.
*/

#include <math.h>
#include <stdio.h>

#include "mouse_sensitivity.h"

static int failures;

#define CHECK(condition, ...) \
	do { if (!(condition)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); \
		failures++; } } while (0)

static double degrees(double radians)
{
	return radians * 180.0 / 3.14159265358979323846;
}

/* float's own error for a turn of this many degrees, a few times over */
static double tolerance(double expected)
{
	return fabs(expected) * 4e-7 + 1e-9;
}

/* the horizontal angle across 4:3 the view is drawn at for a unit camera's
angle (render_camera_get_adjusted_field_of_view_tangent's 0.85, then as
view_fov.c's fov_horizontal_4_3 has it) */
static double drawn(double camera)
{
	double vertical = 2.0 * atan(0.75 * tan(camera * 0.5) * 0.85);

	return 2.0 * atan(tan(vertical * 0.5) / 0.75);
}

static void hip_fire(void)
{
	static const float sensitivities[] = { 0.5f, 1.0f, 1.7f, 2.5f, 5.73f, 10.0f };
	static const int counts[] = { 1, 7, 100, 1000, 16384 };
	int s, n;

	for (s = 0; s < (int)(sizeof(sensitivities) / sizeof(sensitivities[0])); s++)
	{
		float radians = mouse_radians_per_count(sensitivities[s]);

		for (n = 0; n < (int)(sizeof(counts) / sizeof(counts[0])); n++)
		{
			double expected = counts[n] * 0.022 * sensitivities[s];
			double yaw = degrees((float)counts[n] * radians);
			/* (vertical: the multiplier 1, and 0.5) */
			double pitch = degrees((float)counts[n] * radians * 1.0f);
			double half = degrees((float)counts[n] * radians * 0.5f);

			CHECK(fabs(yaw - expected) <= tolerance(expected), "%d counts at %g: %.9f degrees, not %.9f", counts[n],
				sensitivities[s], yaw, expected);
			CHECK(fabs(pitch - expected) <= tolerance(expected), "pitch %.9f, not %.9f", pitch, expected);
			CHECK(fabs(half - expected * 0.5) <= tolerance(expected), "half pitch %.9f", half);
		}
	}
	/* CS2's 1.7: a count is 0.0374 degrees; 360 degrees take 9625.67 counts
	(at 800 counts an inch, 30.57 cm), as in CS2 */
	CHECK(fabs(degrees(mouse_radians_per_count(1.7f)) - 0.0374) < 1e-8, "1.7: %.10f degrees a count",
		degrees(mouse_radians_per_count(1.7f)));
	/* the default turns as the older versions' 0.0022 radians a count did */
	CHECK(fabs(mouse_radians_per_count((float)MOUSE_SENSITIVITY_DEFAULT) - 0.0022) < 0.0022 * 1e-4,
		"the default: %.9f radians a count", mouse_radians_per_count((float)MOUSE_SENSITIVITY_DEFAULT));
	CHECK(fabs(MOUSE_SENSITIVITY_FROM_OLD - 5.7295779513) < 1e-9, "from the old scale: %.10f",
		MOUSE_SENSITIVITY_FROM_OLD);
	/* no acceleration: a thousand events of one count turn as one of a
	thousand */
	{
		float radians = mouse_radians_per_count(1.7f), many = 0.0f;
		int event;

		for (event = 0; event < 1000; event++)
			many += 1.0f * radians;
		CHECK(fabs(degrees(many) - degrees(1000.0f * radians)) <= tolerance(37.4) * 10.0, "1000 x 1: %.9f, 1 x 1000: "
			"%.9f", degrees(many), degrees(1000.0f * radians));
	}
}

static void zoomed(void)
{
	/* the game's camera, and its zooms' magnifications */
	static const double magnifications[] = { 2.0, 8.0, 1.5, 4.0 };
	double camera = 70.0 * 3.14159265358979323846 / 180.0;
	float sensitivity = 1.7f;
	int index;

	for (index = 0; index < (int)(sizeof(magnifications) / sizeof(magnifications[0])); index++)
	{
		double magnification = magnifications[index];
		/* (weapon_get_field_of_view: the camera's angle over the magnification) */
		double unzoomed = drawn(camera), zoomed_angle = drawn(camera / magnification);
		float cs2 = mouse_zoom_scale(_mouse_zoom_cs2, (float)magnification, (float)zoomed_angle, (float)unzoomed);
		float halo = mouse_zoom_scale(_mouse_zoom_halo, (float)magnification, (float)zoomed_angle, (float)unzoomed);
		double hip = 0.022 * sensitivity;
		double cs2_degrees = degrees(100.0f * mouse_radians_per_count(sensitivity) * cs2) / 100.0;
		double halo_degrees = degrees(100.0f * mouse_radians_per_count(sensitivity) * halo) / 100.0;
		double expected_cs2 = hip * MOUSE_ZOOM_SENSITIVITY_RATIO * zoomed_angle / unzoomed;

		CHECK(fabs(cs2_degrees - expected_cs2) <= tolerance(expected_cs2), "%gx, cs2: %.9f degrees a count, not %.9f",
			magnification, cs2_degrees, expected_cs2);
		CHECK(fabs(halo_degrees - hip / magnification) <= tolerance(hip), "%gx, halo: %.9f degrees a count, not %.9f",
			magnification, halo_degrees, hip / magnification);
		printf("%gx: hip %.5f, cs2 %.6f (x%.4f), halo %.6f (x%.4f) degrees a count\n", magnification, hip,
			cs2_degrees, cs2, halo_degrees, halo);
	}
	/* not zoomed, or a field of view that is not one: the game's */
	CHECK(mouse_zoom_scale(_mouse_zoom_cs2, 2.0f, 0.0f, 1.0f) == 0.5f, "no zoomed angle");
	CHECK(mouse_zoom_scale(_mouse_zoom_cs2, 0.0f, 0.5f, 1.0f) == 1.0f, "no magnification");
	CHECK(mouse_zoom_scale(_mouse_zoom_cs2, 2.0f, 1.0f, 1.0f) == 1.0f, "a zoom the game drew no narrower");
}

/* a tick's turn as player_control.c makes it: the stick's look scaled by the
game's one over the magnification (player_control_scale_look), the mouse's
then added, scaled by input.zoom_sensitivity's */
static double tick_yaw(float stick, float mouse_counts, int mode, double magnification, float sensitivity)
{
	double unzoomed = drawn(70.0 * 3.14159265358979323846 / 180.0);
	double zoomed_angle = drawn(70.0 * 3.14159265358979323846 / 180.0 / magnification);
	float stick_yaw = stick * (1.0f / (float)magnification);
	float mouse_yaw = mouse_counts * mouse_radians_per_count(sensitivity) *
		mouse_zoom_scale(mode, (float)magnification, (float)zoomed_angle, (float)unzoomed);

	return (double)stick_yaw + mouse_yaw;
}

static void both_devices(void)
{
	float stick = 0.05f;
	double magnification = 8.0;
	double unzoomed = drawn(70.0 * 3.14159265358979323846 / 180.0);
	double zoomed_angle = drawn(70.0 * 3.14159265358979323846 / 180.0 / magnification);
	double stick_alone = tick_yaw(stick, 0.0f, _mouse_zoom_cs2, magnification, 1.7f);
	double mouse_alone = tick_yaw(0.0f, 50.0f, _mouse_zoom_cs2, magnification, 1.7f);
	double together = tick_yaw(stick, 50.0f, _mouse_zoom_cs2, magnification, 1.7f);

	/* the stick keeps the game's scaling whatever the mouse's setting */
	CHECK(fabs(stick_alone - stick / magnification) < 1e-9, "the stick: %.9f", stick_alone);
	CHECK(fabs(tick_yaw(stick, 0.0f, _mouse_zoom_halo, magnification, 1.7f) - stick_alone) < 1e-12,
		"the stick changed with input.zoom_sensitivity");
	/* the mouse's own, CS2's */
	CHECK(fabs(degrees(mouse_alone) - 50.0 * 0.022 * 1.7 * zoomed_angle / unzoomed) < 1e-5, "the mouse: %.9f",
		degrees(mouse_alone));
	/* in one tick together, each its own */
	CHECK(fabs(together - (stick_alone + mouse_alone)) < 1e-9, "together: %.9f, not %.9f", together,
		stick_alone + mouse_alone);
}

int main(void)
{
	hip_fire();
	zoomed();
	both_devices();
	if (failures)
		return 1;
	printf("PASS\n");
	return 0;
}
