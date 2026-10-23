/*
HOST_APP.M (iOS)

The iOS app (OpenH1.app, tools/ios_build.py): what the host does before the
game starts, in place of port/macos/host_app.m.

- The app's files are in its Documents folder, which the Files app shows
  (Info.plist's UIFileSharingEnabled and LSSupportsOpeningDocumentsInPlace):
  config.toml (SDL's base path for the game), the data root (maps,
  custom_maps, debug.txt) and the saves (saves). The host's log (with the
  game's lines) is logs/halo.log there, the previous start's
  logs/halo.previous.log. The Metal pipelines' archive is in
  Library/Caches/OpenH1 (host_metal.m). HALO_DATA_ROOT and HALO_SAVE_ROOT,
  and config.toml's paths.data and paths.saves, still choose other folders.
- Internet play's brokers (brokers.txt in the app) are copied beside
  config.toml at each start, as on macOS.
- The background: as the app goes there (SDL's WILL_ENTER_BACKGROUND), the
  presentation stops (iOS refuses GPU work from the background:
  host_metal_background) and the game waits in its next event poll
  (host_app_background_wait, port/android/host/host_sdl.c), turning UIKit's
  run loop, until the app is back in the foreground. iOS suspends the
  process meanwhile; a network game's other machines drop it after 15
  seconds, as on Android.
*/

#import <UIKit/UIKit.h>
#include <SDL3/SDL.h>

/* (host.h's host_page_size is Mach's too: not used here) */
#define host_page_size host_page_size_of_guest_memory
#include "host.h"
#undef host_page_size
#include "tomlc17.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static NSString *documents_folder;
static char settings_folder[1024];

const char *host_app_settings_folder(void)
{
	return settings_folder[0] ? settings_folder : NULL;
}

const char *host_app_documents_folder(void)
{
	return documents_folder ? documents_folder.fileSystemRepresentation : NULL;
}

/* port/macos/host_metal.m */
void host_metal_background(int away);

/* ---------- the background */

static atomic_int in_background;

static bool SDLCALL lifecycle_event(void *userdata, SDL_Event *event)
{
	(void)userdata;
	if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND)
	{
		host_logf(HOST_LOG_INFO, "the app goes to the background: the game waits");
		atomic_store(&in_background, 1);
		host_metal_background(1);
	}
	else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND)
	{
		host_logf(HOST_LOG_INFO, "the app is back in the foreground");
		atomic_store(&in_background, 0);
		host_metal_background(0);
	}
	return true;
}

/* (the main thread, the game's: host_sdl.c) */
void host_app_background_wait(void)
{
	static int watching;

	if (!watching)
	{
		watching = 1;
		SDL_AddEventWatch(lifecycle_event, NULL);
	}
	while (atomic_load(&in_background))
	{
		SDL_PumpEvents();
		SDL_Delay(50);
	}
}

/* ---------- the log */

static void log_open(NSString *logs)
{
	NSString *log = [logs stringByAppendingPathComponent:@"halo.log"];
	NSString *previous = [logs stringByAppendingPathComponent:@"halo.previous.log"];

	rename(log.fileSystemRepresentation, previous.fileSystemRepresentation);
	if (!freopen(log.fileSystemRepresentation, "w", stderr))
		return;
	dup2(STDERR_FILENO, STDOUT_FILENO);
	setvbuf(stderr, NULL, _IOLBF, 0);
	setvbuf(stdout, NULL, _IOLBF, 0);
}

/* ---------- config.toml's settings the app reads */

static NSString *config_path_setting(const char *name)
{
	NSString *path = [documents_folder stringByAppendingPathComponent:@"config.toml"];
	toml_result_t result = toml_parse_file_ex(path.fileSystemRepresentation);
	NSString *value = nil;

	if (!result.ok)
		return nil;
	{
		toml_datum_t datum = toml_seek(result.toptab, name);

		if (datum.type == TOML_STRING && datum.u.s[0])
			value = [NSString stringWithUTF8String:datum.u.s];
	}
	toml_free(result);
	return value;
}

/* ---------- start */

void host_app_start(void)
{
	@autoreleasepool
	{
		NSBundle *bundle = [NSBundle mainBundle];
		NSFileManager *files = [NSFileManager defaultManager];
		NSURL *documents = [files URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
		NSURL *caches = [[files URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject
			URLByAppendingPathComponent:@"OpenH1"];
		NSString *logs;
		NSString *brokers;
		NSString *saves;

		/* (SDL's view controller turns with the device: landscape only, as
		Info.plist says, whichever way up; the home indicator hidden until
		the screen's edge is touched) */
		SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
		SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "1");
		documents_folder = documents.path;
		logs = [documents_folder stringByAppendingPathComponent:@"logs"];
		[files createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:nil error:NULL];
		[files createDirectoryAtURL:caches withIntermediateDirectories:YES attributes:nil error:NULL];
		log_open(logs);
		snprintf(settings_folder, sizeof(settings_folder), "%s/", documents_folder.fileSystemRepresentation);
		host_logf(HOST_LOG_INFO, "%s %s (%s) on %s %s, %s", [[bundle objectForInfoDictionaryKey:@"CFBundleName"] UTF8String],
			[[bundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] UTF8String],
			bundle.bundleIdentifier.UTF8String, UIDevice.currentDevice.systemName.UTF8String,
			UIDevice.currentDevice.systemVersion.UTF8String, bundle.bundlePath.fileSystemRepresentation);
		host_logf(HOST_LOG_INFO, "settings, game data and saves: %s", settings_folder);

		/* internet play's brokers (network.brokers_file), the app's list
		beside config.toml at each start */
		brokers = [bundle pathForResource:@"brokers" ofType:@"txt"];
		if (brokers)
		{
			NSString *copy = [documents_folder stringByAppendingPathComponent:@"brokers.txt"];

			[files removeItemAtPath:copy error:NULL];
			[files copyItemAtPath:brokers toPath:copy error:NULL];
		}

		saves = [documents_folder stringByAppendingPathComponent:@"saves"];
		if (!getenv("HALO_SAVE_ROOT") && !config_path_setting("paths.saves"))
			setenv("HALO_SAVE_ROOT", saves.fileSystemRepresentation, 1);
		if (!getenv("HALO_DATA_ROOT") && !config_path_setting("paths.data"))
			setenv("HALO_DATA_ROOT", documents_folder.fileSystemRepresentation, 1);
		/* (HOME is the app's container: the Metal archive's folder,
		Library/Caches/OpenH1) */
	}
}
