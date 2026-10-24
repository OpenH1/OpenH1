/*
BATTERY_SAVER.C

display.battery_saver: "on" draws less, for a longer battery and a cooler
machine (a Steam Deck, a phone, a laptop); "auto" turns it on while the
machine runs on its battery and off on its charger, as SDL_GetPowerInfo
says (a machine without a battery, or one SDL cannot tell, is taken to be
on its charger); "off", the default, never.

While it is on:
- each frame stays on the display for a whole number of its refreshes, 30
  to 60 frames a second (frame_pacing.h, frame_pacing_divisor), where the
  window is shown (platform_frame_rate_apply, sdl_platform.c): the Vulkan
  host presents each frame that many times, the Metal host's display link
  runs at that rate; without vsync, the frames are spaced by the clock
  (platform_video_swap);
- the settings display.graphics chooses that the player has not set are
  read at their "classic" values (port_config.c, config_battery_saver_set).
The game's ticks are as they were (30 a second, or game.sim_rate's): only
the drawing changes, so that a network game is the same on every machine.

Never in VR (the headset's runtime paces its frames, and a frame shown for
two of its refreshes would make its player sick): battery saver is off
while the game plays in the headset.

The battery is looked at on a thread of its own every POWER_POLL_MS (SDL
asks the system, which can take a while: UPower over D-Bus on Linux, Java
on Android), once "auto" is chosen.
*/

#include "platform.h"
#include "battery_saver.h"
#include "port_config.h"
#include "sdl_platform.h"
#ifdef HALO_VR
#include "vr.h"
#endif

#include <SDL3/SDL.h>
#include <pthread.h>
#include <string.h>

#define POWER_POLL_MS 5000

enum
{
	_battery_saver_off,
	_battery_saver_on,
	_battery_saver_auto,
};

static struct
{
	int setting;
	unsigned long read_at;
	int active;
	/* (auto) the thread that looks at the battery is started; the machine
	runs on its battery, as it last said (-1 not yet) */
	int polling;
	volatile int on_battery;
} battery_saver = { _battery_saver_off, (unsigned long)-1, 0, 0, -1 };

static void *battery_saver_poll(void *argument)
{
	(void)argument;
	for (;;)
	{
		if (__atomic_load_n(&battery_saver.setting, __ATOMIC_RELAXED) == _battery_saver_auto)
		{
			SDL_PowerState state = SDL_GetPowerInfo(NULL, NULL);
			int on_battery = state == SDL_POWERSTATE_ON_BATTERY;

			if (on_battery != __atomic_load_n(&battery_saver.on_battery, __ATOMIC_RELAXED))
			{
				platform_log("battery saver: the machine runs on %s", on_battery ? "its battery" :
					state == SDL_POWERSTATE_NO_BATTERY ? "mains power (it has no battery)" :
					state == SDL_POWERSTATE_UNKNOWN || state == SDL_POWERSTATE_ERROR ?
					"its charger, as far as can be told" : "its charger");
				__atomic_store_n(&battery_saver.on_battery, on_battery, __ATOMIC_RELAXED);
			}
		}
		SDL_Delay(POWER_POLL_MS);
	}
	return NULL;
}

static void battery_saver_read(void)
{
	const char *setting = config_string("display.battery_saver");
	int value = !strcmp(setting, "on") ? _battery_saver_on : !strcmp(setting, "auto") ? _battery_saver_auto :
		_battery_saver_off;

	if (value == _battery_saver_off && strcmp(setting, "off"))
		platform_log("battery saver: display.battery_saver \"%s\" is unknown, so off", setting);
	__atomic_store_n(&battery_saver.setting, value, __ATOMIC_RELAXED);
	if (value == _battery_saver_auto && !battery_saver.polling)
	{
		pthread_t thread;

		battery_saver.polling = 1;
		if (pthread_create(&thread, NULL, battery_saver_poll, NULL) == 0)
			pthread_detach(thread);
		else
			platform_log("battery saver: cannot look at the battery; auto stays off");
	}
}

void battery_saver_update(void)
{
	int active;

	if (battery_saver.read_at != config_changes())
	{
		battery_saver.read_at = config_changes();
		battery_saver_read();
	}
	active = battery_saver.setting == _battery_saver_on ||
		(battery_saver.setting == _battery_saver_auto &&
			__atomic_load_n(&battery_saver.on_battery, __ATOMIC_RELAXED) == 1);
#ifdef HALO_VR
	if (halo_vr_running())
		active = 0;
#endif
	if (active == battery_saver.active)
		return;
	battery_saver.active = active;
	platform_log("battery saver: %s", active ? "on" : "off");
	/* (config_changes moves: read again at the next frame, which finds it
	as it is) */
	config_battery_saver_set(active);
	battery_saver.read_at = config_changes();
	platform_frame_rate_apply();
}

int battery_saver_active(void)
{
	return battery_saver.active;
}
