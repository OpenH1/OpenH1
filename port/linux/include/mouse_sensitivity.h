/*
MOUSE_SENSITIVITY.H

The mouse's aim in Counter-Strike 2's units (input.mouse_sensitivity,
input.mouse_vertical_sensitivity, input.zoom_sensitivity), the arithmetic
alone, which the platform layer's mouse (port/linux/src/xinput_sdl.c), the
game's zoom (port/linux/game/view_fov.c) and the tests share.

CS2 turns the view sensitivity times m_yaw (m_pitch upward) degrees for
each count the mouse reports, 0.022 by default: linear, with no
acceleration, whatever the field of view, the resolution or the frame rate.
So does the mouse here: the counts are SDL's relative motion, which on
every desktop build is the mouse's own (raw input, XInput2's raw events,
Wayland's unaccelerated motion; on macOS GCMouse's counts in place of the
system's accelerated pointer, port/macos/host_raw_mouse.m), and each count
is turned at once, not as a rate.

Zoomed, CS2 scales the turn by zoom_sensitivity_ratio (1 by default) times
the zoomed field of view over the unzoomed one; the game scales it by one
over the zoom's magnification. Controllers, the touch controls and VR keep
the game's in every case: this is the mouse's alone.
*/

#ifndef MOUSE_SENSITIVITY_H
#define MOUSE_SENSITIVITY_H

/* CS2's m_yaw and m_pitch: degrees a count at sensitivity 1 */
#define MOUSE_DEGREES_PER_COUNT 0.022
/* CS2's zoom_sensitivity_ratio */
#define MOUSE_ZOOM_SENSITIVITY_RATIO 1.0
/* the default, as near as two decimals come to the older versions' 0.0022
radians a count: 0.0022 * 180 / pi / 0.022 = 5.7296 */
#define MOUSE_SENSITIVITY_DEFAULT 5.73
/* an older version's input.mouse_sensitivity times this is CS2's
(port_config.c's config_migrate_mouse) */
#define MOUSE_SENSITIVITY_FROM_OLD (0.0022 * 180.0 / 3.14159265358979323846 / MOUSE_DEGREES_PER_COUNT)

/* input.zoom_sensitivity */
enum
{
	_mouse_zoom_cs2,
	_mouse_zoom_halo
};

/* radians the view turns for one count at the sensitivity */
static __inline float mouse_radians_per_count(float sensitivity)
{
	return (float)((double)sensitivity * MOUSE_DEGREES_PER_COUNT * (3.14159265358979323846 / 180.0));
}

/* what the zoom multiplies the mouse's turn by: the game's one over the
magnification, or CS2's ratio of the fields of view (the same convention
for both, any; a field of view that is not one keeps the game's) */
static __inline float mouse_zoom_scale(int mode, float magnification, float zoomed_field_of_view,
	float unzoomed_field_of_view)
{
	if (!(magnification > 0.0f))
		return 1.0f;
	if (mode == _mouse_zoom_cs2 && zoomed_field_of_view > 0.0f && unzoomed_field_of_view > 0.0f &&
		zoomed_field_of_view <= unzoomed_field_of_view)
	{
		return (float)(MOUSE_ZOOM_SENSITIVITY_RATIO * zoomed_field_of_view / unzoomed_field_of_view);
	}
	return 1.0f / magnification;
}

#endif
