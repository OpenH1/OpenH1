/*
NETWORK_STANDARD.H

OpenH1's Standard games and OpenCE's Legacy ones (network_standard.c).
*/

#ifndef NETWORK_STANDARD_H
#define NETWORK_STANDARD_H

struct network_game;

/* ---------- prototypes */

/* hosting: the server made reads network.game_mode, which holds while it
hosts: whether its games are Standard */
void network_standard_server_created(void);
/* ... and again (Server Setup's GAME, chosen after the server was made),
while no other machine has joined: a game's kind holds once one has */
void network_standard_server_mode_read(void);
boolean network_standard_hosting(void);
/* the rate of the Standard game hosted: network.sim_rate as read in the
lobby (in_lobby, before the game's start), the same from the start to the
game's end. 0 hosting a Legacy game */
long network_standard_server_rate(boolean in_lobby);
/* the host's game record, as it goes out: a Standard game's rate in it
(nothing of a Legacy game's changes) */
void network_standard_stamp_game(struct network_game *game, boolean in_lobby);

/* joining: whether the game joined is Standard (its advertisement's), set
as the join begins, and its join token made a Standard game's */
void network_standard_client_joining(boolean standard, byte *join_token, long token_size);

/* a join token as a Standard game's (of this protocol's version): the host
of one takes only that, so a machine that is not of it is refused */
void network_standard_join_token(byte *join_token, long token_size);

/* the rate the network game played agreed on (sim_rate.c's
sim_rate_network): the Standard game's, its host's or the one its record
brought, or 0 for a Legacy game (or none) */
long network_standard_agreed_rate(void);

#endif
