/*
UPDATER.C

The desktop ports' self-updater (Linux and Windows; the Android app updates
itself in Java, port/android).

A build of the main branch made by GitHub Actions knows its build number
(HALO_BUILD_NUMBER, the workflow's run number, which names its release:
build-<number>); other builds have none and never look for updates. When
update.auto in config.toml is true (the default), the game asks GitHub for
the latest release when it starts, on a thread of its own: the game starts
meanwhile, and nothing happens if the release is not newer or cannot be
reached. If it is newer, the game asks whether to update:

- Yes: the release's build for this platform and configuration
  (halo-<platform>-<release|debug>.zip) is downloaded into the game's folder
  (update.partial/) and unpacked, its files put in place of the running
  game's (which become <name>.old, listed in update-old.txt and deleted at
  the next start), and the new game started; this one quits.
- No: nothing, until the next start.
- Do not ask again: after the player confirms it, update.auto = false is
  written to config.toml.

The system side (the HTTPS download, the files, starting the new game) is
update.h's: posix_update.c on Linux, win32_update.c on Windows.

On Linux the release is a tree (bin/openh1, lib/openh1, share/...:
install_layout.c, tools/linux_package.py), OpenH1-Linux[-debug].zip or
OpenH1-Linux-ARM64[-debug].zip (tools/release_names.py), and the game's folder
is its root. A game a package manager installed, or one in a folder the
player cannot write to, never looks for updates: its package manager
updates it; nor does a game outside a tree (a build's folder).

The Steam Frame's VR build (UPDATER_PACKAGE) is a package of its own in
~/apps/halo-vr (tools/frame_package.py), which Steam starts through its
launcher, halo-vr.sh. Started by it (HALO_FRAME_LAUNCHER), the game looks for
the latest release's OpenH1-Steam-Frame.zip as the others do, then
downloads and unpacks it in the background (update.partial/halo-vr-frame),
while the game plays. Once it is there, the game asks in its own menus, at
the main menu (a system dialog would show nowhere in the headset, and
stops the game under gamescope, where the Frame runs it):

- Update: update-ready is written beside the game, and the game quits; the
  launcher sees it, installs the package with the package's own install.sh
  (tools/linux_install.sh: its SHA256SUMS checked, each file put in place
  by a rename, the player's files untouched), and starts the new game.
- Not now: nothing, until the next start (the launcher deletes the
  download).

Settings > Network Setup > Check for Updates (update.auto) stops it
looking.
*/

#include "platform.h"
#include "install_layout.h"
#include "port_config.h"
#include "update.h"

/* the release's build that this one updates to (UPDATE_NAME[-debug].zip,
as tools/release_names.py names the release's files; on Linux, the release's
tree); a build without one never looks for updates */
#if defined(HALO_ANDROID) || defined(HALO_WEB) || defined(HALO_MACOS)
/* (none: the Android app updates itself, the site through its service
worker; the macOS app is replaced by its disk image) */
#elif defined(_WIN32)
#define UPDATE_PLATFORM "windows"
#define UPDATE_NAME "OpenH1-Windows"
#elif defined(HALO_VR) && defined(__x86_64__)
/* the 64-bit x86 VR build, which releases have beside the other one
(tools/ci_build.py --vr): it updates to itself, never to the other build */
#define UPDATE_PLATFORM "linux-x86_64-vr"
#define UPDATE_NAME "OpenH1-Linux-VR"
#elif defined(HALO_VR)
/* the 64-bit ARM VR build, the Steam Frame's: a package of its own
(tools/frame_package.py: OpenH1-Steam-Frame.zip, the game beside its
launcher and checksums, not a tree), which it updates to as the package's
install.sh installs it (UPDATER_PACKAGE, below), never to the desktop
build */
#define UPDATE_PLATFORM "steam-frame-vr"
#define UPDATE_NAME "OpenH1-Steam-Frame"
#define UPDATER_PACKAGE "halo-vr-frame"
#elif defined(__aarch64__)
/* the 64-bit ARM build (port/linux/host) */
#define UPDATE_PLATFORM "linux-arm64"
#define UPDATE_NAME "OpenH1-Linux-ARM64"
#elif defined(__x86_64__)
/* the 64-bit x86 build (port/linux/host, as the 64-bit ARM one) */
#define UPDATE_PLATFORM "linux-x86_64"
#define UPDATE_NAME "OpenH1-Linux"
#endif

#ifdef UPDATE_PLATFORM

#include "zlib_prefixed.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (given for this file by the build: tools/android_build.py's
generate_guest_image, windows_build.py) */
#ifndef HALO_BUILD_NUMBER
#define HALO_BUILD_NUMBER 0
#endif
#ifndef HALO_BUILD_FLAVOR
#define HALO_BUILD_FLAVOR "release"
#endif

#define UPDATE_REPOSITORY "OpenH1/OpenH1"
#ifdef _WIN32
#define PATH_SEPARATOR "\\"
#else
#define PATH_SEPARATOR "/"
#endif
/* the release's file (a debug build's has -debug after the name) */
#define UPDATE_ASSET_RELEASE UPDATE_NAME ".zip"
#define UPDATE_ASSET_DEBUG UPDATE_NAME "-debug.zip"
#define UPDATE_ASSET (strcmp(HALO_BUILD_FLAVOR, "release") ? UPDATE_ASSET_DEBUG : UPDATE_ASSET_RELEASE)
#define UPDATE_DIRECTORY "update.partial"
/* the files an update replaced (<name>.old), one on each line, which the new
version deletes */
#define UPDATE_OLD_LIST "update-old.txt"
#define MAXIMUM_UPDATE_FILES 64
/* the most a download may be: GitHub's answer about the latest release, a
release's zip (about 30 MB) */
#define MAXIMUM_CHECK_SIZE (1024ULL * 1024)
#define MAXIMUM_UPDATE_SIZE (256ULL * 1024 * 1024)

enum
{
	_updater_idle,
	_updater_checking,
	_updater_available,
	_updater_handled,
};

static SDL_AtomicInt updater_state;
static long updater_latest_build;
static char updater_directory[1024];
static char updater_executable[1024];

/* ---------- paths */

/* (a name's parts are separated by '/', as in the zip) */
static void updater_local_separators(char *path, size_t from)
{
	for (; path[from]; from++)
	{
		if (path[from] == '/')
			path[from] = PATH_SEPARATOR[0];
	}
}

static void updater_path(char *path, size_t size, const char *name)
{
	snprintf(path, size, "%s" PATH_SEPARATOR "%s", updater_directory, name);
	updater_local_separators(path, strlen(updater_directory));
}

static void updater_partial_path(char *path, size_t size, const char *name)
{
	snprintf(path, size, "%s" PATH_SEPARATOR UPDATE_DIRECTORY PATH_SEPARATOR "%s", updater_directory, name);
	updater_local_separators(path, strlen(updater_directory));
}

/* the folders a file at path is in, from the character at from on (those
before are there already); 1 when they all are */
static int updater_make_folders(const char *path, size_t from)
{
	char folder[1200];
	size_t index;

	snprintf(folder, sizeof(folder), "%s", path);
	for (index = from; folder[index]; index++)
	{
		if (folder[index] == PATH_SEPARATOR[0] && index > from)
		{
			folder[index] = 0;
			if (!update_make_directory(folder))
				return 0;
			folder[index] = PATH_SEPARATOR[0];
		}
	}
	return 1;
}

/* whether a name in the zip is a file inside the game's folder: relative,
its parts separated by '/', none of them empty, "." or ".." (nor a drive's
or a backslash) */
static int updater_name_inside(const char *name)
{
	const char *part = name;

	if (!*name || strchr(name, '\\') || strchr(name, ':'))
		return 0;
	for (;;)
	{
		size_t length = strcspn(part, "/");

		if (!length || (length == 1 && part[0] == '.') || (length == 2 && part[0] == '.' && part[1] == '.'))
			return 0;
		if (!part[length])
			return 1;
		part += length + 1;
	}
}

/* ---------- the zip file (the release's), stored or deflated entries */

static unsigned long zip_word(const unsigned char *bytes)
{
	return (unsigned long)bytes[0] | (unsigned long)bytes[1] << 8;
}

static unsigned long zip_long(const unsigned char *bytes)
{
	return zip_word(bytes) | zip_word(bytes + 2) << 16;
}

/* an entry's data, unpacked, to the file at path; why it could not be is
written to reason */
static int zip_extract_entry(SDL_IOStream *zip, unsigned long local_offset, int method,
	unsigned long packed_size, unsigned long size, unsigned long crc, const char *path,
	char *reason, size_t reason_size)
{
	unsigned char header[30];
	unsigned char input[16384], output[16384];
	SDL_IOStream *file;
	z_stream stream;
	unsigned long remaining = packed_size, written = 0, checksum = crc32(0L, Z_NULL, 0);
	int succeeded = 0, ended = 0;

	if (SDL_SeekIO(zip, (Sint64)local_offset, SDL_IO_SEEK_SET) < 0 ||
		SDL_ReadIO(zip, header, sizeof(header)) != sizeof(header) || zip_long(header) != 0x04034b50 ||
		SDL_SeekIO(zip, (Sint64)(zip_word(header + 26) + zip_word(header + 28)), SDL_IO_SEEK_CUR) < 0)
	{
		snprintf(reason, reason_size, "its header at %lu could not be read (%s)", local_offset, SDL_GetError());
		return 0;
	}
	file = SDL_IOFromFile(path, "wb");
	if (!file)
	{
		snprintf(reason, reason_size, "could not create %s (%s)", path, SDL_GetError());
		return 0;
	}
	memset(&stream, 0, sizeof(stream));
	if (method == 8 && inflateInit2(&stream, -MAX_WBITS) != Z_OK)
	{
		snprintf(reason, reason_size, "zlib would not start (%s)", stream.msg ? stream.msg : "no message");
		SDL_CloseIO(file);
		return 0;
	}
	for (;;)
	{
		size_t count = remaining < sizeof(input) ? remaining : sizeof(input);

		if (count && SDL_ReadIO(zip, input, count) != count)
		{
			snprintf(reason, reason_size, "the download ended %lu bytes early (%s)", remaining, SDL_GetError());
			break;
		}
		remaining -= (unsigned long)count;
		if (method == 0)
		{
			/* (no more than the entry says it holds) */
			if (count > size - written)
			{
				snprintf(reason, reason_size, "it unpacks to more than %lu bytes", size);
				break;
			}
			if (SDL_WriteIO(file, input, count) != count)
			{
				snprintf(reason, reason_size, "could not write %s after %lu bytes (%s)", path, written, SDL_GetError());
				break;
			}
			checksum = crc32(checksum, input, (uInt)count);
			written += (unsigned long)count;
		}
		else
		{
			int result = Z_OK;

			stream.next_in = input;
			stream.avail_in = (uInt)count;
			do
			{
				size_t produced;

				stream.next_out = output;
				stream.avail_out = sizeof(output);
				result = inflate(&stream, Z_NO_FLUSH);
				if (result != Z_OK && result != Z_STREAM_END)
					break;
				produced = sizeof(output) - stream.avail_out;
				/* (no more than the entry says, before it is on the disk) */
				if (written + (unsigned long)produced > size)
				{
					snprintf(reason, reason_size, "it unpacks to more than %lu bytes", size);
					result = Z_ERRNO;
					break;
				}
				if (SDL_WriteIO(file, output, produced) != produced)
				{
					snprintf(reason, reason_size, "could not write %s after %lu bytes (%s)", path, written, SDL_GetError());
					result = Z_ERRNO;
					break;
				}
				checksum = crc32(checksum, output, (uInt)produced);
				written += (unsigned long)produced;
			} while (stream.avail_out == 0 && result != Z_STREAM_END);
			if (result == Z_STREAM_END)
				ended = 1;
			else if (result != Z_OK && result != Z_BUF_ERROR)
			{
				if (result != Z_ERRNO)
				{
					snprintf(reason, reason_size, "zlib stopped with %d after %lu of %lu bytes (%s)", result, written, size,
						stream.msg ? stream.msg : "no message");
				}
				break;
			}
		}
		/* (all of the input unpacked is enough, as the size and the CRC are
		checked: the game's zlib 1.1, used here before, could want a byte past
		the end of a raw stream before it said the stream had ended) */
		if (ended || !remaining)
		{
			if (written != size)
				snprintf(reason, reason_size, "it unpacked to %lu bytes, not %lu", written, size);
			else if (checksum != crc)
				snprintf(reason, reason_size, "its CRC is %08lx, not %08lx", checksum, crc);
			else
				succeeded = 1;
			break;
		}
	}
	if (method == 8)
		inflateEnd(&stream);
	if (!SDL_CloseIO(file) && succeeded)
	{
		snprintf(reason, reason_size, "could not finish writing %s (%s)", path, SDL_GetError());
		succeeded = 0;
	}
	return succeeded;
}

/* the zip's files (the Linux release's tree, or the Windows release's folder) into
update.partial/, their names in names */
static int zip_extract(SDL_IOStream *zip, char names[][256], int *name_count, char *error, size_t error_size)
{
	unsigned char tail[65536 + 22];
	Sint64 size;
	size_t tail_size, index;
	unsigned long entries = 0, directory_offset = 0, entry;
	int found = 0, succeeded = 0;

	*name_count = 0;
	/* the end of the central directory, in the last 64 KB */
	size = SDL_GetIOSize(zip);
	tail_size = size < (Sint64)sizeof(tail) ? (size_t)size : sizeof(tail);
	if (size < 22 || SDL_SeekIO(zip, size - (Sint64)tail_size, SDL_IO_SEEK_SET) < 0 ||
		SDL_ReadIO(zip, tail, tail_size) != tail_size)
	{
		goto done;
	}
	for (index = tail_size - 22 + 1; index-- > 0;)
	{
		if (zip_long(tail + index) == 0x06054b50)
		{
			entries = zip_word(tail + index + 10);
			directory_offset = zip_long(tail + index + 16);
			found = 1;
			break;
		}
	}
	if (!found || SDL_SeekIO(zip, (Sint64)directory_offset, SDL_IO_SEEK_SET) < 0)
		goto done;
	for (entry = 0; entry < entries; entry++)
	{
		unsigned char header[46];
		char name[256];
		unsigned long name_length, extra_length, comment_length;
		Sint64 next;
		char path[1200];
		char reason[512] = "";

		if (SDL_ReadIO(zip, header, sizeof(header)) != sizeof(header) || zip_long(header) != 0x02014b50)
			goto done;
		name_length = zip_word(header + 28);
		extra_length = zip_word(header + 30);
		comment_length = zip_word(header + 32);
		if (name_length >= sizeof(name) || SDL_ReadIO(zip, name, name_length) != name_length)
			goto done;
		name[name_length] = 0;
		next = SDL_TellIO(zip) + (Sint64)(extra_length + comment_length);
		/* (a folder, a link, or a name that would leave the game's folder,
		is left out; a name with a NUL in it is no zip of a release's) */
		if (memchr(name, 0, name_length))
			goto done;
		if (name_length && name[name_length - 1] != '/' && updater_name_inside(name) &&
			!(header[5] == 3 && ((zip_long(header + 38) >> 16) & 0170000) == 0120000))
		{
			int method = (int)zip_word(header + 10);

			if (*name_count >= MAXIMUM_UPDATE_FILES)
			{
				snprintf(error, error_size, "the download has more than %d files", MAXIMUM_UPDATE_FILES);
				goto done;
			}
			if (method != 0 && method != 8)
			{
				snprintf(error, error_size, "the download packs %s in a way this build cannot read", name);
				goto done;
			}
			updater_partial_path(path, sizeof(path), name);
			if (!updater_make_folders(path, strlen(updater_directory) + 1) ||
				!zip_extract_entry(zip, zip_long(header + 42), method, zip_long(header + 20), zip_long(header + 24),
				zip_long(header + 16), path, reason, sizeof(reason)))
			{
				snprintf(error, error_size, "could not unpack %s: %s", name, reason[0] ? reason : "no folder for it");
				goto done;
			}
			/* (executable on Unix, as the zip made there says: bin/openh1) */
			if (header[5] == 3 && ((zip_long(header + 38) >> 16) & 0111) && !update_make_executable(path))
			{
				snprintf(error, error_size, "could not make %s executable", name);
				goto done;
			}
			snprintf(names[*name_count], 256, "%s", name);
			(*name_count)++;
		}
		if (SDL_SeekIO(zip, next, SDL_IO_SEEK_SET) < 0)
			goto done;
	}
	succeeded = *name_count > 0;

done:
	if (!succeeded && !error[0])
		snprintf(error, error_size, "the download is not a zip file this build can read");
	return succeeded;
}

/* ---------- checking */

/* the build number of GitHub's latest release, 0 if there is none */
static long updater_latest_release(void)
{
	char path[1200];
	char error[512] = "";
	size_t size = 0;
	char *text;
	const char *tag;
	long build = 0;

	updater_path(path, sizeof(path), "update-check.json");
	if (!update_download("https://api.github.com/repos/" UPDATE_REPOSITORY "/releases/latest", path,
		MAXIMUM_CHECK_SIZE, NULL, NULL, error, sizeof(error)))
	{
		platform_log("update: could not check for a new version: %s", error);
		return 0;
	}
	text = SDL_LoadFile(path, &size);
	update_delete_file(path);
	if (!text)
		return 0;
	/* "tag_name": "build-<number>" */
	tag = strstr(text, "\"tag_name\"");
	if (tag)
	{
		tag = strchr(tag + 10, '"');
		if (tag && !strncmp(tag, "\"build-", 7))
			build = strtol(tag + 7, NULL, 10);
	}
	SDL_free(text);
	return build;
}

#ifdef UPDATER_PACKAGE
static int updater_unpack(const char *zip_path, int downloaded, char names[][256], int *name_count, char *error,
	size_t error_size);

/* the Frame's: the release's package downloaded (or debug.update_from's zip)
and unpacked into update.partial/, where the launcher's install takes it
(update.partial/halo-vr-frame, with its install.sh, SHA256SUMS and
launcher); 1 when it is there */
static int updater_prepare_package(void)
{
	static char names[MAXIMUM_UPDATE_FILES][256];
	static const char *const needed[] = { "install.sh", "SHA256SUMS", "halo-vr.sh", "halo" };
	char partial[1200], zip_path[1200], url[512], error[512] = "";
	const char *local_zip = config_string("debug.update_from");
	int name_count = 0;
	size_t index;

	updater_path(partial, sizeof(partial), UPDATE_DIRECTORY);
	if (!update_make_directory(partial))
	{
		platform_log("update: could not make %s (is the game's folder read-only?)", partial);
		return 0;
	}
	if (local_zip[0])
	{
		snprintf(zip_path, sizeof(zip_path), "%s", local_zip);
	}
	else
	{
		updater_partial_path(zip_path, sizeof(zip_path), UPDATE_ASSET);
		snprintf(url, sizeof(url), "https://github.com/" UPDATE_REPOSITORY "/releases/download/build-%ld/%s",
			updater_latest_build, UPDATE_ASSET);
		platform_log("update: downloading build %ld (%s)", updater_latest_build, UPDATE_ASSET);
		if (!update_download(url, zip_path, MAXIMUM_UPDATE_SIZE, NULL, NULL, error, sizeof(error)))
		{
			platform_log("update: could not download build %ld: %s", updater_latest_build, error);
			update_delete_file(zip_path);
			return 0;
		}
	}
	if (!updater_unpack(zip_path, !local_zip[0], names, &name_count, error, sizeof(error)))
	{
		platform_log("update: could not unpack build %ld: %s", updater_latest_build, error);
		return 0;
	}
	for (index = 0; index < sizeof(needed) / sizeof(needed[0]); index++)
	{
		char path[1200];

		updater_partial_path(path, sizeof(path), UPDATER_PACKAGE "/");
		snprintf(path + strlen(path), sizeof(path) - strlen(path), "%s", needed[index]);
		if (!SDL_GetPathInfo(path, NULL))
		{
			platform_log("update: build %ld's download is not the Steam Frame's package (no " UPDATER_PACKAGE "/%s)",
				updater_latest_build, needed[index]);
			return 0;
		}
	}
	platform_log("update: build %ld is ready to install (%d files in " UPDATE_DIRECTORY "/" UPDATER_PACKAGE ")",
		updater_latest_build, name_count);
	return 1;
}
#endif

static int SDLCALL updater_check_thread(void *context)
{
	/* (a test's zip, debug.update_from, is always newer) */
	long latest = config_string("debug.update_from")[0] ? HALO_BUILD_NUMBER + 1 : updater_latest_release();

	(void)context;
	if (latest > HALO_BUILD_NUMBER)
	{
		platform_log("update: build %ld is available (this is build %d)", latest, HALO_BUILD_NUMBER);
		updater_latest_build = latest;
#ifdef UPDATER_PACKAGE
		/* (the Frame's: asked about once it is ready to install) */
		if (!updater_prepare_package())
		{
			SDL_SetAtomicInt(&updater_state, _updater_handled);
			return 0;
		}
#endif
		SDL_SetAtomicInt(&updater_state, _updater_available);
	}
	else
	{
		if (latest)
			platform_log("update: this is the latest build (%d)", HALO_BUILD_NUMBER);
		SDL_SetAtomicInt(&updater_state, _updater_handled);
	}
	return 0;
}

/* ---------- updating */

/* (the release's files put in place by the game, but the Frame's, which its
launcher installs) */
#ifndef UPDATER_PACKAGE
struct updater_download
{
	SDL_Mutex *lock;
	char url[512];
	char zip_path[1200];
	unsigned long long received, total;
	int finished, succeeded;
	char error[512];
};

static void updater_download_progress(void *context, unsigned long long received, unsigned long long total)
{
	struct updater_download *download = context;

	SDL_LockMutex(download->lock);
	download->received = received;
	download->total = total;
	SDL_UnlockMutex(download->lock);
}

static int SDLCALL updater_download_thread(void *context)
{
	struct updater_download *download = context;
	char error[512] = "";
	int succeeded = update_download(download->url, download->zip_path, MAXIMUM_UPDATE_SIZE, updater_download_progress,
		download, error, sizeof(error));

	SDL_LockMutex(download->lock);
	download->succeeded = succeeded;
	snprintf(download->error, sizeof(download->error), "%s", error);
	download->finished = 1;
	SDL_UnlockMutex(download->lock);
	return 0;
}

/* downloads the release's zip, showing how far it has got in a window of its
own (drawn in software, clear of the game's OpenGL); 1 when it is there */
static int updater_download_zip(const char *zip_path, char *error, size_t error_size)
{
	static struct updater_download download;
	SDL_Window *window;
	SDL_Renderer *renderer = NULL;
	SDL_Thread *thread;
	int finished = 0;

	memset(&download, 0, sizeof(download));
	download.lock = SDL_CreateMutex();
	snprintf(download.url, sizeof(download.url),
		"https://github.com/" UPDATE_REPOSITORY "/releases/download/build-%ld/%s", updater_latest_build, UPDATE_ASSET);
	snprintf(download.zip_path, sizeof(download.zip_path), "%s", zip_path);
	thread = SDL_CreateThread(updater_download_thread, "update download", &download);
	if (!thread)
	{
		SDL_DestroyMutex(download.lock);
		snprintf(error, error_size, "could not start the download");
		return 0;
	}
	window = SDL_CreateWindow("Halo", 640, 150, 0);
	if (window)
		renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
	while (!finished)
	{
		SDL_Event event;
		unsigned long long received, total;

		/* (the game's own events wait: its window is not drawn meanwhile) */
		while (SDL_PollEvent(&event))
		{
		}
		SDL_LockMutex(download.lock);
		finished = download.finished;
		received = download.received;
		total = download.total;
		SDL_UnlockMutex(download.lock);
		if (renderer)
		{
			char line[160];
			SDL_FRect bar = { 20.0f, 100.0f, 600.0f, 24.0f };
			float fraction = total ? (float)((double)received / (double)total) : 0.0f;

			SDL_SetRenderDrawColor(renderer, 12, 16, 20, 255);
			SDL_RenderClear(renderer);
			SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255);
			SDL_SetRenderScale(renderer, 2.0f, 2.0f);
			SDL_RenderDebugText(renderer, 10.0f, 10.0f, "Downloading the new version...");
			SDL_SetRenderScale(renderer, 1.0f, 1.0f);
			snprintf(line, sizeof(line), "build %ld  (%llu of %llu MB)", updater_latest_build, received >> 20,
				total >> 20);
			SDL_RenderDebugText(renderer, 20.0f, 70.0f, line);
			SDL_SetRenderDrawColor(renderer, 60, 66, 72, 255);
			SDL_RenderFillRect(renderer, &bar);
			bar.w *= fraction;
			SDL_SetRenderDrawColor(renderer, 90, 160, 90, 255);
			SDL_RenderFillRect(renderer, &bar);
			SDL_RenderPresent(renderer);
		}
		SDL_Delay(16);
	}
	SDL_WaitThread(thread, NULL);
	if (renderer)
		SDL_DestroyRenderer(renderer);
	if (window)
		SDL_DestroyWindow(window);
	SDL_DestroyMutex(download.lock);
	if (!download.succeeded)
		snprintf(error, error_size, "%s", download.error);
	return download.succeeded;
}

/* a file replaced, <name>.old, into update-old.txt */
static void updater_list_old(SDL_IOStream *old_list, const char *name)
{
	char line[300];
	int length = snprintf(line, sizeof(line), "%s.old\n", name);

	if (old_list && length > 0 && length < (int)sizeof(line))
		SDL_WriteIO(old_list, line, (size_t)length);
}

/* update.partial/, once its files are in place: its folders, the deepest
first */
static void updater_delete_partial(const char *partial, char names[][256], int name_count)
{
	int index;

	for (index = 0; index < name_count; index++)
	{
		char name[256];
		char *slash;

		snprintf(name, sizeof(name), "%s", names[index]);
		while ((slash = strrchr(name, '/')) != NULL)
		{
			char path[1200];

			*slash = 0;
			updater_partial_path(path, sizeof(path), name);
			update_delete_file(path);
		}
	}
	update_delete_file(partial);
}

#endif

/* the zip, no larger than a release's can be, unpacked; a download is
deleted either way (a test's zip, debug.update_from, is kept) */
static int updater_unpack(const char *zip_path, int downloaded, char names[][256], int *name_count, char *error,
	size_t error_size)
{
	SDL_IOStream *stream = SDL_IOFromFile(zip_path, "rb");
	Sint64 size = stream ? SDL_GetIOSize(stream) : -1;
	int succeeded = 0;

	/* (read from the file, not from a copy in memory: a guest's SDL bridges
	no SDL_IOFromConstMem, guest_sdl.c) */
	if (size < 0 || (unsigned long long)size > MAXIMUM_UPDATE_SIZE)
		snprintf(error, error_size, "could not read the download");
	else
		succeeded = zip_extract(stream, names, name_count, error, error_size);
	if (stream)
		SDL_CloseIO(stream);
	if (downloaded)
		update_delete_file(zip_path);
	return succeeded;
}

#ifndef UPDATER_PACKAGE
/* the first count of the new files, already in place, taken back out (to
the update folder) and the old ones (<name>.old) put back, the last first:
the install as it was before the update. A new file that had no old one is
deleted */
static void updater_put_back(char names[][256], int count)
{
	int index, failures = 0;

	for (index = count - 1; index >= 0; index--)
	{
		char path[1200], new_path[1200], old_path[1300];

		updater_path(path, sizeof(path), names[index]);
		updater_partial_path(new_path, sizeof(new_path), names[index]);
		snprintf(old_path, sizeof(old_path), "%s.old", path);
		if (SDL_GetPathInfo(old_path, NULL))
		{
			if (!update_replace_file(path, old_path, new_path))
			{
				platform_log("update: could not put back %s", names[index]);
				failures++;
			}
		}
		else
		{
			update_delete_file(path);
		}
	}
	if (failures)
		platform_log("update: %d of %d replaced files could not be put back", failures, count);
	else
		platform_log("update: the %d replaced files put back: the install is as it was", count);
}

/* downloads, unpacks and puts in place the new build, and starts it; returns
only if something failed */
static void updater_update(void)
{
	char names[MAXIMUM_UPDATE_FILES][256];
	char zip_path[1200], partial[1200], list_path[1200], error[512] = "";
	const char *local_zip = config_string("debug.update_from");
	int name_count = 0, index;

	updater_path(partial, sizeof(partial), UPDATE_DIRECTORY);
	if (local_zip[0])
		snprintf(zip_path, sizeof(zip_path), "%s", local_zip);
	else
		updater_partial_path(zip_path, sizeof(zip_path), UPDATE_ASSET);
	platform_log("update: %s build %ld (%s)", local_zip[0] ? "unpacking" : "downloading", updater_latest_build,
		local_zip[0] ? local_zip : UPDATE_ASSET);
	if (!update_make_directory(partial))
	{
		snprintf(error, sizeof(error), "could not make %s (is the game's folder read-only?)", partial);
	}
	else if ((local_zip[0] || updater_download_zip(zip_path, error, sizeof(error))) &&
		updater_unpack(zip_path, !local_zip[0], names, &name_count, error, sizeof(error)))
	{
		/* (the files replaced, for the new version to delete) */
		SDL_IOStream *old_list;
		char launch[1200];

		updater_path(list_path, sizeof(list_path), UPDATE_OLD_LIST);
		old_list = SDL_IOFromFile(list_path, "ab");
		/* the new files in place of the old ones */
		for (index = 0; index < name_count; index++)
		{
			char path[1200], new_path[1200], old_path[1300];

			updater_path(path, sizeof(path), names[index]);
			updater_partial_path(new_path, sizeof(new_path), names[index]);
			snprintf(old_path, sizeof(old_path), "%s.old", path);
			if (!updater_make_folders(path, strlen(updater_directory) + 1) ||
				!update_replace_file(path, new_path, old_path))
			{
				snprintf(error, sizeof(error), "could not replace %s", path);
				break;
			}
			updater_list_old(old_list, names[index]);
		}
		/* (one could not be: the ones before it put back as they were, and
		the list of them, which is of no update now, deleted) */
		if (index < name_count && index > 0)
			updater_put_back(names, index);
		snprintf(launch, sizeof(launch), "%s", updater_executable);
#ifndef _WIN32
		/* (the tree's game: install_layout.c) */
		if (index == name_count)
			updater_path(launch, sizeof(launch), "bin/" INSTALL_NAME);
#endif
		if (old_list)
			SDL_CloseIO(old_list);
		if (index < name_count)
			update_delete_file(list_path);
		if (index == name_count)
		{
			updater_delete_partial(partial, names, name_count);
			platform_log("update: starting build %ld", updater_latest_build);
			if (update_launch(launch))
				exit(EXIT_SUCCESS);
			snprintf(error, sizeof(error), "the new version is in place, but could not be started: start it again");
		}
	}
	platform_log("update: failed: %s", error);
	{
		char message[800];

		snprintf(message, sizeof(message), "The update failed:\n\n%s", error);
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Halo", message, NULL);
	}
}

/* the files a previous update left behind, which update-old.txt lists, and
the notice of extract-xiso, which releases carried until the disc image
readers were written again; 1 when the list was there (this is the start
that follows an update) */
static int updater_clean_up(void)
{
	/* (the Windows folder's; the Linux tree's, with the CC0 licence the
	releases before the AGPL had, under the names of the trees then and now:
	tools/linux_package.py) */
	static const char *const notices[] =
	{
#ifdef _WIN32
		"extract-xiso-LICENSE.txt",
#else
		"share/licenses/opence/extract-xiso-LICENSE.txt", "share/licenses/opence/LICENSE.md",
		"share/licenses/" INSTALL_NAME "/extract-xiso-LICENSE.txt",
		"share/licenses/" INSTALL_NAME "/LICENSE.md",
#endif
	};
	size_t index;
	char path[1200];
	size_t size = 0;
	char *list;

	for (index = 0; index < sizeof(notices) / sizeof(notices[0]); index++)
	{
		updater_path(path, sizeof(path), notices[index]);
		update_delete_file(path);
	}
	updater_path(path, sizeof(path), UPDATE_OLD_LIST);
	list = SDL_LoadFile(path, &size);
	if (list)
	{
		char *line = list;

		while (*line)
		{
			size_t length = strcspn(line, "\r\n");
			char name[256];

			if (length && length < sizeof(name))
			{
				memcpy(name, line, length);
				name[length] = 0;
				/* (only <name>.old inside the game's folder) */
				if (length > 4 && !strcmp(name + length - 4, ".old") && updater_name_inside(name))
				{
					char old_path[1200];

					updater_path(old_path, sizeof(old_path), name);
					update_delete_file(old_path);
				}
			}
			line += length;
			line += strspn(line, "\r\n");
		}
		SDL_free(list);
		update_delete_file(path);
	}
	updater_partial_path(path, sizeof(path), UPDATE_ASSET);
	update_delete_file(path);
	updater_path(path, sizeof(path), UPDATE_DIRECTORY);
	update_delete_file(path);
	return list != NULL;
}
#endif

/* ---------- the game's */

/* at start-up (sdl_platform.c): looks for a new version, in the background */
void updater_start(void)
{
	char *slash;
	int updated;

	if (!update_executable_path(updater_executable, sizeof(updater_executable)))
		return;
#ifdef _WIN32
	snprintf(updater_directory, sizeof(updater_directory), "%s", updater_executable);
	slash = strrchr(updater_directory, PATH_SEPARATOR[0]);
	if (!slash)
		return;
	*slash = 0;
#else
	/* (the tree's root; not a game a package manager installed, which it
	updates, nor one outside a tree, a build's: install_layout.c) */
	(void)slash;
	if (install_is_system())
	{
		if (HALO_BUILD_NUMBER > 0)
			platform_log("update: not looking for updates: the game is installed (%s)", install_system_reason());
		return;
	}
#ifdef UPDATER_PACKAGE
	/* (the Frame's package's folder, whose launcher, which says it started
	the game, installs an update once the game has quit; HALO_ as the host
	passes only those to the game, port/linux/host/host_main.c) */
	snprintf(updater_directory, sizeof(updater_directory), "%s", install_folder());
	if (!updater_directory[0])
		return;
	{
		char launcher[1200], sums[1200];
		const char *started_by = getenv("HALO_FRAME_LAUNCHER");

		updater_path(launcher, sizeof(launcher), "halo-vr.sh");
		updater_path(sums, sizeof(sums), "SHA256SUMS");
		if (!started_by || !started_by[0] || !SDL_GetPathInfo(launcher, NULL) || !SDL_GetPathInfo(sums, NULL))
		{
			if (HALO_BUILD_NUMBER > 0)
				platform_log("update: not looking for updates: the game was not started by the Steam Frame "
					"package's halo-vr.sh");
			return;
		}
	}
	/* (not again just after the launcher installed one, or tried to) */
	{
		const char *just_updated = getenv("HALO_FRAME_UPDATED");

		updated = just_updated && just_updated[0];
		if (updated)
		{
			platform_log("update: not looked for: halo-vr.sh has just installed an update (or tried to)");
			return;
		}
	}
#else
	if (!install_is_tree())
	{
		if (HALO_BUILD_NUMBER > 0)
			platform_log("update: not looking for updates: the game is not in a release's tree (bin/" INSTALL_NAME ")");
		return;
	}
	snprintf(updater_directory, sizeof(updater_directory), "%s", install_folder());
	if (!updater_directory[0])
		return;
#endif
#endif
#ifndef UPDATER_PACKAGE
	updated = updater_clean_up();
#endif
	/* (not for builds without a number, unless a test names a zip (not
	again in the version it started), the player's no, or runs nobody is
	watching, but for a test with its answer) */
	if (config_string("debug.update_from")[0] && updated)
		return;
	if (HALO_BUILD_NUMBER <= 0 && !config_string("debug.update_from")[0])
	{
		platform_log("update: not looked for: only the builds CI numbers update themselves (this one has no "
			"number)");
		return;
	}
	if (!config_boolean("update.auto"))
	{
		platform_log("update: not looked for: update.auto is false");
		return;
	}
	if (!config_string("debug.update_answer")[0] && (config_boolean("debug.hidden_window") ||
		config_real("debug.exit_after") > 0.0 || config_string("debug.network_test")[0]))
	{
		return;
	}
	SDL_SetAtomicInt(&updater_state, _updater_checking);
	{
		SDL_Thread *thread = SDL_CreateThread(updater_check_thread, "update check", NULL);

		if (thread)
			SDL_DetachThread(thread);
		else
			SDL_SetAtomicInt(&updater_state, _updater_handled);
	}
}

#ifdef UPDATER_PACKAGE
/* menu_functions.c's: the update question opened at the main menu, once it
has been seen; 1 when it was, or cannot be */
int pc_menu_update_question_open(void);
void platform_request_quit(void);
void updater_update_now(void);

/* every frame, on the game's thread (sdl_platform.c): the Frame's asks in
the game's menus once the new version is ready to install */
void updater_poll(SDL_Window *window)
{
	const char *test_answer = config_string("debug.update_answer");

	(void)window;
	if (SDL_GetAtomicInt(&updater_state) != _updater_available)
		return;
	/* (an automated test's answer: debug.update_answer) */
	if (test_answer[0])
	{
		SDL_SetAtomicInt(&updater_state, _updater_handled);
		platform_log("update: answering %s (debug.update_answer)", test_answer);
		if (!strcmp(test_answer, "yes"))
			updater_update_now();
		else if (!strcmp(test_answer, "never"))
			config_write_boolean("update.auto", 0);
		return;
	}
	if (!pc_menu_update_question_open())
		return;
	SDL_SetAtomicInt(&updater_state, _updater_handled);
	platform_log("update: asking about build %ld at the main menu", updater_latest_build);
}

/* "port update now" (the update question's UPDATE: menu_functions.c): the
game quits, and its launcher installs the update (update-ready says so)
and starts the new game */
void updater_update_now(void)
{
	char path[1200], line[64];
	SDL_IOStream *ready;
	int length;

	updater_path(path, sizeof(path), "update-ready");
	ready = SDL_IOFromFile(path, "wb");
	length = snprintf(line, sizeof(line), "build %ld\n", updater_latest_build);
	if (!ready || SDL_WriteIO(ready, line, (size_t)length) != (size_t)length)
	{
		platform_log("update: could not write %s (%s)", path, SDL_GetError());
		if (ready)
			SDL_CloseIO(ready);
		return;
	}
	SDL_CloseIO(ready);
	platform_log("update: quitting, for halo-vr.sh to install build %ld", updater_latest_build);
	platform_request_quit();
}
#else
/* whether the game runs under gamescope (the Steam Deck's Game Mode), where
a system dialog such as SDL's message box crashes the game. gamescope sets
GAMESCOPE_WAYLAND_DISPLAY for the programs it runs and Game Mode's session
XDG_CURRENT_DESKTOP; Steam sets SteamDeck and SteamGamepadUI there. Wine
passes them on to the Windows build under Proton. */
static int updater_gamescope(void)
{
	const char *wayland_display = getenv("GAMESCOPE_WAYLAND_DISPLAY");
	const char *desktop = getenv("XDG_CURRENT_DESKTOP");
	const char *steam_deck = getenv("SteamDeck");
	const char *gamepad_ui = getenv("SteamGamepadUI");

	return (wayland_display && wayland_display[0]) || (desktop && strstr(desktop, "gamescope")) ||
		(steam_deck && !strcmp(steam_deck, "1") && gamepad_ui && !strcmp(gamepad_ui, "1"));
}

/* every frame, on the game's thread (sdl_platform.c): asks the player once a
new version is found */
void updater_poll(SDL_Window *window)
{
	static const SDL_MessageBoxButtonData question_buttons[] =
	{
		{ SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes" },
		{ SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No" },
		{ 0, 2, "Do not ask again" },
	};
	static const SDL_MessageBoxButtonData confirm_buttons[] =
	{
		{ SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes" },
		{ SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No" },
	};
	char message[400];
	int answer = 0;
	int fullscreen;
	const char *test_answer = config_string("debug.update_answer");

	if (SDL_GetAtomicInt(&updater_state) != _updater_available)
		return;
	SDL_SetAtomicInt(&updater_state, _updater_handled);
	/* (an automated test's answer: debug.update_answer) */
	if (test_answer[0])
	{
		platform_log("update: answering %s (debug.update_answer)", test_answer);
		if (!strcmp(test_answer, "yes"))
			updater_update();
		else if (!strcmp(test_answer, "never"))
			config_write_boolean("update.auto", 0);
		return;
	}
	/* (no question under gamescope: the player updates from the desktop, or
	with debug.update_answer) */
	if (updater_gamescope())
	{
		platform_log("update: build %ld is out (this is build %d); not asking under gamescope (Steam Deck Game Mode)",
			updater_latest_build, HALO_BUILD_NUMBER);
		return;
	}
	/* (a dialog cannot show above a fullscreen game) */
	fullscreen = window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
	if (fullscreen)
		SDL_SetWindowFullscreen(window, false);
	snprintf(message, sizeof(message),
		"A new version of Halo was detected (build %ld; this is build %d).\n\n"
		"Do you want to update? The game will close and start the new version.",
		updater_latest_build, HALO_BUILD_NUMBER);
	{
		SDL_MessageBoxData question = { SDL_MESSAGEBOX_INFORMATION, window, "Halo: new version", message,
			3, question_buttons, NULL };

		if (!SDL_ShowMessageBox(&question, &answer))
			answer = 0;
	}
	if (answer == 2)
	{
		SDL_MessageBoxData confirm = { SDL_MESSAGEBOX_WARNING, window, "Halo: new version",
			"Stop asking about new versions?\n\n"
			"To ask again, set auto = true in the [update] section of config.toml.",
			2, confirm_buttons, NULL };
		int confirmed = 0;

		if (SDL_ShowMessageBox(&confirm, &confirmed) && confirmed == 1)
		{
			if (config_write_boolean("update.auto", 0))
				platform_log("update: update.auto = false written to config.toml");
			else
				platform_log("update: could not write update.auto to config.toml");
		}
	}
	else if (answer == 1)
	{
		updater_update();
	}
	if (fullscreen)
		SDL_SetWindowFullscreen(window, true);
}

/* (the Frame's alone: menu_functions.c's "port update now") */
void updater_update_now(void)
{
}
#endif

#else

void updater_start(void)
{
}

/* (the Frame's alone: menu_functions.c's "port update now") */
void updater_update_now(void)
{
}

#ifndef HALO_ANDROID
#include <SDL3/SDL.h>

/* (nothing to ask about: the site updates itself, port/web/site/sw.js, and
the other builds without UPDATE_PLATFORM are updated otherwise) */
void updater_poll(SDL_Window *window)
{
	(void)window;
}
#endif

#endif
