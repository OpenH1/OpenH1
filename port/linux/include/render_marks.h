/*
RENDER_MARKS.H

debug.render_marks (port/linux/game/render_marks.c): the renderer's command
stream told what the draws that follow belong to (port/linux/src/
xgpu_stream.h's MARK records), for the inspector's replay
(port/macos/tools/halo_replay.m --serve): the camera of each window drawn,
and the owner of each run of draws (an object, the structure's shader, an
effect, the HUD, a menu widget, text). The backends draw nothing for them;
with the setting off nothing is sent and each call returns at once.

The kinds are xgpu_stream.h's XGPU_STREAM_MARK_*, in the same order
(render_marks.c checks).
*/

#ifndef RENDER_MARKS_H
#define RENDER_MARKS_H

enum
{
	RENDER_MARK_OTHER,
	RENDER_MARK_WORLD_OBJECT,
	RENDER_MARK_FIRST_PERSON,
	RENDER_MARK_BSP,
	RENDER_MARK_SKY,
	RENDER_MARK_DECAL,
	RENDER_MARK_EFFECT,
	RENDER_MARK_PARTICLE,
	RENDER_MARK_CONTRAIL,
	RENDER_MARK_LIGHTNING,
	RENDER_MARK_WEATHER,
	RENDER_MARK_WATER,
	RENDER_MARK_HUD,
	RENDER_MARK_TEXT,
	RENDER_MARK_MENU_WIDGET,
	RENDER_MARK_CINEMATIC_LETTERBOX,
	RENDER_MARK_DEBUG,
	RENDER_MARK_DETAIL_OBJECT,
	RENDER_MARK_LENS_FLARE,
	RENDER_MARK_FOG,
	RENDER_MARK_SCREEN_EFFECT,
	RENDER_MARK_LIGHT_VOLUME,
	RENDER_MARK_SHADOW,
	RENDER_MARK_CLEAR,
	NUMBER_OF_RENDER_MARK_KINDS
};

/* nonzero while the setting is on (read once a frame) */
extern int render_marks_on;

#endif
