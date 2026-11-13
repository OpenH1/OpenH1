/*
SPARTAN_LINK.C

The HUD's Spartan link (display.spartan_link): live feeds of the living
teammates' views in the HUD's top line, between the ammunition's panel at
the top left and the shield's at the top right. A key shows and hides it
(controls.spartan_link, B; on a controller in the stock layouts, D-pad
Down), off at the start of every game. Co-op (network and split screen)
and team games only, teammates only; a free-for-all game, and a Pro 2V2
game, have none, and the key says so on the HUD.

Each feed is the teammate's first-person view as this machine has the
world: their eye (where their body is drawn, render_interpolation.c), their
aim, their weapon's zoom, their weapon and arms (first_person_weapons.c's
posed draw: the idle animation, not their reloads), their body hidden, and
none of their HUD. A teammate in a seat whose camera follows the vehicle
is seen from behind. Under each: their name, their shield and their
distance. A teammate who dies is shown down for a moment, then dropped;
with none left alive the link closes. While this player is dead the link
is hidden (spectating fills the screen: dead_spectate.c) and comes back
with the respawn. Split screen: each window has its own link, of its own
player's teammates.

The cost: one feed is drawn a frame (render.c's render_frame, before the
window's view), each kept in its own images between (d3d8_device.c's extra
views, halo_view_begin) at the size it is drawn on the screen, without the
anti-aliasing pass, multisampling, the world's ambient occlusion, the
objects' shadows, lens flares or the weather, and its models' levels of
detail chosen for that size. So the frame pays for about one small view,
however many feeds there are; with the link hidden, nothing.

Only what this machine draws: nothing is sent, the game state is not
touched (no seed is taken, no effect or sound made), and the network
version is unchanged.
*/

#include "cseries.h"
#include "camera/director.h"
#include "camera/observer.h"
#include "camera/first_person_camera.h"
#include "camera/static_camera.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/hud.h"
#include "interface/hud_messaging.h"
#include "interface/ui_widget.h"
#include "items/weapons.h"
#include "main/console.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "render/render_cameras.h"
#include "scenario/scenario.h"
#include "game/game_globals.h"
#include "bitmaps/bitmaps.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmap_color_conversion.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "units/units.h"
#include "units/unit_definitions.h"

#include "halo_keyboard.h"
#include "controller_remap.h"
#include "far_clip.h"
#include "network_coop.h"
#include "network_standard.h"
#include "spartan_link.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------- prototypes */

/* port/linux/src/port_config.c */
char const *config_string(char const *name);
int config_boolean(char const *name);
unsigned long config_changes(void);
void platform_log(char const *format, ...);
unsigned long system_milliseconds(void);

/* port/linux/src/d3d8_device.c: the extra views */
void halo_view_begin(int view, unsigned long width, unsigned long height);
void halo_view_end(void);
int halo_view_drawn(int view);
void halo_view_sample(int view);
void halo_view_release(int view);
float halo_screen_pixel_scale(void);
long halo_screen_width(void);

/* port/linux/src/d3d8_device.c: the next frame saved as
debug.screenshot_directory's <name>.bmp */
void halo_screenshot_named(const char *name);

/* source/objects/object_lights.c */
void lights_port_keep(boolean keep);

/* port/linux/game/render_interpolation.c */
void render_interpolation_unit_camera_position(long unit_index, real_point3d *position);
real render_interpolation_fraction(void);

/* port/linux/src/xinput_sdl.c */
int halo_spartan_link_pad_pressed(short port);

/* port/linux/game/network_chat.c */
boolean network_chat_typing(void);

/* ---------- constants */

#define SPARTAN_LINK_FEEDS 4
/* (the extra views' numbers: 1 + a local player's 4) */
#define SPARTAN_LINK_VIEW(local_player_index, feed) (1 + (local_player_index) * SPARTAN_LINK_FEEDS + (feed))

/* how long a teammate who died is shown down before their feed drops out,
and a refusal's line shown (milliseconds) */
#define SPARTAN_LINK_DOWN_MILLISECONDS 1500
#define SPARTAN_LINK_REFUSAL_MILLISECONDS 2000

/* the layout, in the screen's units of a window 480 tall (the HUD's): each
feed's width for one to four feeds, the gap between them, the top (from the
view's top edge, not the title-safe frame's), and the row under each (name,
shield, distance). At 1600x900 these are the design's 300, 260, 228 and 210
pixels */
static real const spartan_link_widths[SPARTAN_LINK_FEEDS] = { 160.0f, 138.7f, 121.6f, 112.0f };
#define SPARTAN_LINK_GAP 6.4f
#define SPARTAN_LINK_TOP 11.0f
#define SPARTAN_LINK_ROW 11.0f
/* what the HUD's panels take of each side of the top line, which the feeds
stay clear of (the stock HUD's and the high-res HUD's, at the HUD's scale) */
#define SPARTAN_LINK_PANEL_LEFT 180.0f
#define SPARTAN_LINK_PANEL_RIGHT 198.0f
/* the frame's angled ends: how far in its top corners are, of its height */
#define SPARTAN_LINK_SLANT 0.12f

/* ---------- structures */

struct spartan_link_feed
{
	long player_index;
	/* the unit they were last seen alive in */
	long unit_index;
	/* when they were seen dead (0: alive) */
	unsigned long down_at;
	/* the view's size in the screen's units, as last drawn */
	unsigned long width, height;
};

struct spartan_link_window
{
	boolean open;
	short count;
	struct spartan_link_feed feeds[SPARTAN_LINK_FEEDS];
	/* a refusal's line (free for all, Pro 2V2), and since when */
	wchar_t const *refusal;
	unsigned long refusal_at;
};

static struct
{
	boolean in_game;
	long last_game_time;
	unsigned long read_at;
	boolean enabled;
	real size;
	boolean log;
	boolean debug_open;
	long logged_tick;
	struct spartan_link_window windows[MAXIMUM_LOCAL_PLAYERS];
	boolean key_held;
	/* the feed drawn this frame (local player, feed; NONE none), and the
	one drawn last, which the next follows round */
	short due_local_player, due_feed;
	short last_local_player, last_feed;
	/* the feed being drawn: whose eyes hide whose body, and whose weapon is
	posed (render_objects.c, first_person_weapons.c) */
	boolean drawing;
	long hidden_unit;
	long posed_unit;
	/* the views drawn in this game (to free them as it ends) */
	unsigned long views_used;
} spartan_link = { FALSE, 0, (unsigned long)-1, TRUE, 1.0f, FALSE };

/* ---------- private code */

/* a game in progress (and the game's time made: not before the first map) */
static boolean spartan_link_playing(
	void)
{
	return game_time_initialized() && game_in_progress();
}

static void spartan_link_read(
	void)
{
	char const *size;

	if (spartan_link.read_at == config_changes())
		return;
	spartan_link.read_at = config_changes();
	size = config_string("display.spartan_link");
	spartan_link.enabled = strcmp(size, "off") != 0;
	spartan_link.size = !strcmp(size, "small") ? 0.8f : !strcmp(size, "large") ? 1.2f : 1.0f;
	spartan_link.log = config_boolean("debug.spartan_link_log");
}

/* a game whose host allows no teammates' feeds: Pro 2V2's, Classic and
Crispy (network_standard_teammate_feeds_allowed) */
static boolean spartan_link_pro_doubles(
	void)
{
	return !network_standard_teammate_feeds_allowed();
}

/* whether this game has a link: co-op (a campaign, network or split
screen) or a team game, in play, and not Pro 2V2 */
static boolean spartan_link_game(
	void)
{
	if (!spartan_link.enabled || !spartan_link_playing() || cinematic_in_progress() ||
		game_connection() == _game_connection_film_playback)
	{
		return FALSE;
	}
	if (network_coop_active())
		return TRUE;
	return game_engine_running() ? game_engine_has_teams() && game_engine_in_play() &&
		!spartan_link_pro_doubles() : TRUE;
}

/* other is self's living teammate (co-op: anyone) */
static boolean spartan_link_teammate(
	long self,
	long other)
{
	struct player_datum *player = player_try_and_get(other);

	if (!player || other == self || player->unit_index == NONE ||
		!object_try_and_get_and_verify_type(player->unit_index, _object_mask_unit))
	{
		return FALSE;
	}
	if (TEST_FLAG(object_get(player->unit_index)->object.damage_flags, _object_dead_bit))
		return FALSE;
	return !game_engine_running() || player->team_index == player_get(self)->team_index;
}

/* the window's feeds brought up to date: the downed kept for a moment,
then dropped; the living added in the players' order, up to four */
static void spartan_link_window_update(
	short local_player_index,
	unsigned long now)
{
	struct spartan_link_window *window = &spartan_link.windows[local_player_index];
	long self = local_player_get_player_index(local_player_index);
	struct data_iterator iterator;
	struct player_datum *player;
	short index;

	if (self == NONE)
	{
		window->count = 0;
		window->open = FALSE;
		return;
	}
	for (index = 0; index < window->count; )
	{
		struct spartan_link_feed *feed = &window->feeds[index];
		struct player_datum *teammate = player_try_and_get(feed->player_index);

		if (spartan_link_teammate(self, feed->player_index) && teammate->unit_index == feed->unit_index)
		{
			feed->down_at = 0;
		}
		else if (!feed->down_at && teammate)
		{
			feed->down_at = now ? now : 1;
		}
		if (!teammate || (feed->down_at && now - feed->down_at >= SPARTAN_LINK_DOWN_MILLISECONDS))
		{
			short later;

			for (later = index; later + 1 < window->count; later++)
				window->feeds[later] = window->feeds[later + 1];
			window->count--;
			continue;
		}
		index++;
	}
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL &&
		window->count < SPARTAN_LINK_FEEDS)
	{
		boolean listed = FALSE;

		if (!spartan_link_teammate(self, iterator.datum_index))
			continue;
		for (index = 0; index < window->count; index++)
		{
			if (window->feeds[index].player_index == iterator.datum_index)
				listed = TRUE;
		}
		if (listed)
			continue;
		csmemset(&window->feeds[window->count], 0, sizeof(window->feeds[window->count]));
		window->feeds[window->count].player_index = iterator.datum_index;
		window->feeds[window->count].unit_index = player->unit_index;
		window->count++;
	}
	/* (nobody left alive: the link closes; debug.spartan_link_open's opens
	again as they come) */
	if (window->open && window->count == 0 && !spartan_link.debug_open)
		window->open = FALSE;
	if (spartan_link.debug_open && window->count > 0)
		window->open = TRUE;
}

/* the window's player is alive, and sees the link */
static boolean spartan_link_shown(
	short local_player_index)
{
	long self;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS ||
		!spartan_link.windows[local_player_index].open || !spartan_link_game())
	{
		return FALSE;
	}
	self = local_player_get_player_index(local_player_index);
	return self != NONE && player_get(self)->unit_index != NONE;
}

/* a press of the key for a local player: the link shown or hidden, or why
there is none */
static void spartan_link_toggle(
	short local_player_index,
	unsigned long now)
{
	struct spartan_link_window *window;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS || !spartan_link.enabled ||
		!spartan_link_playing() || cinematic_in_progress())
	{
		return;
	}
	window = &spartan_link.windows[local_player_index];
	window->refusal = NULL;
	if (!spartan_link_game())
	{
		window->refusal = spartan_link_pro_doubles() ? L"Spartan link is off in Pro 2v2" :
			L"Spartan link: no teammates in free for all";
		window->refusal_at = now;
		if (spartan_link.log)
		{
			platform_log("spartan link: local player %d: %ls", (int)local_player_index, window->refusal);
			halo_screenshot_named("spartan_link_refusal");
		}
		return;
	}
	spartan_link_window_update(local_player_index, now);
	if (!window->open && window->count == 0)
	{
		window->refusal = L"Spartan link: no teammates alive";
		window->refusal_at = now;
		return;
	}
	window->open = !window->open;
	if (spartan_link.log)
		platform_log("spartan link: local player %d %s, %d feeds", (int)local_player_index,
			window->open ? "open" : "closed", (int)window->count);
}

/* the feed drawn this frame: one a frame of every window's, a feed whose
image was never drawn first, else the next after the last drawn */
static void spartan_link_schedule(
	void)
{
	short local_player_index;
	short feed;
	short best_local = NONE, best_feed = NONE;
	long best_distance = LONG_MAX;
	long last = spartan_link.last_local_player == NONE ? -1 :
		spartan_link.last_local_player * SPARTAN_LINK_FEEDS + spartan_link.last_feed;

	spartan_link.due_local_player = NONE;
	spartan_link.due_feed = NONE;
	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		struct spartan_link_window *window = &spartan_link.windows[local_player_index];

		if (!spartan_link_shown(local_player_index))
			continue;
		for (feed = 0; feed < window->count; feed++)
		{
			long slot = local_player_index * SPARTAN_LINK_FEEDS + feed;
			long distance;

			/* (a downed teammate's image stays as it was) */
			if (window->feeds[feed].down_at)
				continue;
			distance = slot > last ? slot - last : slot - last + MAXIMUM_LOCAL_PLAYERS * SPARTAN_LINK_FEEDS;
			if (!halo_view_drawn(SPARTAN_LINK_VIEW(local_player_index, feed)))
				distance -= MAXIMUM_LOCAL_PLAYERS * SPARTAN_LINK_FEEDS;
			if (distance < best_distance)
			{
				best_distance = distance;
				best_local = local_player_index;
				best_feed = feed;
			}
		}
	}
	spartan_link.due_local_player = best_local;
	spartan_link.due_feed = best_feed;
}

/* the window's feeds' layout: each feed's rectangle in the view's units, from
its top left (left, top, right, bottom), and the scale of the units the
design's are in. The HUD's panels are placed in the window's title-safe frame
(its window bounds, 36 of 480 below the top of a whole screen's view), so the
feeds are sized and placed across between the panels in it; but they hang
from the view's own top edge, up in the panels' line, not under the frame's
inset */
static short spartan_link_layout(
	short local_player_index,
	struct render_camera const *camera,
	real_rectangle2d *rectangles,
	real *scale)
{
	struct spartan_link_window *window = &spartan_link.windows[local_player_index];
	/* (the title-safe frame in the view's units) */
	real frame_x0 = (real)(camera->window_bounds.x0 - camera->viewport_bounds.x0);
	real width = (real)(camera->window_bounds.x1 - camera->window_bounds.x0);
	real height = (real)(camera->window_bounds.y1 - camera->window_bounds.y0);
	real feed_width, total, room, left;
	short count = window->count;
	short index;

	if (count <= 0 || width <= 0.0f || height <= 0.0f)
		return 0;
	/* (a split screen's window keeps the HUD's size, and the feeds most of
	theirs) */
	*scale = PIN(height / 480.0f * 1.5f, 0.6f, 1.0f);
	feed_width = spartan_link_widths[count - 1] * spartan_link.size * *scale;
	total = feed_width * count + SPARTAN_LINK_GAP * *scale * (count - 1);
	room = width - (SPARTAN_LINK_PANEL_LEFT + SPARTAN_LINK_PANEL_RIGHT) * *scale;
	if (total > room && room > 0.0f)
	{
		real fit = room / total;

		feed_width *= fit;
		total = room;
	}
	/* (centred between the panels) */
	left = frame_x0 + SPARTAN_LINK_PANEL_LEFT * *scale + (room - total) * 0.5f;
	for (index = 0; index < count; index++)
	{
		rectangles[index].x0 = left + index * (feed_width + SPARTAN_LINK_GAP * *scale);
		rectangles[index].x1 = rectangles[index].x0 + feed_width;
		rectangles[index].y0 = SPARTAN_LINK_TOP * *scale;
		rectangles[index].y1 = rectangles[index].y0 + feed_width * 9.0f / 16.0f;
	}
	return count;
}

/* the field of view a unit looks through: its weapon's at its zoom (as
dead_spectate.c's) */
static real spartan_link_field_of_view(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct unit_definition *definition = unit_definition_get(unit->definition_index);
	long weapon_index = unit_inventory_get_weapon(unit_index, unit->unit.current_weapon_index);
	real field_of_view = definition->unit.camera_field_of_view;

	if (weapon_index != NONE)
		field_of_view = weapon_get_field_of_view(weapon_index, field_of_view, (short)unit->unit.current_zoom_level);
	if (!(field_of_view >= 0.001f && field_of_view <= _pi / 2.f))
		field_of_view = DEGREES_TO_RADIANS(70.f);
	return field_of_view;
}

/* ---------- the drawing's pieces */

/* the maps' white texture, NULL where the map has none (menu_shapes.c's) */
static struct bitmap_data *spartan_link_white(
	void)
{
	struct game_globals *game_globals = scenario_get_game_globals();
	struct game_globals_rasterizer_data *rasterizer_data;

	if (!game_globals || !game_globals->rasterizer_data.count)
		return NULL;
	rasterizer_data = TAG_BLOCK_GET_ELEMENT(&game_globals->rasterizer_data, 0, struct game_globals_rasterizer_data);
	return TAG_BLOCK_GET_ELEMENT(&bitmap_group_get(rasterizer_data->default_textures[0].index)->bitmaps, 1,
		struct bitmap_data);
}

/* a quadrilateral, its corners in the window's units (clockwise from the
top left), of the white texture or (u, v from 0 to 1 over bounds) the
feed's image */
static void spartan_link_quad(
	struct bitmap_data *white,
	real const x[4],
	real const y[4],
	pixel32 color,
	real_rectangle2d const *bounds)
{
	struct rasterizer_dynamic_screen_geometry_parameters parameters;
	struct dynamic_screen_vertex vertices[4];
	real pixels = halo_screen_pixel_scale() > 0.0f ? halo_screen_pixel_scale() : 1.0f;
	short index;

	for (index = 0; index < 4; index++)
	{
		/* (on the display's pixels' edges: where the screen's unit 0 falls
		is the Xbox's pixels' centres, menu_shapes.c) */
		vertices[index].position.x = floorf(x[index] * pixels + 0.5f) / pixels + 0.5f;
		vertices[index].position.y = floorf(y[index] * pixels + 0.5f) / pixels + 0.5f;
		vertices[index].texture_coordinates.x = bounds ? (x[index] - bounds->x0) / (bounds->x1 - bounds->x0) : 0.0f;
		vertices[index].texture_coordinates.y = bounds ? (y[index] - bounds->y0) / (bounds->y1 - bounds->y0) : 0.0f;
		vertices[index].color = color;
	}
	csmemset(&parameters, 0, sizeof(parameters));
	parameters.map[0] = white;
	parameters.map_texture_scale[0].i = parameters.map_texture_scale[0].j = 1.0f;
	parameters.map_scale[0].i = parameters.map_scale[0].j = 1.0f;
	rasterizer_globals.current_lock_operation = _rasterizer_lock_cinematics;
	rasterizer_psuedo_dynamic_screen_quad_draw(&parameters, vertices);
	rasterizer_globals.current_lock_operation = _rasterizer_lock_none;
}

static void spartan_link_rectangle(
	struct bitmap_data *white,
	real x0,
	real y0,
	real x1,
	real y1,
	pixel32 color)
{
	real x[4], y[4];

	if (x1 <= x0 || y1 <= y0)
		return;
	x[0] = x0; y[0] = y0;
	x[1] = x1; y[1] = y0;
	x[2] = x1; y[2] = y1;
	x[3] = x0; y[3] = y1;
	spartan_link_quad(white, x, y, color, NULL);
}

/* a colour of the HUD's text (its messages': hud_get_text_color), at alpha */
static pixel32 spartan_link_color(
	real alpha,
	real brightness)
{
	real_argb_color text_color;
	real_argb_color const *text = hud_get_text_color(&text_color);
	real_argb_color color;

	color.alpha = alpha;
	color.red = PIN(text->red * brightness, 0.0f, 1.0f);
	color.green = PIN(text->green * brightness, 0.0f, 1.0f);
	color.blue = PIN(text->blue * brightness, 0.0f, 1.0f);
	return real_argb_color_to_pixel32(&color);
}

/* a line of text in the HUD's font, its top at y in the window's units,
scaled: from x (justification 0), ending at x + width (1), or centred over
width (2) */
static void spartan_link_text(
	wchar_t const *text,
	real x,
	real y,
	real width,
	short justification,
	real text_scale,
	real_argb_color const *color)
{
	long font_index = hud_get_font_index();
	struct font_header *font;
	rectangle2d bounds, text_bounds, cursor_bounds;
	real height, text_width, left;

	if (font_index == NONE || !text[0])
		return;
	font = font_definition_get(font_index);
	height = (real)(font->ascending_height + font->descending_height);
	/* (measured unscaled, left-justified; then drawn from where it starts,
	scaled about that corner) */
	draw_string_set_draw_mode(font_index, NONE, 0, 0, color);
	bounds.x0 = 0;
	bounds.y0 = 0;
	bounds.x1 = 2000;
	bounds.y1 = (short)ceilf(height);
	draw_unicode_string_compute_bounds(&bounds, text, &text_bounds, &cursor_bounds);
	text_width = (real)(text_bounds.x1 - text_bounds.x0) * text_scale;
	left = justification == 1 ? x + width - text_width : justification == 2 ? x + (width - text_width) * 0.5f : x;
	bounds.x0 = (short)floorf(left);
	bounds.y0 = (short)floorf(y);
	bounds.x1 = (short)(bounds.x0 + 2000);
	bounds.y1 = (short)ceilf(y + height);
	rasterizer_text_set_scale(text_scale, (real)bounds.x0, (real)bounds.y0);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, text);
	rasterizer_text_set_scale(1.0f, 0.0f, 0.0f);
}

/* the frame about a feed: its image (or, never drawn, dark glass), a tint,
an edge of the HUD's colour, its top corners cut in */
static void spartan_link_draw_feed(
	struct bitmap_data *white,
	short local_player_index,
	short index,
	real_rectangle2d const *bounds,
	real scale)
{
	struct spartan_link_feed const *feed = &spartan_link.windows[local_player_index].feeds[index];
	int view = SPARTAN_LINK_VIEW(local_player_index, index);
	real slant = (bounds->y1 - bounds->y0) * SPARTAN_LINK_SLANT;
	real edge = MAX(1.0f / (halo_screen_pixel_scale() > 0.0f ? halo_screen_pixel_scale() : 1.0f), 0.75f * scale);
	real x[4], y[4];
	pixel32 edge_color = spartan_link_color(0.9f, 1.0f);

	/* the image, tinted toward the HUD's blue (a down teammate's darkened) */
	x[0] = bounds->x0 + slant; y[0] = bounds->y0;
	x[1] = bounds->x1 - slant; y[1] = bounds->y0;
	x[2] = bounds->x1; y[2] = bounds->y1;
	x[3] = bounds->x0; y[3] = bounds->y1;
	if (halo_view_drawn(view))
	{
		halo_view_sample(view);
		spartan_link_quad(white, x, y, feed->down_at ? 0xFF505868UL : 0xFFDCEBFFUL, bounds);
		halo_view_sample(0);
		spartan_link_quad(white, x, y, spartan_link_color(0.10f, 1.0f), NULL);
	}
	else
	{
		spartan_link_quad(white, x, y, 0xC0081018UL, NULL);
	}
	/* the edge: top and bottom, and the angled ends */
	spartan_link_rectangle(white, bounds->x0 + slant, bounds->y0, bounds->x1 - slant, bounds->y0 + edge, edge_color);
	spartan_link_rectangle(white, bounds->x0, bounds->y1 - edge, bounds->x1, bounds->y1, edge_color);
	x[0] = bounds->x0 + slant; y[0] = bounds->y0;
	x[1] = bounds->x0 + slant + edge; y[1] = bounds->y0;
	x[2] = bounds->x0 + edge; y[2] = bounds->y1;
	x[3] = bounds->x0; y[3] = bounds->y1;
	spartan_link_quad(white, x, y, edge_color, NULL);
	x[0] = bounds->x1 - slant - edge; y[0] = bounds->y0;
	x[1] = bounds->x1 - slant; y[1] = bounds->y0;
	x[2] = bounds->x1; y[2] = bounds->y1;
	x[3] = bounds->x1 - edge; y[3] = bounds->y1;
	spartan_link_quad(white, x, y, edge_color, NULL);
}

/* a line's width in the HUD's font, unscaled (spartan_link_text's units) */
static real spartan_link_text_width(
	wchar_t const *text)
{
	long font_index = hud_get_font_index();
	rectangle2d bounds, text_bounds, cursor_bounds;

	if (font_index == NONE || !text[0])
		return 0.0f;
	draw_string_set_draw_mode(font_index, NONE, 0, 0, global_real_argb_white);
	bounds.x0 = 0;
	bounds.y0 = 0;
	bounds.x1 = 2000;
	bounds.y1 = 100;
	draw_unicode_string_compute_bounds(&bounds, text, &text_bounds, &cursor_bounds);
	return (real)(text_bounds.x1 - text_bounds.x0);
}

/* under a feed: the teammate's name, a thin bar of their shield, and how
far away they are (the name cut short, and the bar left out, where the
feed is too narrow for them all) */
static void spartan_link_draw_row(
	struct bitmap_data *white,
	short local_player_index,
	short index,
	real_rectangle2d const *bounds,
	real scale)
{
	struct spartan_link_feed const *feed = &spartan_link.windows[local_player_index].feeds[index];
	struct player_datum *teammate = player_try_and_get(feed->player_index);
	long self = local_player_get_player_index(local_player_index);
	real_argb_color text_color;
	real width = bounds->x1 - bounds->x0;
	real top = bounds->y1 + 1.5f * scale;
	real margin = 2.0f * scale;
	/* (the text as large as the design's at a feed's width of four, smaller
	in a narrower feed, to a point) */
	real text_scale = 0.6f * MAX(scale, 0.9f) * MAX(spartan_link.size, 0.9f) *
		PIN(width / (spartan_link_widths[SPARTAN_LINK_FEEDS - 1] * scale), 0.75f, 1.0f);
	real shield = 0.0f;
	real bar_height = MAX(2.0f * scale, 1.0f / (halo_screen_pixel_scale() > 0.0f ? halo_screen_pixel_scale() : 1.0f));
	real name_width, distance_width, bar_x0, bar_x1, bar_y;
	wchar_t name[16];
	wchar_t distance[16];
	short length;

	if (!teammate)
		return;
	hud_get_text_color(&text_color);
	ustrncpy(name, teammate->name, NUMBEROF(name) - 1);
	name[NUMBEROF(name) - 1] = 0;
	distance[0] = 0;
	if (feed->down_at)
	{
		usnprintf(distance, NUMBEROF(distance) - 1, L"DOWN");
	}
	else if (self != NONE && player_get(self)->unit_index != NONE &&
		object_try_and_get(feed->unit_index))
	{
		real_point3d here, there;

		object_get_origin(player_get(self)->unit_index, &here);
		object_get_origin(feed->unit_index, &there);
		usnprintf(distance, NUMBEROF(distance) - 1, L"%ldm", (long)floorf(distance3d(&here, &there) * 3.048f + 0.5f));
		shield = object_get(feed->unit_index)->object.shield_vitality;
	}
	distance[NUMBEROF(distance) - 1] = 0;
	distance_width = spartan_link_text_width(distance) * text_scale;
	/* (the name as long as fits beside the distance) */
	for (length = (short)ustrlen(name); length > 0; length--)
	{
		name[length] = 0;
		name_width = spartan_link_text_width(name) * text_scale;
		if (name_width + distance_width + 3.0f * margin <= width)
			break;
	}
	name_width = spartan_link_text_width(name) * text_scale;
	text_color.alpha = 1.0f;
	spartan_link_text(name, bounds->x0 + margin, top, name_width, 0, text_scale, &text_color);
	spartan_link_text(distance, bounds->x1 - margin - distance_width, top, distance_width, 0, text_scale,
		&text_color);
	/* (the shield: a thin bar between them, where there is room for one) */
	bar_x0 = bounds->x0 + margin * 2.0f + name_width;
	bar_x1 = bounds->x1 - margin * 2.0f - distance_width;
	bar_y = top + SPARTAN_LINK_ROW * scale * 0.38f;
	if (bar_x1 - bar_x0 >= 6.0f * scale)
	{
		shield = PIN(shield, 0.0f, 1.0f);
		spartan_link_rectangle(white, bar_x0, bar_y, bar_x1, bar_y + bar_height, spartan_link_color(0.35f, 0.6f));
		spartan_link_rectangle(white, bar_x0, bar_y, bar_x0 + (bar_x1 - bar_x0) * shield, bar_y + bar_height,
			spartan_link_color(0.95f, 1.0f));
	}
}

/* ---------- public code */

void spartan_link_update(
	void)
{
	unsigned long now = system_milliseconds();
	short local_player_index;
	boolean key;
	short port;

	spartan_link_read();
	/* (a new game: every link closed, as each game starts) */
	if (!spartan_link_playing() || game_time_get() < spartan_link.last_game_time)
	{
		if (spartan_link.in_game)
		{
			short view;

			csmemset(spartan_link.windows, 0, sizeof(spartan_link.windows));
			for (view = 1; view <= MAXIMUM_LOCAL_PLAYERS * SPARTAN_LINK_FEEDS; view++)
			{
				if (spartan_link.views_used & (1UL << view))
					halo_view_release(view);
			}
			spartan_link.views_used = 0;
		}
		spartan_link.in_game = FALSE;
	}
	if (spartan_link_playing())
	{
		if (!spartan_link.in_game)
		{
			csmemset(spartan_link.windows, 0, sizeof(spartan_link.windows));
			spartan_link.last_local_player = NONE;
			spartan_link.last_feed = NONE;
			/* (debug.spartan_link_open: open from the start, for the tests) */
			spartan_link.debug_open = config_boolean("debug.spartan_link_open");
		}
		spartan_link.in_game = TRUE;
		spartan_link.last_game_time = game_time_get();
	}
	/* the presses: a controller's (the stock layouts' D-pad Down, each
	port's own player's), the keyboard's (the first player's) */
	for (port = 0; port < MAXIMUM_LOCAL_PLAYERS; port++)
	{
		if (halo_spartan_link_pad_pressed(port))
			spartan_link_toggle(controller_remap_local_player(port), now);
	}
	key = (halo_keyboard_actions(0) & FLAG(HALO_KEYBOARD_SPARTAN_LINK)) != 0;
	if (key && !spartan_link.key_held && !console_is_active() && !network_chat_typing())
	{
		short first = controller_remap_local_player(0);

		if (first == NONE)
			first = local_player_get_next(NONE);
		if (first != NONE && !ui_widgets_active_for_local_player(first))
			spartan_link_toggle(first, now);
	}
	spartan_link.key_held = key;
	if (!spartan_link.in_game)
		return;
	/* (debug.spartan_link_log: each tick, every player's eye and aim as this
	machine has them, which another machine's feeds are compared with) */
	if (spartan_link.log && game_time_get() != spartan_link.logged_tick)
	{
		struct data_iterator iterator;
		struct player_datum *player;

		spartan_link.logged_tick = game_time_get();
		data_iterator_new(&iterator, player_data);
		while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		{
			struct camera_command command;

			if (player->unit_index == NONE || !object_try_and_get_and_verify_type(player->unit_index, _object_mask_unit))
				continue;
			first_person_camera_fake(player->unit_index, &command);
			platform_log("spartan link player: %ld %ld %.4f %.4f %.4f %.5f %.5f %.5f", game_time_get(),
				(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index), command.position.x, command.position.y,
				command.position.z, command.forward.i, command.forward.j, command.forward.k);
		}
	}
	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		struct spartan_link_window *window = &spartan_link.windows[local_player_index];

		if ((window->open || spartan_link.debug_open) && spartan_link_game())
			spartan_link_window_update(local_player_index, now);
		else if (window->open && !spartan_link_game())
			window->open = FALSE;
	}
	spartan_link_schedule();
}

unsigned char spartan_link_pad_open(
	short port)
{
	short local_player_index;

	if (!spartan_link.enabled || !spartan_link_playing() || cinematic_in_progress() || !controller_remap_playing(port))
		return FALSE;
	local_player_index = controller_remap_local_player(port);
	return local_player_index != NONE && !ui_widgets_active_for_local_player(local_player_index) &&
		(game_engine_running() || network_coop_active() || local_player_count() > 1);
}

boolean spartan_link_feed_begin(
	short local_player_index,
	struct render_camera *camera)
{
	struct spartan_link_window *window;
	struct spartan_link_feed *feed;
	struct player_datum *teammate;
	struct camera_command command;
	real_rectangle2d rectangles[SPARTAN_LINK_FEEDS];
	director_perspective perspective;
	real scale;
	real_point3d eye;
	boolean through_eyes;
	unsigned long width, height;
	int view;

	if (local_player_index != spartan_link.due_local_player || spartan_link.due_feed == NONE ||
		!spartan_link_shown(local_player_index))
	{
		return FALSE;
	}
	window = &spartan_link.windows[local_player_index];
	if (spartan_link.due_feed >= window->count ||
		spartan_link_layout(local_player_index, camera, rectangles, &scale) <= spartan_link.due_feed)
	{
		return FALSE;
	}
	feed = &window->feeds[spartan_link.due_feed];
	teammate = player_try_and_get(feed->player_index);
	if (!teammate || feed->down_at || !object_try_and_get_and_verify_type(feed->unit_index, _object_mask_unit))
		return FALSE;
	/* (the next frame's is another's) */
	spartan_link.last_local_player = spartan_link.due_local_player;
	spartan_link.last_feed = spartan_link.due_feed;
	spartan_link.due_local_player = NONE;
	spartan_link.due_feed = NONE;

	/* its camera: their eye as their body is drawn, along their aim; or in a
	seat whose camera follows the vehicle, from behind */
	first_person_camera_fake(feed->unit_index, &command);
	render_interpolation_unit_camera_position(feed->unit_index, &eye);
	through_eyes = !director_desired_perspective(feed->unit_index, &perspective);
	camera->forward = command.forward;
	camera->up = command.up;
	if (through_eyes)
	{
		camera->position = eye;
	}
	else
	{
		struct object_datum *unit = object_get(feed->unit_index);
		long vehicle_index = unit->object.parent_object_index;
		real radius = vehicle_index != NONE && object_try_and_get(vehicle_index) ?
			object_get(vehicle_index)->object.bounding_sphere_radius : 1.0f;
		real back = radius * 1.6f + 0.6f;

		camera->position.x = eye.x - command.forward.i * back;
		camera->position.y = eye.y - command.forward.j * back;
		camera->position.z = eye.z - command.forward.k * back + radius * 0.35f;
	}
	camera->vertical_field_of_view = 2.0f * arctangent(
		0.75f * render_camera_get_adjusted_field_of_view_tangent(
			through_eyes ? spartan_link_field_of_view(feed->unit_index) : DEGREES_TO_RADIANS(70.0f)),
		1.0f);
	camera->mirrored = FALSE;
	camera->z_near = rasterizer_globals.near_clip_distance;
	camera->z_far = render_far_clip_distance(local_player_index);
	/* its window: its size on the screen, from the screen's top left */
	width = (unsigned long)floorf(rectangles[spartan_link.last_feed].x1 - rectangles[spartan_link.last_feed].x0 + 0.5f);
	height = (unsigned long)floorf(rectangles[spartan_link.last_feed].y1 - rectangles[spartan_link.last_feed].y0 + 0.5f);
	if (width < 8 || height < 8)
		return FALSE;
	camera->viewport_bounds.x0 = 0;
	camera->viewport_bounds.y0 = 0;
	camera->viewport_bounds.x1 = (short)width;
	camera->viewport_bounds.y1 = (short)height;
	camera->window_bounds = camera->viewport_bounds;
	feed->width = width;
	feed->height = height;

	view = SPARTAN_LINK_VIEW(local_player_index, spartan_link.last_feed);
	spartan_link.views_used |= 1UL << view;
	spartan_link.drawing = TRUE;
	spartan_link.hidden_unit = through_eyes ? feed->unit_index : NONE;
	spartan_link.posed_unit = through_eyes && object_get(feed->unit_index)->object.parent_object_index == NONE ?
		feed->unit_index : NONE;
	halo_view_begin(view, width, height);
	/* (what the view's drawing sets of the lights, put back after it) */
	lights_port_keep(TRUE);
	if (spartan_link.log)
	{
		platform_log("spartan link: %ld %d %ld %d %.4f %.4f %.4f %.5f %.5f %.5f %.4f %.3f", game_time_get(),
			(int)local_player_index, (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(feed->player_index), through_eyes ? 1 : 0,
			camera->position.x, camera->position.y, camera->position.z, camera->forward.i, camera->forward.j,
			camera->forward.k, camera->vertical_field_of_view, render_interpolation_fraction());
	}
	return TRUE;
}

void spartan_link_feed_end(
	void)
{
	halo_view_end();
	lights_port_keep(FALSE);
	spartan_link.drawing = FALSE;
	spartan_link.hidden_unit = NONE;
	spartan_link.posed_unit = NONE;
}

boolean spartan_link_drawing(
	void)
{
	return spartan_link.drawing;
}

long spartan_link_hidden_unit(
	void)
{
	return spartan_link.drawing ? spartan_link.hidden_unit : NONE;
}

long spartan_link_posed_unit(
	void)
{
	return spartan_link.drawing ? spartan_link.posed_unit : NONE;
}

void spartan_link_draw(
	short local_player_index)
{
	struct spartan_link_window *window;
	struct bitmap_data *white;
	real_rectangle2d rectangles[SPARTAN_LINK_FEEDS];
	real scale;
	short count;
	short index;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS || spartan_link.drawing)
		return;
	window = &spartan_link.windows[local_player_index];
	white = NULL;
	/* (the HUD's coordinates are the view's own, from its top left: a split
	screen's lower window's too) */
	if (window->refusal && system_milliseconds() - window->refusal_at < SPARTAN_LINK_REFUSAL_MILLISECONDS)
	{
		real_argb_color color;
		rectangle2d const *frame = &render.camera.window_bounds;
		real height = (real)(frame->y1 - frame->y0);
		real line_scale = PIN(height / 480.0f * 1.5f, 0.6f, 1.0f);

		hud_get_text_color(&color);
		color.alpha = 1.0f;
		spartan_link_text(window->refusal, (real)(frame->x0 - render.camera.viewport_bounds.x0),
			SPARTAN_LINK_TOP * line_scale, (real)(frame->x1 - frame->x0), 2, 0.8f * line_scale, &color);
	}
	if (!spartan_link_shown(local_player_index))
		return;
	white = spartan_link_white();
	if (!white)
		return;
	count = spartan_link_layout(local_player_index, &render.camera, rectangles, &scale);
	for (index = 0; index < count; index++)
	{
		spartan_link_draw_feed(white, local_player_index, index, &rectangles[index], scale);
		spartan_link_draw_row(white, local_player_index, index, &rectangles[index], scale);
	}
}
