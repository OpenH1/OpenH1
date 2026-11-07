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
/* a report asked for (controls.report, keyboard 1: its note asked for in a
game; Settings' REPORT A PROBLEM, 0: none), saved and sent at the next frame
presented (report_write); and taken (1 if asked) */
void report_request(int keyboard);
int report_take_request(void);
/* the report saved: the target presented is its screenshot (0: none); send,
a GitHub issue opened in the browser, filled in with report.md, and the
reports folder, for its zip to be dragged in. The zip is written on a
thread of its own: the player is told (where it is, or why it failed) once
it is, at a frame presented (report_frame_presented), so message is left
empty; 1. In a game the summary and the frame are taken now, and the zip
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
addresses (but 127.x and 0.0.0.0) and IPv6 ones (but ::1), chat lines and
the host's notices, and what follows a few markers (a Discord name, a link's
code, network.public_address); malloc()'s, its size into scrubbed_size */
char *report_scrub(const char *text, size_t size, const char *home, size_t *scrubbed_size);
/* where a log's end taken from its middle (cut: 1) starts: after its first
line's end, the partial line left out */
size_t report_whole_lines(const char *text, size_t size, int cut);
/* the zip's CRC-32 (crc 0 to begin, then each part's) */
uint32_t report_crc32(uint32_t crc, const unsigned char *data, size_t size);

/* port/linux/game/report_game.c: the map and the game, in a few words;
whether a game is being played (not the menus, nor a loading screen: the
note's line closes); and whether the main menu is up (the ui map: a
report's issue waits for it) */
void report_game_describe(char *text, size_t size);
int report_game_playing(void);
int report_game_menus(void);
/* the line for a report's note opened (network_chat.c: 1 if it is; it calls
report_note_done), or closed unfinished; and the player told something */
int report_game_note_begin(void);
void report_game_note_cancel(void);
void report_game_say(const char *text);

#endif
