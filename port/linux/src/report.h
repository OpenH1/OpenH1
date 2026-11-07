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

/* this session begun (sdl_platform.c, as the game starts; not a dedicated
server's): the last run's crash found, if it did not exit, and this one's
file written, and the host told where to write a crash */
void report_session_start(void);
/* the game halted on a fatal error (main.c's halt_and_catch_fire, its
reason error_get's): reported at the next start as a crash is, though the
player then closes the game */
void report_session_halted(const char *reason);
/* the crash question's answer (menu_functions.c's "port crash report", 1,
and "port crash dismiss", 0): its report saved at the next frame, its issue
opened, or the crash let go; neither asked of again */
void report_crash_answer(int report);

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
/* report.md's heading of the last run's crash: session, its session's file
("build: ...", "started: ...", "reference address: ..." lines); record, the
host's (the crash's lines, then the log after host_common.c's heading), or
NULL if it wrote none */
void report_crash_summary(const char *session, const char *record, char *text, size_t size);
/* the zip's CRC-32 (crc 0 to begin, then each part's) */
uint32_t report_crc32(uint32_t crc, const unsigned char *data, size_t size);

/* port/linux/game/report_game.c: the map and the game, in a few words; and
whether a game is being played (not the menus: a report's issue waits) */
void report_game_describe(char *text, size_t size);
int report_game_playing(void);
/* the line for a report's note opened (network_chat.c: 1 if it is; it calls
report_note_done), or closed unfinished; and the player told something */
int report_game_note_begin(void);
void report_game_note_cancel(void);
void report_game_say(const char *text);

#endif
