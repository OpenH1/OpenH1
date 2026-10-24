/*
HOST_APP.M

The macOS app (OpenH1.app, tools/macos_build.py): what the host does when its
executable runs inside the app's bundle, before the game starts, so that it
behaves as a Mac app is expected to. A bare executable (build/macos/halo)
does none of it, and the HALO_ settings of the environment (HALO_DATA_ROOT,
HALO_SAVE_ROOT) and config.toml's paths.data and paths.saves are kept.

- The app's files are in ~/Library/Application Support/OpenH1 (the folder
  the game finds config.toml in, its data root and its saves) and the
  host's log in ~/Library/Logs/OpenH1 (halo.log, and the previous start's
  halo.previous.log), where Console shows it, unless the error output is a
  terminal. The Metal pipelines' archive is in ~/Library/Caches/OpenH1
  (xgpu_metal.m).
- The game data: the players supply the Xbox game's maps folder or its
  disc image. Without one, an open panel asks for either. A folder is
  checked (ui.map, an Xbox cache, and the other maps of the disc), kept as a
  bookmark (maps.bookmark), which follows the folder when it is moved or
  renamed, and the data root's maps is a symbolic link to it, made again at
  each start. A disc image (.iso or .xiso, any name: halo1.xiso.iso) has its
  maps folder extracted into the data root's maps, a folder of its own, by
  port/linux/src/xiso.c (the desktop builds' and the iOS app's extractor),
  with a window showing the copy; the image is not needed after.
- The window: its place and size, and whether it was fullscreen, are kept
  (window.plist) and the next start opens it so (display.fullscreen, through
  HALO_FULLSCREEN, and the window's frame once it is a window).

- Updates: a release's app updates itself with Sparkle (Sparkle.framework
  in the bundle, loaded at run time; tools/macos_app.py gives a release's
  Info.plist its appcast and public key). It looks when the game reaches
  its main menu and every four hours after, unless update.auto is false; a
  new version's window (its notes, Install Update, Remind Me Later, Skip
  This Version) is shown only at the main menu: found during a game, it
  waits until the game is over (the guest says which, updater.c's
  updater_poll). Installing replaces the app and starts it again; the
  player's files, here in Application Support, are untouched. The app's
  menu has Check for Updates... and Automatically Check for Updates, which
  is update.auto (written to config.toml; Settings > Network Setup > Check
  for Updates is the same setting).

SDL makes the app's menus (About, Hide, Quit, the Window menu with Toggle
Full Screen) and turns Quit into the game's quit event.
*/

#import <AppKit/AppKit.h>
#import <Sparkle/Sparkle.h>
#include <SDL3/SDL.h>

/* (host.h's host_page_size is Mach's too: not used here) */
#define host_page_size host_page_size_of_guest_memory
#include "host.h"
#undef host_page_size
#include "tomlc17.h"
#include "xiso.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* the folder's name, under Application Support, Logs and Caches (not Halo:
Halo for Mac, MacSoft's port, has ~/Library/Application Support/Halo) */
#define APP_FOLDER @"OpenH1"

/* the Xbox disc's maps, which the game's menus offer: the campaign and
multiplayer */
static NSString *const disc_maps[] =
{
	@"a10", @"a30", @"a50", @"b30", @"b40", @"c10", @"c20", @"c40", @"d20", @"d40",
	@"beavercreek", @"bloodgulch", @"boardingaction", @"carousel", @"chillout", @"damnation", @"hangemhigh",
	@"longest", @"prisoner", @"putput", @"ratrace", @"sidewinder", @"wizard",
};

static NSString *support_folder;
static char settings_folder[1024];

const char *host_app_settings_folder(void)
{
	return settings_folder[0] ? settings_folder : NULL;
}

/* ---------- the log */

static void log_open(NSURL *logs)
{
	NSString *log = [logs URLByAppendingPathComponent:@"halo.log"].path;
	NSString *previous = [logs URLByAppendingPathComponent:@"halo.previous.log"].path;

	if (isatty(STDERR_FILENO))
		return;
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
	NSString *path = [support_folder stringByAppendingPathComponent:@"config.toml"];
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

/* a boolean setting of config.toml, else its default */
static BOOL config_boolean_setting(const char *name, BOOL default_value)
{
	NSString *path = [support_folder stringByAppendingPathComponent:@"config.toml"];
	toml_result_t result = toml_parse_file_ex(path.fileSystemRepresentation);
	BOOL value = default_value;

	if (!result.ok)
		return value;
	{
		toml_datum_t datum = toml_seek(result.toptab, name);

		if (datum.type == TOML_BOOLEAN)
			value = datum.u.boolean ? YES : NO;
	}
	toml_free(result);
	return value;
}

/* ---------- the maps */

/* xiso.c's log lines, the host's */
void platform_log(const char *format, ...)
{
	char line[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	host_logf(HOST_LOG_INFO, "%s", line);
}

/* name in folder, in any case (the volume may tell case apart), or nil */
static NSURL *entry_named(NSURL *folder, NSString *name)
{
	NSArray<NSURL *> *entries = [[NSFileManager defaultManager] contentsOfDirectoryAtURL:folder
		includingPropertiesForKeys:nil options:0 error:NULL];

	for (NSURL *entry in entries)
	{
		if ([entry.lastPathComponent caseInsensitiveCompare:name] == NSOrderedSame)
			return entry;
	}
	return nil;
}

enum maps_check
{
	_maps_good,
	_maps_incomplete,
	_maps_unusable
};

/* whether folder (or a maps folder in it) is the Xbox game's maps folder:
its maps (in *maps), else why not (in *problem; the missing maps where
incomplete) */
static enum maps_check maps_check(NSURL *folder, NSURL **maps, NSString **problem)
{
	NSURL *ui = entry_named(folder, @"ui.map");
	NSMutableArray<NSString *> *missing = [NSMutableArray array];
	NSFileHandle *file;
	NSData *header;
	uint32_t words[2];
	size_t index;

	if (!ui && entry_named(folder, @"maps"))
	{
		folder = entry_named(folder, @"maps");
		ui = entry_named(folder, @"ui.map");
	}
	*maps = folder;
	if (!ui)
	{
		*problem = [NSString stringWithFormat:@"“%@” has no ui.map: it is not Halo’s maps folder.",
			folder.lastPathComponent];
		return _maps_unusable;
	}
	file = [NSFileHandle fileHandleForReadingFromURL:ui error:NULL];
	header = [file readDataOfLength:sizeof(words)];
	[file closeFile];
	if (header.length != sizeof(words))
	{
		*problem = @"Its ui.map cannot be read.";
		return _maps_unusable;
	}
	memcpy(words, header.bytes, sizeof(words));
	/* a cache file's header: 'head' and its version, 5 for the Xbox's (7
	for the PC version's, 609 for Custom Edition's) */
	if (words[0] != 0x68656164)
	{
		*problem = @"Its ui.map is not one of Halo’s maps.";
		return _maps_unusable;
	}
	if (words[1] != 5)
	{
		*problem = @"These are the maps of Halo for PC or Halo Custom Edition. The game needs the maps "
			"folder of the Xbox game, Halo: Combat Evolved. (Custom Edition maps go in the custom_maps "
			"folder beside it.)";
		return _maps_unusable;
	}
	for (index = 0; index < sizeof(disc_maps) / sizeof(disc_maps[0]); index++)
	{
		if (!entry_named(folder, [disc_maps[index] stringByAppendingString:@".map"]))
			[missing addObject:disc_maps[index]];
	}
	if (missing.count)
	{
		*problem = [NSString stringWithFormat:@"%lu of the disc’s maps are not in it: %@.",
			(unsigned long)missing.count, [missing componentsJoinedByString:@", "]];
		return _maps_incomplete;
	}
	return _maps_good;
}

static NSString *bookmark_path(void)
{
	return [support_folder stringByAppendingPathComponent:@"maps.bookmark"];
}

/* the data root's maps, a link to the folder */
static BOOL maps_link(NSURL *maps)
{
	NSFileManager *files = [NSFileManager defaultManager];
	NSString *link = [support_folder stringByAppendingPathComponent:@"maps"];
	NSString *target = [files destinationOfSymbolicLinkAtPath:link error:NULL];
	NSError *error = nil;

	if (target && [target isEqualToString:maps.path])
		return YES;
	if (target)
		[files removeItemAtPath:link error:NULL];
	if (![files createSymbolicLinkAtPath:link withDestinationPath:maps.path error:&error])
	{
		host_logf(HOST_LOG_ERROR, "cannot link %s to %s: %s", link.fileSystemRepresentation,
			maps.fileSystemRepresentation, error.localizedDescription.UTF8String);
		return NO;
	}
	return YES;
}

static void maps_remember(NSURL *maps)
{
	NSError *error = nil;
	NSData *bookmark = [maps bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil
		error:&error];

	if (!bookmark || ![bookmark writeToFile:bookmark_path() atomically:YES])
		host_logf(HOST_LOG_ERROR, "cannot keep the maps folder's bookmark: %s",
			error ? error.localizedDescription.UTF8String : "cannot write it");
}

/* the folder the bookmark names, or nil */
static NSURL *maps_remembered(void)
{
	NSData *bookmark = [NSData dataWithContentsOfFile:bookmark_path()];
	BOOL stale = NO;
	NSURL *maps;

	if (!bookmark)
		return nil;
	maps = [NSURL URLByResolvingBookmarkData:bookmark options:NSURLBookmarkResolutionWithoutUI relativeToURL:nil
		bookmarkDataIsStale:&stale error:NULL];
	if (maps && stale)
		maps_remember(maps);
	return maps;
}

/* HALO_MAPS_PANEL_ANSWER, for the automated tests: the folders or disc
images (or "cancel") the panel is answered with, one for each time it is shown,
separated by semicolons; each alert is answered with its first button. The
answer comes a second after the panel or alert shows. */
static NSMutableArray<NSString *> *test_answers;

static void test_answer_soon(void (^answer)(void))
{
	NSTimer *timer = [NSTimer timerWithTimeInterval:1.0 repeats:NO block:^(NSTimer *unused) { answer(); }];

	[[NSRunLoop mainRunLoop] addTimer:timer forMode:NSModalPanelRunLoopMode];
}

static NSModalResponse alert(NSAlertStyle style, NSString *message, NSString *information, NSArray *buttons)
{
	NSAlert *box = [[NSAlert alloc] init];

	box.alertStyle = style;
	box.messageText = message;
	box.informativeText = information;
	for (NSString *button in buttons)
		[box addButtonWithTitle:button];
	host_logf(HOST_LOG_INFO, "%s: %s", message.UTF8String, information.UTF8String);
	if (test_answers)
	{
		test_answer_soon(^{
			host_logf(HOST_LOG_INFO, "test: answered \"%s\"", [buttons[0] UTF8String]);
			[NSApp stopModalWithCode:NSAlertFirstButtonReturn];
		});
	}
	return [box runModal];
}

/* the folder the panel is answered with, or nil for none */
static NSURL *test_answer_panel(void)
{
	NSString *answer;

	if (!test_answers.count)
		return nil;
	answer = test_answers.firstObject;
	[test_answers removeObjectAtIndex:0];
	test_answer_soon(^{
		host_logf(HOST_LOG_INFO, "test: the panel answered with %s", answer.UTF8String);
		[NSApp stopModalWithCode:[answer isEqualToString:@"cancel"] ? NSModalResponseCancel : NSModalResponseOK];
	});
	return [answer isEqualToString:@"cancel"] ? nil : [NSURL fileURLWithPath:answer];
}

/* whether url names a disc image: .iso or .xiso, in any case (and
halo1.xiso.iso) */
static BOOL is_disc_image(NSURL *url)
{
	NSString *extension = url.pathExtension.lowercaseString;

	return [extension isEqualToString:@"iso"] || [extension isEqualToString:@"xiso"];
}

static BOOL is_folder(NSURL *url)
{
	BOOL directory = NO;

	return [[NSFileManager defaultManager] fileExistsAtPath:url.path isDirectory:&directory] && directory;
}

/* the panel's choices: folders, and of the files only disc images (the
panel is given no types: .xiso has none of its own, and would be greyed
out) */
@interface HaloMapsPanelDelegate : NSObject <NSOpenSavePanelDelegate>
@end

@implementation HaloMapsPanelDelegate

- (BOOL)panel:(id)sender shouldEnableURL:(NSURL *)url
{
	(void)sender;
	return is_folder(url) || is_disc_image(url);
}

@end

/* the extraction's progress, which xiso.c reports from its thread */
static _Atomic(unsigned long long) extract_done, extract_total;

static void extract_progress(void *context, const char *file, unsigned long long done, unsigned long long total)
{
	(void)context;
	(void)file;
	atomic_store(&extract_done, done);
	atomic_store(&extract_total, total);
}

/* the disc image's maps folder copied into the data root's maps (a folder
of its own: no bookmark, no link), with a window showing the copy; nil, or
why not (for the player) */
static NSString *maps_extract(NSURL *image)
{
	static char error[512];
	NSFileManager *files = [NSFileManager defaultManager];
	NSString *maps = [support_folder stringByAppendingPathComponent:@"maps"];
	NSString *destination = support_folder;
	NSPanel *window = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 440, 110)
		styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
	NSTextField *label = [NSTextField labelWithString:[NSString stringWithFormat:
		@"Copying the maps out of “%@”…", image.lastPathComponent]];
	NSTextField *detail = [NSTextField labelWithString:@" "];
	NSProgressIndicator *bar = [[NSProgressIndicator alloc] init];
	NSStackView *stack;
	dispatch_semaphore_t finished = dispatch_semaphore_create(0);
	NSModalSession session;
	__block int succeeded = 0;

	/* (a link to a folder chosen before that is not usable: the copy takes
	its place) */
	if ([files destinationOfSymbolicLinkAtPath:maps error:NULL])
		[files removeItemAtPath:maps error:NULL];
	host_logf(HOST_LOG_INFO, "maps: extracting %s into %s", image.fileSystemRepresentation,
		maps.fileSystemRepresentation);
	window.title = @"Copying Halo’s Maps";
	label.lineBreakMode = NSLineBreakByTruncatingMiddle;
	detail.font = [NSFont monospacedDigitSystemFontOfSize:[NSFont smallSystemFontSize] weight:NSFontWeightRegular];
	detail.textColor = [NSColor secondaryLabelColor];
	bar.style = NSProgressIndicatorStyleBar;
	bar.indeterminate = NO;
	bar.minValue = 0.0;
	bar.maxValue = 1.0;
	stack = [NSStackView stackViewWithViews:@[ label, bar, detail ]];
	stack.orientation = NSUserInterfaceLayoutOrientationVertical;
	stack.alignment = NSLayoutAttributeLeading;
	stack.spacing = 10;
	stack.edgeInsets = NSEdgeInsetsMake(20, 20, 20, 20);
	[bar.widthAnchor constraintEqualToConstant:400].active = YES;
	[label.widthAnchor constraintLessThanOrEqualToConstant:400].active = YES;
	window.contentView = stack;
	[window center];
	atomic_store(&extract_done, 0);
	atomic_store(&extract_total, 0);
	error[0] = 0;
	session = [NSApp beginModalSessionForWindow:window];
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		succeeded = xiso_extract_maps(image.path.fileSystemRepresentation, destination.fileSystemRepresentation,
			extract_progress, NULL, error, sizeof(error));
		dispatch_semaphore_signal(finished);
	});
	for (;;)
	{
		unsigned long long done = atomic_load(&extract_done), total = atomic_load(&extract_total);

		if (total)
		{
			bar.doubleValue = (double)done / (double)total;
			detail.stringValue = [NSString stringWithFormat:@"%@ of %@",
				[NSByteCountFormatter stringFromByteCount:(long long)done countStyle:NSByteCountFormatterCountStyleFile],
				[NSByteCountFormatter stringFromByteCount:(long long)total countStyle:NSByteCountFormatterCountStyleFile]];
		}
		[NSApp runModalSession:session];
		if (!dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 20)))
			break;
	}
	[NSApp endModalSession:session];
	[window orderOut:nil];
	if (!succeeded)
	{
		host_logf(HOST_LOG_WARN, "maps: the extraction failed: %s", error);
		return error[0] ? [NSString stringWithUTF8String:error] : @"The maps could not be copied out of it.";
	}
	/* (the folder chosen before, if any, is no longer the game's) */
	[files removeItemAtPath:bookmark_path() error:NULL];
	return nil;
}

/* Quit while the app asks for the maps (before the game reads SDL's
events): at once */
static bool SDLCALL quit_watch(void *unused, SDL_Event *event)
{
	(void)unused;
	if (event->type == SDL_EVENT_QUIT)
	{
		host_logf(HOST_LOG_INFO, "quit before the game started");
		exit(EXIT_SUCCESS);
	}
	return true;
}

/* the maps folder the player chooses, or the one extracted from the disc
image they choose (*extracted, the data root's maps); quits if they choose
none */
static NSURL *maps_choose(BOOL *extracted)
{
	NSURL *chosen = nil;
	HaloMapsPanelDelegate *delegate = [HaloMapsPanelDelegate new];
	BOOL from_image = NO;

	/* (SDL's application: its menus, and Quit as the game's quit event) */
	SDL_InitSubSystem(SDL_INIT_VIDEO);
	SDL_AddEventWatch(quit_watch, NULL);
	if (getenv("HALO_MAPS_PANEL_ANSWER"))
	{
		test_answers = [[@(getenv("HALO_MAPS_PANEL_ANSWER")) componentsSeparatedByString:@";"] mutableCopy];
	}
	[NSApp activate];
	while (!chosen)
	{
		NSOpenPanel *panel = [NSOpenPanel openPanel];
		NSURL *answer = test_answer_panel();
		NSURL *picked;
		NSURL *maps;
		NSString *problem = nil;
		BOOL image;

		panel.canChooseDirectories = YES;
		panel.canChooseFiles = YES;
		panel.allowsMultipleSelection = NO;
		panel.delegate = delegate;
		panel.prompt = @"Use These Maps";
		panel.message = @"Choose the maps folder or the disc image (.iso or .xiso) of Halo: Combat Evolved for the "
			"Xbox, from your own copy of the game.\nA folder holds ui.map, bloodgulch.map and the other maps; it stays "
			"where it is. A disc image’s maps are copied into the game’s folder.";
		if ([panel runModal] != NSModalResponseOK || !(answer ?: panel.URL))
		{
			if (alert(NSAlertStyleInformational, @"Halo needs its maps to play",
					@"The game data is not part of the app: choose the maps folder or the disc image of your "
					"copy of Halo: Combat Evolved for the Xbox.", @[ @"Choose…", @"Quit" ]) ==
				NSAlertFirstButtonReturn)
			{
				continue;
			}
			host_logf(HOST_LOG_INFO, "no maps folder chosen: quitting");
			exit(EXIT_SUCCESS);
		}
		picked = answer ?: panel.URL;
		image = !is_folder(picked);
		if (image)
		{
			problem = maps_extract(picked);
			if (problem)
			{
				alert(NSAlertStyleWarning, @"The maps could not be copied from that disc image", problem,
					@[ @"Choose Another…" ]);
				continue;
			}
			picked = [NSURL fileURLWithPath:[support_folder stringByAppendingPathComponent:@"maps"]];
		}
		switch (maps_check(picked, &maps, &problem))
		{
		case _maps_good:
			chosen = maps;
			break;
		case _maps_incomplete:
			if (alert(NSAlertStyleWarning, @"Some maps are missing", [problem stringByAppendingString:
					@" The levels without their maps cannot be played."],
					@[ image ? @"Use These Maps" : @"Use This Folder", @"Choose Another…" ]) ==
				NSAlertFirstButtonReturn)
			{
				chosen = maps;
			}
			break;
		case _maps_unusable:
			alert(NSAlertStyleWarning, image ? @"That disc image cannot be used" : @"That folder cannot be used",
				problem, @[ @"Choose Another…" ]);
			break;
		}
		/* (a copy not taken is not kept) */
		if (image && !chosen)
			[[NSFileManager defaultManager] removeItemAtURL:picked error:NULL];
		from_image = image;
	}
	*extracted = from_image;
	SDL_RemoveEventWatch(quit_watch, NULL);
	return chosen;
}

/* the data root (support_folder) with its maps, asking for them if there
are none */
static void maps_find(void)
{
	NSURL *maps = maps_remembered();
	NSURL *found;
	NSString *problem = nil;
	NSURL *link = [NSURL fileURLWithPath:[support_folder stringByAppendingPathComponent:@"maps"]];
	NSNumber *symbolic = nil;

	/* a maps folder of its own, which the player put there */
	[link getResourceValue:&symbolic forKey:NSURLIsSymbolicLinkKey error:NULL];
	if ([[NSFileManager defaultManager] fileExistsAtPath:link.path] && !symbolic.boolValue)
	{
		host_logf(HOST_LOG_INFO, "maps: %s", link.fileSystemRepresentation);
		return;
	}
	if (maps && maps_check(maps, &found, &problem) == _maps_unusable)
	{
		host_logf(HOST_LOG_WARN, "maps: %s cannot be used: %s", maps.fileSystemRepresentation, problem.UTF8String);
		maps = nil;
	}
	else if (maps)
	{
		maps = found;
	}
	if (!maps)
	{
		BOOL extracted = NO;

		maps = maps_choose(&extracted);
		if (extracted)
		{
			host_logf(HOST_LOG_INFO, "maps: %s (from the disc image)", maps.fileSystemRepresentation);
			return;
		}
		maps_remember(maps);
	}
	host_logf(HOST_LOG_INFO, "maps: %s", maps.fileSystemRepresentation);
	maps_link(maps);
}

/* ---------- the window */

static NSString *window_path(void)
{
	return [support_folder stringByAppendingPathComponent:@"window.plist"];
}

static NSMutableDictionary *window_state;
static BOOL window_placed;
static unsigned long window_writes;

static BOOL is_game_window(NSWindow *window)
{
	return [window isKindOfClass:NSClassFromString(@"SDL3Window")];
}

static void window_save_soon(void)
{
	unsigned long write = ++window_writes;

	/* (once a moving or resizing window rests) */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
		if (write == window_writes)
			[window_state writeToFile:window_path() atomically:YES];
	});
}

static void window_frame_saved(NSWindow *window)
{
	if (!window_placed || !window.visible || (window.styleMask & NSWindowStyleMaskFullScreen))
		return;
	window_state[@"frame"] = NSStringFromRect(window.frame);
	window_save_soon();
}

/* whether the window is fullscreen (either kind), as SDL has it once the
window rests, kept for the next start: a window only when the player made
it one (F11, Toggle Full Screen). The Space's own notifications also come
when the game leaves it for display.mode's fullscreen, which keeps SDL's
flag, and when the window closes at quit; a NO from those made the next
start's default (display.mode empty) a window, where display.resolution
does not apply. */
static void window_fullscreen_saved(NSWindow *window)
{
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
		SDL_Window **windows = SDL_GetWindows(NULL);
		NSNumber *fullscreen = nil;
		int index;

		for (index = 0; windows && windows[index]; index++)
		{
			if (SDL_GetPointerProperty(SDL_GetWindowProperties(windows[index]), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
				NULL) == (__bridge void *)window)
			{
				fullscreen = SDL_GetWindowFlags(windows[index]) & SDL_WINDOW_FULLSCREEN ? @YES : @NO;
			}
		}
		SDL_free(windows);
		if (!fullscreen || !window.visible || [fullscreen isEqual:window_state[@"fullscreen"]])
			return;
		window_state[@"fullscreen"] = fullscreen;
		window_save_soon();
	});
}

/* the last frame, where a screen still shows it */
static void window_place(NSWindow *window)
{
	NSString *saved = window_state[@"frame"];
	NSRect frame;

	if (window_placed || (window.styleMask & NSWindowStyleMaskFullScreen))
		return;
	window_placed = YES;
	if (!saved)
		return;
	frame = NSRectFromString(saved);
	for (NSScreen *screen in [NSScreen screens])
	{
		if (NSIntersectsRect(NSInsetRect(frame, 40, 40), screen.visibleFrame))
		{
			[window setFrame:frame display:YES];
			return;
		}
	}
}

static void window_remember(void)
{
	NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
	NSOperationQueue *main = [NSOperationQueue mainQueue];
	NSNumber *fullscreen;

	window_state = [NSMutableDictionary dictionaryWithContentsOfFile:window_path()] ?: [NSMutableDictionary dictionary];
	fullscreen = window_state[@"fullscreen"];
	if (fullscreen)
		setenv("HALO_FULLSCREEN", fullscreen.boolValue ? "true" : "false", 0);
	/* (each after SDL's own handling of the change) */
	[center addObserverForName:NSWindowDidBecomeKeyNotification object:nil queue:main usingBlock:^(NSNotification *note) {
		NSWindow *window = note.object;

		if (is_game_window(window))
			dispatch_async(dispatch_get_main_queue(), ^{ window_place(window); });
	}];
	[center addObserverForName:NSWindowDidExitFullScreenNotification object:nil queue:main usingBlock:^(NSNotification *note) {
		NSWindow *window = note.object;

		if (!is_game_window(window))
			return;
		window_fullscreen_saved(window);
		dispatch_async(dispatch_get_main_queue(), ^{
			/* (a game that started fullscreen: the window now) */
			window_place(window);
			window_frame_saved(window);
		});
	}];
	[center addObserverForName:NSWindowDidEnterFullScreenNotification object:nil queue:main usingBlock:^(NSNotification *note) {
		if (is_game_window(note.object))
			window_fullscreen_saved(note.object);
	}];
	for (NSNotificationName name in @[ NSWindowDidMoveNotification, NSWindowDidResizeNotification ])
	{
		[center addObserverForName:name object:nil queue:main usingBlock:^(NSNotification *note) {
			if (is_game_window(note.object))
				window_frame_saved(note.object);
		}];
	}
}

/* ---------- updates (Sparkle) */

/* port/linux/host/host_desktop.c's: 1 while a game is played, 0 at the main
menu, -1 before the game's first frame (updater.c's updater_poll) */
int host_app_is_playing(void);

@interface HaloUpdater : NSObject <SPUUpdaterDelegate, SPUStandardUserDriverDelegate, NSMenuItemValidation>
@end

static SPUStandardUpdaterController *updater_controller;
static HaloUpdater *updater_delegate;
/* a new version found during a game, to be shown at the main menu */
static BOOL update_waiting;
/* the check of the start, made at the first main menu */
static BOOL update_start_checked;
/* config.toml's modification date when update.auto was last read */
static NSDate *update_config_read;

static NSString *config_path(void)
{
	return [support_folder stringByAppendingPathComponent:@"config.toml"];
}

static NSDate *config_modified(void)
{
	return [[NSFileManager defaultManager] attributesOfItemAtPath:config_path() error:NULL].fileModificationDate;
}

/* update.auto = on written to config.toml as the game writes a setting:
its line in [update] (or its commented default) replaced, else added. The
game reads the file again before it writes any setting, and keeps this */
static void config_write_update_auto(BOOL on)
{
	NSCharacterSet *blank = [NSCharacterSet whitespaceCharacterSet];
	NSString *text = [NSString stringWithContentsOfFile:config_path() encoding:NSUTF8StringEncoding error:NULL] ?: @"";
	NSMutableArray<NSString *> *lines = [[text componentsSeparatedByString:@"\n"] mutableCopy];
	NSString *line = on ? @"auto = true" : @"auto = false";
	NSUInteger section = NSNotFound, index;
	BOOL replaced = NO;

	for (index = 0; index < lines.count; index++)
	{
		NSString *trimmed = [lines[index] stringByTrimmingCharactersInSet:blank];

		if ([trimmed hasPrefix:@"["])
		{
			if (section != NSNotFound)
				break;
			if ([trimmed isEqualToString:@"[update]"])
				section = index;
			continue;
		}
		if (section == NSNotFound)
			continue;
		if ([trimmed hasPrefix:@"#"])
			trimmed = [[trimmed substringFromIndex:1] stringByTrimmingCharactersInSet:blank];
		if ([trimmed hasPrefix:@"auto"] &&
			[[[trimmed substringFromIndex:4] stringByTrimmingCharactersInSet:blank] hasPrefix:@"="])
		{
			lines[index] = line;
			replaced = YES;
			break;
		}
	}
	if (!replaced && section == NSNotFound)
	{
		if (lines.count && [lines.lastObject length] == 0)
			[lines removeLastObject];
		[lines addObjectsFromArray:@[ @"", @"[update]", line, @"" ]];
	}
	else if (!replaced)
		[lines insertObject:line atIndex:section + 1];
	if (![[lines componentsJoinedByString:@"\n"] writeToFile:config_path() atomically:YES encoding:NSUTF8StringEncoding
		error:NULL])
	{
		host_logf(HOST_LOG_WARN, "update: could not write update.auto to config.toml");
	}
}

/* Sparkle's windows over the game's when it is fullscreen (in its Space),
not in a Space of their own */
static void update_windows_over_game(void)
{
	dispatch_async(dispatch_get_main_queue(), ^{
		for (NSWindow *window in NSApp.windows)
		{
			if (is_game_window(window) || !window.visible)
				continue;
			window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenAuxiliary |
				NSWindowCollectionBehaviorMoveToActiveSpace;
			[window orderFrontRegardless];
		}
	});
}

@implementation HaloUpdater

/* (the appcast of a test: HALO_UPDATE_FEED, port/macos/README.md) */
- (NSString *)feedURLStringForUpdater:(SPUUpdater *)updater
{
	const char *feed = getenv("HALO_UPDATE_FEED");

	return feed && *feed ? [NSString stringWithUTF8String:feed] : nil;
}

- (void)updater:(SPUUpdater *)updater didFindValidUpdate:(SUAppcastItem *)item
{
	host_logf(HOST_LOG_INFO, "update: build %s is out", item.versionString.UTF8String);
}

- (void)updaterDidNotFindUpdate:(SPUUpdater *)updater
{
	host_logf(HOST_LOG_INFO, "update: this is the latest build");
}

- (void)updater:(SPUUpdater *)updater didAbortWithError:(NSError *)error
{
	host_logf(HOST_LOG_INFO, "update: %s", error.localizedDescription.UTF8String);
}

- (void)updaterWillRelaunchApplication:(SPUUpdater *)updater
{
	host_logf(HOST_LOG_INFO, "update: installed; starting the new build");
}

/* a version found by a check of Sparkle's own (not Check for Updates...):
shown by Sparkle at the main menu, else kept for it (update_tick) */
- (BOOL)supportsGentleScheduledUpdateReminders
{
	return YES;
}

- (BOOL)standardUserDriverShouldHandleShowingScheduledUpdate:(SUAppcastItem *)update andInImmediateFocus:(BOOL)immediateFocus
{
	return host_app_is_playing() == 0;
}

- (void)standardUserDriverWillHandleShowingUpdate:(BOOL)handleShowingUpdate forUpdate:(SUAppcastItem *)update
	state:(SPUUserUpdateState *)state
{
	if (handleShowingUpdate)
	{
		update_windows_over_game();
		return;
	}
	update_waiting = YES;
	host_logf(HOST_LOG_INFO, "update: build %s shown at the main menu, after this game",
		update.versionString.UTF8String);
}

- (void)standardUserDriverDidReceiveUserAttentionForUpdate:(SUAppcastItem *)update
{
	update_waiting = NO;
}

- (void)standardUserDriverWillFinishUpdateSession
{
	update_waiting = NO;
}

/* the app menu's Automatically Check for Updates: update.auto */
- (void)toggleAutomaticChecks:(NSMenuItem *)item
{
	BOOL on = !updater_controller.updater.automaticallyChecksForUpdates;

	updater_controller.updater.automaticallyChecksForUpdates = on;
	config_write_update_auto(on);
	update_config_read = config_modified();
	host_logf(HOST_LOG_INFO, "update: update.auto = %s", on ? "true" : "false");
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
	if (item.action == @selector(toggleAutomaticChecks:))
	{
		item.state = updater_controller.updater.automaticallyChecksForUpdates ? NSControlStateValueOn
			: NSControlStateValueOff;
	}
	return YES;
}

@end

/* every two seconds, on the main thread (between the game's frames, which
pump its run loop): update.auto as Settings changes it, the start's check
at the first main menu, and a version found during a game shown once the
game is over */
static void update_tick(void)
{
	SPUUpdater *updater = updater_controller.updater;
	NSDate *modified = config_modified();

	if (modified && ![modified isEqualToDate:update_config_read])
	{
		BOOL on = config_boolean_setting("update.auto", YES);

		update_config_read = modified;
		if (on != updater.automaticallyChecksForUpdates)
		{
			updater.automaticallyChecksForUpdates = on;
			host_logf(HOST_LOG_INFO, "update: update.auto = %s", on ? "true" : "false");
		}
	}
	if (host_app_is_playing() != 0)
		return;
	if (!update_start_checked)
	{
		update_start_checked = YES;
		if (updater.automaticallyChecksForUpdates && updater.canCheckForUpdates)
			[updater checkForUpdatesInBackground];
	}
	if (update_waiting)
	{
		update_waiting = NO;
		[updater_controller checkForUpdates:nil];
		update_windows_over_game();
	}
}

/* once SDL has made the app's menus (NSApplicationDidFinishLaunching):
Sparkle, if the app has it and an appcast (a release's), else nothing */
static void update_start(void)
{
	NSBundle *bundle = [NSBundle mainBundle];
	NSBundle *sparkle = [NSBundle bundleWithPath:[bundle.privateFrameworksPath
		stringByAppendingPathComponent:@"Sparkle.framework"]];
	BOOL test = getenv("HALO_UPDATE_FEED") && *getenv("HALO_UPDATE_FEED");
	NSMenu *menu = NSApp.mainMenu.itemArray.firstObject.submenu;
	NSError *error = nil;
	Class controller;

	if (updater_controller)
		return;
	if (![bundle objectForInfoDictionaryKey:@"SUPublicEDKey"] || (!test && ![bundle objectForInfoDictionaryKey:@"SUFeedURL"]))
	{
		host_logf(HOST_LOG_INFO, "update: not looked for (a release's app looks: this build has no appcast)");
		return;
	}
	if (!sparkle || ![sparkle loadAndReturnError:&error] || !(controller = NSClassFromString(@"SPUStandardUpdaterController")))
	{
		host_logf(HOST_LOG_WARN, "update: no Sparkle (%s)", error ? error.localizedDescription.UTF8String : "");
		return;
	}
	updater_delegate = [HaloUpdater new];
	updater_controller = [[controller alloc] initWithStartingUpdater:NO updaterDelegate:updater_delegate
		userDriverDelegate:updater_delegate];
	[updater_controller startUpdater];
	updater_controller.updater.automaticallyChecksForUpdates = config_boolean_setting("update.auto", YES);
	update_config_read = config_modified();
	host_logf(HOST_LOG_INFO, "update: Sparkle started (%s), update.auto = %s",
		updater_controller.updater.feedURL.absoluteString.UTF8String,
		updater_controller.updater.automaticallyChecksForUpdates ? "true" : "false");
	if (menu)
	{
		NSMenuItem *check = [[NSMenuItem alloc] initWithTitle:@"Check for Updates\u2026"
			action:@selector(checkForUpdates:) keyEquivalent:@""];
		NSMenuItem *automatic = [[NSMenuItem alloc] initWithTitle:@"Automatically Check for Updates"
			action:@selector(toggleAutomaticChecks:) keyEquivalent:@""];

		check.target = updater_controller;
		automatic.target = updater_delegate;
		[menu insertItem:[NSMenuItem separatorItem] atIndex:1];
		[menu insertItem:check atIndex:2];
		[menu insertItem:automatic atIndex:3];
	}
	[NSTimer scheduledTimerWithTimeInterval:2 repeats:YES block:^(NSTimer *timer) { update_tick(); }];
}

/* ---------- start */

void host_app_start(void)
{
	@autoreleasepool
	{
		NSBundle *bundle = [NSBundle mainBundle];
		NSFileManager *files = [NSFileManager defaultManager];
		NSURL *library = [files URLsForDirectory:NSLibraryDirectory inDomains:NSUserDomainMask].firstObject;
		NSURL *support = [[files URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject
			URLByAppendingPathComponent:APP_FOLDER];
		NSURL *logs = [[library URLByAppendingPathComponent:@"Logs"] URLByAppendingPathComponent:APP_FOLDER];
		NSString *brokers;
		NSString *saves;

		if (!bundle.bundleIdentifier || ![bundle.bundlePath.pathExtension isEqualToString:@"app"])
			return;
		[files createDirectoryAtURL:support withIntermediateDirectories:YES attributes:nil error:NULL];
		[files createDirectoryAtURL:logs withIntermediateDirectories:YES attributes:nil error:NULL];
		log_open(logs);
		support_folder = support.path;
		snprintf(settings_folder, sizeof(settings_folder), "%s/", support_folder.fileSystemRepresentation);
		host_logf(HOST_LOG_INFO, "%s %s (%s), %s", [[bundle objectForInfoDictionaryKey:@"CFBundleName"] UTF8String],
			[[bundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] UTF8String],
			bundle.bundleIdentifier.UTF8String, bundle.bundlePath.fileSystemRepresentation);
		host_logf(HOST_LOG_INFO, "settings and saves: %s", settings_folder);

		/* internet play's brokers (network.brokers_file), the app's list
		beside config.toml at each start, as an update replaces the one
		beside the desktop builds' executable */
		brokers = [bundle pathForResource:@"brokers" ofType:@"txt"];
		if (brokers)
		{
			NSString *copy = [support_folder stringByAppendingPathComponent:@"brokers.txt"];

			[files removeItemAtPath:copy error:NULL];
			[files copyItemAtPath:brokers toPath:copy error:NULL];
		}

		saves = [support_folder stringByAppendingPathComponent:@"saves"];
		if (!getenv("HALO_SAVE_ROOT") && !config_path_setting("paths.saves"))
			setenv("HALO_SAVE_ROOT", saves.fileSystemRepresentation, 1);
		if (!getenv("HALO_DATA_ROOT") && !config_path_setting("paths.data"))
		{
			setenv("HALO_DATA_ROOT", support_folder.fileSystemRepresentation, 1);
			maps_find();
		}
		window_remember();
		[[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidFinishLaunchingNotification object:nil
			queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification *note) {
				dispatch_async(dispatch_get_main_queue(), ^{ update_start(); });
			}];
	}
}
