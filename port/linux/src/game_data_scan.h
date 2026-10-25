/*
GAME_DATA_SCAN.H

Where the first start looks for the player's game data (game_data_scan.c):
Xbox disc images of Halo (.iso, .xiso) and maps folders already copied out
of one, in the places a player puts a file (Downloads, the Desktop, the home
folder, an SD card or a USB drive), in the maps folders an earlier version
of the game used, and next to the game. first_start.c shows what it finds
in the game's window, on Linux and macOS. Only the C library is used here, so that
it can be compiled and tested anywhere (port/android/tests/game_data_scan_test.c).
*/

#ifndef GAME_DATA_SCAN_H
#define GAME_DATA_SCAN_H

#define GAME_DATA_CANDIDATES 12
#define GAME_DATA_PATH_SIZE 1024
#define GAME_DATA_MEDIA_ROOTS 3
#define GAME_DATA_EARLIER 4
#define GAME_DATA_ROOTS 16

enum
{
	/* an Xbox disc image whose maps are extracted (xiso.c) */
	_game_data_image,
	/* a maps folder (with the Xbox game's ui.map) whose maps are copied */
	_game_data_maps_folder,
};

struct game_data_candidate
{
	int kind;
	/* the image, or the maps folder */
	char path[GAME_DATA_PATH_SIZE];
	/* its file name, or for a maps folder the folder's */
	char name[256];
	/* where it is, for the player: "Downloads", "SD card/Games" */
	char place[GAME_DATA_PATH_SIZE];
	/* the image's bytes, or the maps folder's maps' */
	unsigned long long size;
};

/* the places looked in, in order (game_data_default_places) */
struct game_data_places
{
	/* the folder the maps are copied to (the game's: install_data_folder),
	looked in itself only */
	char destination[GAME_DATA_PATH_SIZE];
	/* the home folder ("" for none), and its Downloads and Desktop folders
	(the XDG user folders, else ~/Downloads and ~/Desktop) */
	char home[GAME_DATA_PATH_SIZE];
	char downloads[GAME_DATA_PATH_SIZE];
	char desktop[GAME_DATA_PATH_SIZE];
	/* the folders removable drives are mounted under (/run/media,
	/media; /Volumes on macOS), and the table of mounts that names their
	devices ("" for none: each drive is then the first folder below its
	media folder, as /Volumes/NAME) */
	char media[GAME_DATA_MEDIA_ROOTS][GAME_DATA_PATH_SIZE];
	char mounts[GAME_DATA_PATH_SIZE];
	/* the maps folders an earlier version of the game used (the macOS
	app's: HALO_MAPS_USED_BEFORE, a path a line), looked in first */
	char earlier[GAME_DATA_EARLIER][GAME_DATA_PATH_SIZE];
	/* how long a scan may take, in milliseconds */
	int time_limit;
};

/* whether an image is one of Halo (xiso.c's xiso_is_halo_image), so that
only those are offered */
typedef int (*game_data_image_check)(const char *path, void *context);

/* the places on this system, for a game whose maps go to destination */
void game_data_default_places(struct game_data_places *places, const char *destination);

/* looks for disc images and maps folders: a few folders deep, skipping
hidden and very large folders, and within places->time_limit. Writes up to
maximum candidates, images first, each found once, and returns how many */
int game_data_scan(const struct game_data_places *places, game_data_image_check check, void *context,
	struct game_data_candidate *candidates, int maximum);

/* the places a scan looks in, for the player ("Downloads, the Desktop, ...") */
void game_data_places_text(const struct game_data_places *places, char *text, int size);

/* path for the player: in the home folder, from ~ */
void game_data_short_path(const struct game_data_places *places, const char *path, char *result, int size);

/* what a folder the player picked (the system's file dialog) is: 1 with
*candidate its Xbox maps folder (the folder, or a maps folder in it); 0 when
it is not a folder (a disc image, perhaps); -1 when it holds no Xbox maps,
with the reason (for the player) in error */
int game_data_picked_folder(const char *path, struct game_data_candidate *candidate, char *error, int error_size);

/* called as the copy goes: the file being copied, and the bytes copied of
all the files' */
typedef void (*game_data_progress_proc)(void *context, const char *file, unsigned long long done,
	unsigned long long total);

/* copies a maps folder's maps (its .map files and loading.tga) to
<destination>/maps, through <destination>/maps.partial as xiso.c does;
returns nonzero on success, or 0 with the reason (for the player) in error */
int game_data_copy_maps(const char *maps, const char *destination, game_data_progress_proc progress, void *context,
	char *error, int error_size);

#endif
