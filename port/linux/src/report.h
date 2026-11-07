/*
REPORT.H

Save Report (report.c): a zip of what helps find a problem, saved on this
machine for the player to send.
*/

#ifndef REPORT_H
#define REPORT_H

#include <stddef.h>
#include <stdint.h>

/* a frame presented (d3d8_device.c's present): its time kept, the last
REPORT_FRAMES of them */
void report_frame_presented(void);
/* a line of the log (xbox_kernel.c's platform_log), kept: the last
REPORT_LOG_SIZE bytes */
void report_log_line(const char *line);
/* a report asked for (controls.report, Settings' REPORT A PROBLEM), saved
and sent at the next frame presented (report_write); and taken (1 if asked) */
void report_request(void);
int report_take_request(void);
/* the report saved: the target presented is its screenshot (0: none); send,
a GitHub issue opened in the browser, filled in with report.md, and the
reports folder, for its zip to be dragged in; what the player is told
(where it is, or why it failed) into message (empty: nothing yet); 1 if
saved. In a game the summary and the frame are taken now, and the zip
written once the player's note is typed (report_note_done). */
int report_write(unsigned int target, int send, char *message, size_t message_size);
/* the note typed for a report asked for in a game (network_chat.c's line),
NULL if it was let go: the report finished, as report_write does */
void report_note_done(const char *note);

/* ---------- report.c's parts (tools/harness/tests/test_report.py) */

/* the frame times' summary, in report.md's words: micros, each frame's time
(microseconds), at_ms, when it ended (milliseconds from the start), oldest
first; now_ms, the report's time */
void report_frame_summary(const uint32_t *micros, const uint32_t *at_ms, unsigned long count, uint32_t now_ms,
	char *text, size_t size);
/* a new GitHub issue's address on OpenH1's repository, its title and body
filled in (percent-encoded; the body cut at a line to keep the address
within most bytes); 1 if it fits */
int report_issue_url(const char *title, const char *body, char *url, size_t most);
/* a text with what could say who the player is taken out, its issue being
public: home (the account's folder) as ~, invite and party links, IPv4
addresses (but 127.x and 0.0.0.0), other players' chat lines; malloc()'s,
its size into scrubbed_size */
char *report_scrub(const char *text, size_t size, const char *home, size_t *scrubbed_size);
/* the zip's CRC-32 (crc 0 to begin, then each part's) */
uint32_t report_crc32(uint32_t crc, const unsigned char *data, size_t size);

/* where the problem happened, as report_game_where takes it the moment a
report is asked for: numbers and this machine's tag names only, never a
player's name or an address */
enum
{
	REPORT_WHERE_BUILDS = 8,
	/* (a build that is OpenCE's, not OpenH1's) */
	REPORT_WHERE_OPENCE = -1,
};
struct report_where
{
	/* 0: the menus (nothing more is said) */
	int playing;
	/* the map and the game, report_game_describe's; the level's name, as
	map_name takes it (the scenario's tag, or custom_maps\<name>); a
	campaign (no game engine) and its difficulty (0 easy to 3 impossible);
	the structure BSP loaded */
	char game[256];
	char map[128];
	int campaign, difficulty, bsp;
	/* the game's time, in ticks of ticks_per_second */
	long ticks, ticks_per_second;
	/* the first local player: whether there is one, alive; its place (its
	vehicle's, in one), its aim in degrees (yaw from the x axis, pitch up);
	its vehicle's tag (empty: on foot) */
	int player, alive;
	float position[3], yaw, pitch;
	char vehicle[128];
	/* its camera: position, yaw, pitch, roll and field of view in degrees
	(as debug_camera_set takes them, camera_pose.c) */
	int camera;
	float camera_position[3], camera_yaw, camera_pitch, camera_roll, camera_field_of_view;
	/* a network game: 0 none, 1 hosting, 2 joined; its OpenH1 game's rate
	(0: Legacy); the round trip in milliseconds to the host (a client's) or
	the slowest client's (the host's), -1 not known; the host silent for
	two seconds (a client); the players in the game; (the host) the other
	machines' builds as they told them, a count of each (a release's
	number, 0 a local build, REPORT_WHERE_OPENCE), and of those that told
	none */
	int network;
	long rate, ping;
	int connection_problem, players;
	long builds[REPORT_WHERE_BUILDS], build_counts[REPORT_WHERE_BUILDS];
	int build_kinds, builds_unknown;
};
/* report.md's "## Where" section of it (empty for the menus) */
void report_where_text(const struct report_where *where, char *text, size_t size);

/* port/linux/game/report_game.c: the map and the game, in a few words; and
whether a game is being played (not the menus: a report's issue waits) */
void report_game_describe(char *text, size_t size);
int report_game_playing(void);
/* ... and where the game is, this moment */
void report_game_where(struct report_where *where);
/* the line for a report's note opened (network_chat.c: 1 if it is; it calls
report_note_done), or closed unfinished; and the player told something */
int report_game_note_begin(void);
void report_game_note_cancel(void);
void report_game_say(const char *text);

#endif
