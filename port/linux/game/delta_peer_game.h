/*
DELTA_PEER_GAME.H

The game's Delta Peer (delta_peer_game.c; port/linux/NETCODE.md, "Delta
Peer"): ChupathingyCE's handshake beside the game, so that OpenH1 and the
ports that speak Delta know each other in a game and show each other's
platform on the scoreboard. It never touches the game's state, holds up
nothing, and changes nothing an OpenCE machine sees but its advertisement's
last nine reserved bytes, which OpenCE leaves and ignores.
*/

#ifndef __DELTA_PEER_GAME_H
#define __DELTA_PEER_GAME_H

/* each frame, in the lobby and in game: the host's sessions, or this
client's handshake with its host (network.delta_peer) */
void delta_peer_game_update(void);

/* the host's advertisement: its reserved bytes (size of them), the Delta
tag at their tail while this host listens for Delta Peer in a game OpenCE
machines may join (its version HALO_PORT_NETWORK_VERSION's); else the
tag's bytes zero */
void delta_peer_game_advertise(byte *reserved, long size);

/* a client: whether a host's advertisement (its reserved bytes) says it
speaks Delta Peer, a tag of any major (another major answers LEGACY) */
boolean delta_peer_game_advertised(byte const *reserved, long size);
/* ... and that of the game this client joins now */
void delta_peer_game_joining(boolean host_speaks_delta);

/* the scoreboard's badge of a player's machine (its index in the game):
TRUE and its text (count characters, its end included) for one known over
Delta whose badge is not this machine's own (delta_peer.h,
delta_peer_machine_badge) */
boolean delta_peer_game_badge(long machine_index, wchar_t *text, long count);

#endif
