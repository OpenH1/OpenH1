/*
LATENCY_LOG.C

Where the time goes between the first local player's input and the first
frame that shows its result (debug.latency_log): a line for each press of
the fire control and each move from standing, on the controller and on the
mouse, and each turn of the mouse from still, with the time each stage took,
in milliseconds of SDL's clock (SDL_GetTicksNS, the game's).
Off (debug.latency_log empty), the game's calls cost a test of
latency_log_on (LATENCY_LOG, halo_linux_source_fixups.h).

An event begins when a reading of the first controller (XInputGetState,
port/linux/src/xinput_sdl.c) first has the fire control pulled, or the left
stick pushed while the player's unit (or the vehicle it rides) stands still;
or the mouse's fire binding held (controls.fire), or the mouse moved after
it was still for LATENCY_STILL_NS (a reading of the mouse: the frame's, or
the drawn view's own late one, halo_linux_mouse_look_late); or, while a menu
is up, the D-pad, A or the left stick pressed (a menu event). A press the
scripted player made (debug.test_input "latency:") began when the script
pressed it, which a reading takes up to a frame later. A press of either
fire control or a move is acted on by the simulation step (a tick at
game.sim_rate 30, a sub-step at 60 and 120: game_time.c) in which the
unit's weapon fires (weapons.c), or the gun of the vehicle it rides, or the
unit (the vehicle) has moved from where it stood; a turn by the view's
taking it (the player's facing turned by the frame's input,
player_control.c, or the drawn view by the late reading), at the end of the
step after it; a menu event by the menus' taking it (ui_widget.c: the
selection moved, an item chosen or the screen left, as its sound plays),
which the next frame drawn shows. Its result is
shown by the first frame whose drawn time has reached that step: the frames
draw between the last two ticks (render_interpolation.c), the latest tick's
end less one tick plus the fraction the clock has run into the next
(render_interpolation_fraction), which counts the open tick's sub-steps; a
step's own time is its end, in ticks; but the player's own view, drawn from
the newest step (display.direct_camera), shows it in the first frame drawn
after it, and a turn the drawn view took late in the frame that took it.

    # latency log 1 rate <R>
    <kind> <press_to_read> <read_to_act> <act_to_frame> <frame_to_shown> <render> <present> <total> <fraction> <frame> <press_clock> <input_clock> <seat>

    kind            fire (the controller's fire control), move (its stick),
                    click (the mouse's fire), turn (the mouse's motion) or
                    menu (the D-pad, A or the stick in a menu)
    press_to_read   the scripted press to the reading that first has it (0
                    for a person's: the reading is when it is known)
    read_to_act     the reading to the step that acts on it: the wait for
                    the step, and the step itself
    act_to_frame    the step to the start of the first frame drawn after it
                    (0 for a turn the drawn view took late)
    frame_to_shown  that frame's start to the start of the first frame whose
                    drawn time reaches the step: interpolation's cost (0 when
                    that frame shows it)
    render          the frame that shows it: its drawing (its start, or the
                    late reading's, to the end of main_game_render)
    present         its end to main_present_frame's return (the throttle,
                    the frame handed to the renderer)
    total           the press to that return
    fraction        the interpolation fraction the showing frame drew at
    frame           the mean time between frames since the line before
    press_clock     the press, in nanoseconds of CLOCK_MONOTONIC (the
                    renderer's clock)
    input_clock     the input read of the frame that shows it, likewise:
                    the Vulkan renderer's HALO_VULKAN_FRAME_LOG has the
                    frame by it, with its present and its time on screen
                    (tools/latency_summary.py --frames)
    seat            1 when the player was in a vehicle's seat as the step
                    took the event, 0 on foot

An event read with no unit to act on, or a move while the unit moves, is
left out; one not acted on within a second (the weapon empty) is counted,
not logged; tools/latency_summary.py takes the lines' median and
95th percentile. The frame's own way to the screen is the renderer's: the
Metal backend's HALO_METAL_STATS logs its input to screen, the Vulkan
backend's HALO_VULKAN_FRAME_LOG each frame's (xgpu_vulkan_pacing.c).

The sound of a shot (a fire event's step) gets a line of its own once its
first frames are handed to the sound device (port/linux/src/dsound_sdl.c,
"the sound's latency"):

    sound <act_to_start> <start_to_request> <answer> <queued> <system> <total> <buffer> <dropouts>

    act_to_start      the step to the voice the game starts next (its sound
                      update, in the frame after the step)
    start_to_request  that to the device's next request for frames, whose
                      buffer has the voice from its start
    answer            the request to its frames handed back (the mix, and the
                      hand-off to the thread that runs it); not in the total
    queued            the frames SDL's driver keeps queued ahead of that
                      buffer (by the driver and the buffer)
    system            the system's own latency after, as it reports it (the
                      Mac's CoreAudio; -1 not known)
    total             the step to the sound heard: act_to_start,
                      start_to_request, queued and system
    buffer            the device's buffer (frames; audio.buffer_frames)
    dropouts          the times the device ran dry since it opened

A rumble begun from rest by an impulse to the first local player
(player_rumble.c: a shot, a hit, a grenade) gets a line once its motors
stop:

    rumble <step_to_call> <on> <duration>

    step_to_call      the impulse made, in its step, to the first write of
                      motors not at rest to the first controller
                      (XInputSetState, port/linux/src/xinput_sdl.c: the SDL
                      call), however the game writes them
    on                that write to the write that stops them
    duration          the impulse's own (its longer motor's), which the time
                      on matches but for the impulses after it
*/

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/players.h"
#include "objects/objects.h"
#include "units/units.h"
#include "sim_rate.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *config_string(char const *name);
void platform_log(char const *format, ...);
/* port/linux/game/render_interpolation.c */
float render_interpolation_fraction(void);
/* SDL's clock, the game's (port/linux/src: the frame's pacing, the
controllers' reading) */
unsigned long long SDL_GetTicksNS(void);
/* port/linux/src/d3d8_device.c: when the frame drawn read its input, in
nanoseconds of CLOCK_MONOTONIC (frame_pacing.h) */
unsigned long long d3d_frame_input_time(void);
unsigned long long d3d_frame_clock_now(void);
/* port/linux/src/dsound_sdl.c: the sound of a shot */
void dsound_sdl_latency_arm(unsigned long long act_ns);
int dsound_sdl_latency_take(unsigned long long *times, double *queued_ms, double *system_ms, long *buffer_frames,
	unsigned long *dropouts);

int latency_log_on;

enum
{
	_latency_fire,
	_latency_move,
	_latency_click,
	_latency_turn,
	_latency_menu,
	NUMBER_OF_LATENCY_KINDS,

	LATENCY_EVENTS = 8,
	/* a pull of the fire control, a push of the stick (of 255, 32767) */
	LATENCY_TRIGGER_DOWN = 128,
	LATENCY_TRIGGER_UP = 32,
	LATENCY_STICK_DOWN = 16000,
	LATENCY_STICK_UP = 4000,
};

/* (an event acted on before any step: a menu's) */
#define LATENCY_ACT_TIME_NOW -1.0e9

#define LATENCY_TIMEOUT_NS 1000000000ull
/* how long the mouse is still before its motion is a turn */
#define LATENCY_STILL_NS 100000000ull
/* how long before its reading a scripted press may have been made */
#define LATENCY_PRESS_NS 100000000ull
/* how far a unit standing still moves before it has moved (world units) */
#define LATENCY_MOVED 0.0005f
/* how fast a unit standing still may move (world units a tick) */
#define LATENCY_STILL 0.0005f

struct latency_event
{
	boolean in_use;
	short kind;
	unsigned long long press_ns, read_ns;
	/* acted on: in the running step, then at its time (in ticks) */
	boolean begun, acting, acted;
	unsigned long long act_ns;
	double act_time;
	real_point3d stood;
	/* in a vehicle's seat as the step took it */
	boolean seated;
	/* the first frame begun after the step, the first that shows it */
	unsigned long long frame_ns, shown_ns, rendered_ns;
	/* the input read of the frame that shows it (CLOCK_MONOTONIC) */
	unsigned long long shown_input;
	real fraction;
	boolean framed, shown, rendered;
};

static struct
{
	boolean checked;
	FILE *file;
	boolean fire_down, move_down, click_down, menu_down;
	/* when the mouse last moved */
	unsigned long long moved_ns;
	struct latency_event events[LATENCY_EVENTS];
	/* the step running */
	short substep_index;
	/* this frame's drawn time (ticks) */
	double frame_drawn;
	unsigned long long frame_ns;
	/* the scripted player's last press */
	unsigned long long pressed_ns;
	/* frames handed on since the last line, and since when */
	long frames;
	unsigned long long frames_since_ns;
	long dropped[NUMBER_OF_LATENCY_KINDS];
	long logged[NUMBER_OF_LATENCY_KINDS];
} latency_log;

static unsigned long long latency_now(
	void)
{
	return SDL_GetTicksNS();
}

static double latency_ms(
	unsigned long long from,
	unsigned long long to)
{
	return to > from ? (double)(to - from) / 1e6 : 0.0;
}

static boolean latency_log_check(
	void)
{
	if (!latency_log.checked)
	{
		char const *path = config_string("debug.latency_log");

		latency_log.checked = TRUE;
		if (path && *path)
		{
			latency_log.file = fopen(path, "w");
			if (latency_log.file)
				latency_log_on = TRUE;
			else
				platform_log("latency log: cannot write %s", path);
		}
	}
	return latency_log_on;
}

/* the first local player's unit, NONE for none (in a step: the game
running) */
static long latency_unit(
	void)
{
	long player_index = local_player_get_player_index(0);

	return player_index == NONE ? NONE : player_get(player_index)->unit_index;
}

static void latency_begin(
	short kind)
{
	short index;

	for (index = 0; index < LATENCY_EVENTS; index++)
	{
		struct latency_event *event = &latency_log.events[index];

		if (!event->in_use)
		{
			memset(event, 0, sizeof(*event));
			event->in_use = TRUE;
			event->kind = kind;
			event->read_ns = latency_now();
			event->press_ns = latency_log.pressed_ns && latency_log.pressed_ns <= event->read_ns &&
				event->read_ns - latency_log.pressed_ns < LATENCY_PRESS_NS ? latency_log.pressed_ns : event->read_ns;
			/* (a menu's has no unit to wait for: latency_log_substep_begin) */
			event->begun = kind == _latency_menu;
			return;
		}
	}
}

static void latency_end(
	struct latency_event *event)
{
	static char const *const names[NUMBER_OF_LATENCY_KINDS] = { "fire", "move", "click", "turn", "menu" };
	short kind;
	long logged = 0;
	unsigned long long clock_ns, ticks_ns;

	/* (SDL's clock to CLOCK_MONOTONIC, now: SDL's may be another clock,
	CLOCK_MONOTONIC_RAW, which drifts from it) */
	ticks_ns = latency_now();
	clock_ns = d3d_frame_clock_now();
	for (kind = 0; kind < NUMBER_OF_LATENCY_KINDS; kind++)
		logged += latency_log.logged[kind];
	if (!logged)
		fprintf(latency_log.file, "# latency log 1 rate %ld\n", sim_rate_get());
	fprintf(latency_log.file, "%s %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %llu %llu %d\n", names[event->kind],
		latency_ms(event->press_ns, event->read_ns), latency_ms(event->read_ns, event->act_ns),
		latency_ms(event->act_ns, event->frame_ns), latency_ms(event->frame_ns, event->shown_ns),
		latency_ms(event->shown_ns, event->rendered_ns), latency_ms(event->rendered_ns, latency_log.frame_ns),
		latency_ms(event->press_ns, latency_log.frame_ns), event->fraction,
		latency_log.frames ? latency_ms(latency_log.frames_since_ns, latency_log.frame_ns) / latency_log.frames : 0.0,
		event->press_ns + clock_ns - ticks_ns, event->shown_input, event->seated ? 1 : 0);
	latency_log.frames = 0;
	latency_log.frames_since_ns = latency_log.frame_ns;
	fflush(latency_log.file);
	latency_log.logged[event->kind]++;
	event->in_use = FALSE;
}

/* (an event never acted on) */
static void latency_sweep(
	void)
{
	unsigned long long now = latency_now();
	short index;

	for (index = 0; index < LATENCY_EVENTS; index++)
	{
		struct latency_event *event = &latency_log.events[index];

		if (event->in_use && !event->acted && now - event->read_ns > LATENCY_TIMEOUT_NS)
		{
			if (event->begun)
				latency_log.dropped[event->kind]++;
			event->in_use = FALSE;
		}
	}
}

/* (the mouse moved: a turn, if it was still) */
static void latency_moved(
	float dx,
	float dy)
{
	unsigned long long now = latency_now();

	if (dx == 0.0f && dy == 0.0f)
		return;
	if (!latency_log.moved_ns || now - latency_log.moved_ns >= LATENCY_STILL_NS)
		latency_begin(_latency_turn);
	latency_log.moved_ns = now;
}

/* the view took the mouse's motion: the turns read, acted on (late: the drawn
view's own reading, in the frame drawing it) */
static void latency_turned(
	boolean late)
{
	unsigned long long now = latency_now();
	long unit_index = latency_unit();
	short index;

	for (index = 0; index < LATENCY_EVENTS; index++)
	{
		struct latency_event *event = &latency_log.events[index];

		if (!event->in_use || event->kind != _latency_turn || event->acted)
			continue;
		if (unit_index == NONE)
		{
			event->in_use = FALSE;
			continue;
		}
		event->begun = event->acting = event->acted = TRUE;
		event->seated = object_get(unit_index)->object.parent_object_index != NONE;
		event->act_ns = now;
		/* (the end of the step after it, as the unit takes it, for a view
		drawn between the ticks) */
		event->act_time = (double)game_time_get() + (double)(sim_substep_next() + 1) / (double)sim_substep_count();
		if (late)
		{
			event->framed = event->shown = TRUE;
			event->frame_ns = event->shown_ns = now;
			event->shown_input = d3d_frame_input_time();
			event->fraction = render_interpolation_fraction();
		}
	}
}

/* ---------- public code */

void latency_log_pressed(
	unsigned long long when_ns)
{
	latency_log.pressed_ns = when_ns;
}

void latency_log_read(
	int trigger,
	int stick_x,
	int stick_y)
{
	int stick;

	if (!latency_log_check())
		return;
	stick = abs(stick_x) > abs(stick_y) ? abs(stick_x) : abs(stick_y);
	if (!latency_log.fire_down && trigger >= LATENCY_TRIGGER_DOWN)
		latency_begin(_latency_fire);
	if (!latency_log.move_down && stick >= LATENCY_STICK_DOWN)
		latency_begin(_latency_move);
	if (trigger >= LATENCY_TRIGGER_DOWN)
		latency_log.fire_down = TRUE;
	else if (trigger <= LATENCY_TRIGGER_UP)
		latency_log.fire_down = FALSE;
	if (stick >= LATENCY_STICK_DOWN)
		latency_log.move_down = TRUE;
	else if (stick <= LATENCY_STICK_UP)
		latency_log.move_down = FALSE;
	latency_sweep();
}

void latency_log_menu_read(
	int pressed,
	int menus)
{
	if (!latency_log_check())
		return;
	if (pressed && menus && !latency_log.menu_down)
		latency_begin(_latency_menu);
	latency_log.menu_down = pressed != 0;
}

void latency_log_menu_acted(
	void)
{
	unsigned long long now = latency_now();
	short index;

	for (index = 0; index < LATENCY_EVENTS; index++)
	{
		struct latency_event *event = &latency_log.events[index];

		if (event->in_use && event->kind == _latency_menu && !event->acted)
		{
			event->acting = event->acted = TRUE;
			event->act_ns = now;
			event->act_time = LATENCY_ACT_TIME_NOW;
		}
	}
}

void latency_log_mouse_read(
	int fire,
	float dx,
	float dy)
{
	if (!latency_log_check())
		return;
	if (fire && !latency_log.click_down)
		latency_begin(_latency_click);
	latency_log.click_down = fire != 0;
	latency_moved(dx, dy);
}

void latency_log_mouse_turned(
	void)
{
	if (latency_log_on)
		latency_turned(FALSE);
}

void latency_log_mouse_late(
	float dx,
	float dy)
{
	if (!latency_log_on)
		return;
	latency_moved(dx, dy);
	latency_turned(TRUE);
}

void latency_log_fire(
	long owner_object_index)
{
	long unit_index = latency_unit();
	short index;

	/* (the unit's own weapon, or the vehicle's it rides: a gunner's, a
	Ghost's) */
	if (unit_index == NONE || object_get_ultimate_parent(owner_object_index) != object_get_ultimate_parent(unit_index))
		return;
	for (index = 0; index < LATENCY_EVENTS; index++)
	{
		struct latency_event *event = &latency_log.events[index];

		if (event->in_use && event->begun && (event->kind == _latency_fire || event->kind == _latency_click) &&
			!event->acting && !event->acted)
		{
			event->acting = TRUE;
			event->act_ns = latency_now();
			dsound_sdl_latency_arm(event->act_ns);
		}
	}
}

void latency_log_substep_begin(
	void)
{
	long unit_index = latency_unit();
	short index;

	latency_log.substep_index = sim_substep_next();
	/* (an event read since the last step: the unit it is for, where it
	stands, or the vehicle it rides; a move from standing only) */
	for (index = 0; index < LATENCY_EVENTS; index++)
	{
		struct latency_event *event = &latency_log.events[index];
		struct object_datum *mover;

		if (!event->in_use || event->begun)
			continue;
		if (unit_index == NONE)
		{
			event->in_use = FALSE;
			continue;
		}
		mover = object_get(object_get_ultimate_parent(unit_index));
		if (event->kind == _latency_move && magnitude3d(&mover->object.translational_velocity) >= LATENCY_STILL)
		{
			event->in_use = FALSE;
			continue;
		}
		event->begun = TRUE;
		event->stood = mover->object.position;
		event->seated = object_get(unit_index)->object.parent_object_index != NONE;
	}
}

void latency_log_substep_end(
	void)
{
	long unit_index = latency_unit();
	unsigned long long now = latency_now();
	/* (the step's end, in ticks: the tick it is in counted from its start) */
	double time = (double)game_time_get() + (double)(latency_log.substep_index + 1) / (double)sim_substep_count();
	short index;

	for (index = 0; index < LATENCY_EVENTS; index++)
	{
		struct latency_event *event = &latency_log.events[index];

		if (!event->in_use || !event->begun || event->acted)
			continue;
		if (event->kind == _latency_move && unit_index != NONE)
		{
			if (distance3d(&event->stood, &object_get(object_get_ultimate_parent(unit_index))->object.position) >
				LATENCY_MOVED)
			{
				event->acting = TRUE;
				event->act_ns = now;
			}
		}
		if (event->acting)
		{
			event->acted = TRUE;
			event->act_time = time;
		}
	}
}

void latency_log_frame_begin(
	void)
{
	short index;

	latency_log.frame_ns = latency_now();
	/* (between the last two ticks: the latest's end less one, and the way
	into the next) */
	latency_log.frame_drawn = (double)game_time_get() - 1.0 + (double)render_interpolation_fraction();
	for (index = 0; index < LATENCY_EVENTS; index++)
	{
		struct latency_event *event = &latency_log.events[index];

		if (!event->in_use || !event->acted || event->shown)
			continue;
		if (!event->framed)
		{
			event->framed = TRUE;
			event->frame_ns = latency_log.frame_ns;
		}
		/* (the player's own view drawn from the newest step shows every step
		run: render_interpolation_view_newest) */
		if (render_interpolation_view_newest(0) || latency_log.frame_drawn >= event->act_time - 1e-3)
		{
			event->shown = TRUE;
			event->shown_ns = latency_log.frame_ns;
			event->shown_input = d3d_frame_input_time();
			event->fraction = render_interpolation_fraction();
		}
	}
}

void latency_log_frame_rendered(
	void)
{
	unsigned long long now = latency_now();
	short index;

	for (index = 0; index < LATENCY_EVENTS; index++)
	{
		struct latency_event *event = &latency_log.events[index];

		if (event->in_use && event->shown && !event->rendered)
		{
			event->rendered = TRUE;
			event->rendered_ns = now;
		}
	}
}

/* a rumble from rest: the impulse that began it, and its motors' writes
(the first controller's, from the thread that writes them) */
static struct
{
	pthread_mutex_t lock;
	boolean caused, on;
	unsigned long long caused_ns, on_ns;
	double duration_ms;
} latency_rumble = { PTHREAD_MUTEX_INITIALIZER };

void latency_log_rumble_caused(
	short local_player_index,
	real duration_seconds)
{
	if (local_player_index != 0 || duration_seconds <= 0.0f)
		return;
	pthread_mutex_lock(&latency_rumble.lock);
	if (!latency_rumble.on && !latency_rumble.caused)
	{
		latency_rumble.caused = TRUE;
		latency_rumble.caused_ns = latency_now();
		latency_rumble.duration_ms = duration_seconds * 1000.0;
	}
	pthread_mutex_unlock(&latency_rumble.lock);
}

void latency_log_rumble_written(
	unsigned left,
	unsigned right)
{
	unsigned long long now;

	if (!latency_log_on)
		return;
	now = latency_now();
	pthread_mutex_lock(&latency_rumble.lock);
	if (!latency_rumble.on && (left || right) && latency_rumble.caused)
	{
		latency_rumble.on = TRUE;
		latency_rumble.on_ns = now;
	}
	else if (latency_rumble.on && !left && !right)
	{
		fprintf(latency_log.file, "rumble %.3f %.3f %.3f\n", latency_ms(latency_rumble.caused_ns, latency_rumble.on_ns),
			latency_ms(latency_rumble.on_ns, now), latency_rumble.duration_ms);
		fflush(latency_log.file);
		latency_rumble.on = latency_rumble.caused = FALSE;
	}
	/* (one written as nothing: the profile's vibration off) */
	else if (!latency_rumble.on && latency_rumble.caused && now - latency_rumble.caused_ns > LATENCY_TIMEOUT_NS)
		latency_rumble.caused = FALSE;
	pthread_mutex_unlock(&latency_rumble.lock);
}

/* the sound of a shot, once handed to the device */
static void latency_sound(
	void)
{
	unsigned long long times[4];
	double queued, system, act_to_start, start_to_request;
	long buffer_frames;
	unsigned long dropouts;

	if (!dsound_sdl_latency_take(times, &queued, &system, &buffer_frames, &dropouts))
		return;
	/* (the voice may start while the request it is mixed for waits) */
	act_to_start = latency_ms(times[0], times[1]);
	start_to_request = times[2] >= times[1] ? latency_ms(times[1], times[2]) : -latency_ms(times[2], times[1]);
	fprintf(latency_log.file, "sound %.3f %.3f %.3f %.3f %.3f %.3f %ld %lu\n", act_to_start, start_to_request,
		latency_ms(times[2], times[3]), queued, system, act_to_start + start_to_request + queued +
		(system > 0.0 ? system : 0.0), buffer_frames, dropouts);
	fflush(latency_log.file);
}

void latency_log_frame_presented(
	void)
{
	short index;

	/* (frame_ns, from here on, is when the frame was handed on) */
	latency_log.frame_ns = latency_now();
	if (!latency_log.frames_since_ns)
		latency_log.frames_since_ns = latency_log.frame_ns;
	else
		latency_log.frames++;
	for (index = 0; index < LATENCY_EVENTS; index++)
	{
		struct latency_event *event = &latency_log.events[index];

		if (event->in_use && event->rendered)
			latency_end(event);
	}
	latency_sound();
}
