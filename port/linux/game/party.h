/*
PARTY.H

The party's menus (party_menu.inc, in menu_functions.c; the party itself is
port/linux/src/p2p_party.c's).
*/

#ifndef __HALO_PORT_PARTY_H
#define __HALO_PORT_PARTY_H

/* each frame (main.c): the party told this machine's player and game, and
a member taken into its leader's game */
void party_update(void);
/* each frame (main.c): friends told the profile whose they are (player 1's)
and its name (port/linux/game/friends_menu.inc, port/linux/src/p2p_friends.c) */
void friends_update(void);

#endif
