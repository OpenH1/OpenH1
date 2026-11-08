/*
DEMO.H

Demos (demo.c, docs/demos.md): every game of this machine recorded as it
plays, its ticks' input and what reached its game state from outside them,
and played again exactly.
*/

#ifndef __HALO_PORT_DEMO_H
#define __HALO_PORT_DEMO_H

struct player_action;

/* each game state allocation, as game_state.c makes it */
void demo_game_state_allocation(char const *name, void const *address, long size);

/* the game state rewritten as a whole outside the ticks: a saved game or a
core loaded, a checkpoint reverted to (game_state.c's after-load
procedures), the map started over (main.c), a new map (game.c) */
void demo_note_state_loaded(void);
void demo_note_map_reset(void);
void demo_note_new_map(void);

/* the clock's ticks (game_time.c): before a tick's first sub-step, and
once its last has run */
void demo_tick_begin(long tick);
void demo_tick_end(long tick);

/* a tick left open between two of its sub-steps, the frames meanwhile
(game_time.c: game.sim_rate's sub-steps spread over the frames), and the
next of them begun: what the frames put into the game state in between
(a host's netcode dealing a client's reported hits) */
void demo_tick_pause(void);
void demo_substep_resume(short substep_index);

/* the tick's players' input (players.c): TRUE if a demo's playback gave it
(the bots then take no part); else, around the bots' turn, it is recorded */
boolean demo_playback_tick_actions(struct player_action *actions);
void demo_record_tick_actions_begin(void);
void demo_record_tick_actions(struct player_action const *actions);

/* a sub-step past the tick's first (player_control.c's slot): the players'
units' controls as it began and as it ended */
void demo_substep_begin(short substep_index);
void demo_substep_end(short substep_index, short substep_count);
/* in playback, a unit's controls that went down in the running sub-step */
unsigned long demo_playback_pressed_flags(long unit_index);

/* inside a tick, what the netcode puts into the game state from what other
machines sent (a host takes its clients' predictions of their units:
network_distributed.c): recorded as it changed the game state, and in
playback written as it did (the netcode then has nothing to put in) */
void demo_foreign_begin(void);
void demo_foreign_end(void);
/* inside a tick, a choice the netcode makes by what other machines are
there (a host leaves a client's hits to its reports: network_damage.c):
recorded, and in playback the recording's */
boolean demo_decide(boolean decision);

/* whether a demo is playing (the input of the machine's players, its own
saved games, its recording left alone), and the rate its ticks run at (0
when none is: sim_rate.c) */
boolean demo_playing(void);
long demo_playback_sim_rate(void);

/* each frame (main.c): a demo asked for (demo.play, --demo) started, and
the playback's end */
void demo_frame(boolean main_menu_loaded, real seconds);

/* the clip key: the demo being recorded or played marked here (its last
demo.clip_seconds) */
void demo_bookmark(void);

#endif
