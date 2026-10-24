/*
NETWORK_SUBSTEPS.H

An OpenH1 game's sub-step inputs (network_substeps.c).
*/

#ifndef NETWORK_SUBSTEPS_H
#define NETWORK_SUBSTEPS_H

/* ---------- prototypes */

/* whether this tick is a network game's of several sub-steps (an OpenH1
game at 60 or 120), whose players' sub-steps go between the machines */
boolean network_substeps_active(void);

/* a new game: nothing heard or kept of the last */
void network_substeps_new_game(void);

/* the fast phase (player_control.c's slot): its first sub-step, before
which each player's tick input was taken, and each later one, after the
local players' units took their sub-step's reading */
void network_substeps_tick_begun(short substep_count);
void network_substeps_replay(short substep_index, short substep_count);
/* a local player's unit given its sub-step's reading (player_control.c):
its facing as given, and whether the unit's aim turned to it */
void network_substeps_local_given(long player_index, long unit_index, short substep_index, short substep_count,
	real_euler_angles2d const *facing, boolean turned);
/* the controls of a remote player's unit that went down in the running
sub-step, as its record has them (player_control_substep_pressed_flags'
for the units no local player controls) */
unsigned long network_substeps_pressed_flags(long unit_index);

/* after each tick (network_distributed.c): a client's own players' sub-steps
to the host; the host's of the players it relays to a client this tick (by
absolute index), with the update their input is of */
void network_substeps_client_send(void);
void network_substeps_host_send(long machine_index, long update_number, short const *player_indices, short count);

/* the messages (network_distributed.c's handling, before its own) */
boolean network_substeps_handles_message(word const *message, word size);
void network_substeps_handle_message(long machine_index, word const *message, word size);

/* the bytes of sub-step messages this machine sent since it was last asked
(the tests' bandwidth figures) */
long network_substeps_bytes_sent(void);

#endif
