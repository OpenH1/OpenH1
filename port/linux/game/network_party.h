/*
NETWORK_PARTY.H

Parties made in a network game (network_party.c): what the main loop, the
HUD (hud.c), the scoreboard (game_engine.c) and the netcode ask of it.
*/

#ifndef __NETWORK_PARTY_H
#define __NETWORK_PARTY_H
#pragma once

/* ---------- constants */

/* what this machine may do about another's player (a bit each) */
enum
{
	/* invite them to this machine's party (one made for it if none) */
	_network_party_action_invite,
	/* ask to join theirs (they are in one) */
	_network_party_action_join,
	/* accept their invite, or let them in (they asked) */
	_network_party_action_accept,
	NUMBER_OF_NETWORK_PARTY_ACTIONS
};

/* ---------- prototypes/NETWORK_PARTY.C */

/* every frame (main.c): this machine's party told the host, the host's
list of who can party sent on, and the accept key */
void network_party_update(void);
/* the actions offered for a player (a datum index) of another machine: a
bit each (_network_party_action_*); none for an OpenCE machine's player, a
bot, a machine that has not said it can party, or outside an OpenH1 game.
The scoreboard's player menu shows them (the lobby's and the carnage
report's could) */
unsigned long network_party_player_actions(long player_index);
/* ... and what one says (at most size characters with its end) */
void network_party_action_label(long player_index, short action, wchar_t *label, short size);
/* ... done */
void network_party_player_act(long player_index, short action);
/* the newest invite (or ask) accepted: as its key does */
boolean network_party_accept(void);
/* (the HUD of the first local player's view) the invite to accept, with
its key, or what was done; and a Discord friend's ask to join */
void network_party_draw(short local_player_index);
/* a Discord friend's ask to join, with its keys, if there is one: centered
in window at y, in the font (the HUD's in the game; over the menus,
ui_widget.c's, theirs) */
void network_party_discord_draw(rectangle2d const *window, long font_index, short y);

/* (network_distributed.c) the messages, in the lobby too; from_stream: it
came over the sender's own stream (anything else is refused) */
boolean network_party_handles_message(word const *message, word size);
void network_party_handle_message(long machine_index, word const *message, word size, boolean from_stream);

#endif
