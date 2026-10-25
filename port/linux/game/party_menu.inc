/*
PARTY_MENU.INC

The party's menus (port/linux/src/p2p_party.c's parties): Multiplayer's
PARTY and its screen (tools/port_settings.py's _party), included in
menu_functions.c, whose widgets' helpers it uses (kept apart for the
Multiplayer menu's redesign).

Each frame (party_update, from main.c) the party is told this player's
name and the game this machine is in: an internet game (its invite: the
one it hosts, or the one it joined, by a link or from the server browser),
a game no link reaches (a LAN game, a game of its own), or the menus. The
leader's is the party's.

A member asked to join its leader's game (p2p_party_follow) joins it from
the Party screen, which opens for it over the main menu, Multiplayer or a
browser (once for each of the leader's games; B leaves it, JOIN LEADER asks
again): the host is reached by its invite (p2p_join_invite), its game found
among the client's, then joined as a browser's game is (an A press posted
to the screen; a game under way shows its lobby first, as the browsers do).
A game full, of another version, a Legacy game this machine cannot join, or
a host not reached is said, and the member stays in the party; a member in
another game is joined once back in the menus. A party joined by a link
elsewhere (the clipboard, a link opened, JOIN BY LINK) opens the screen too.

The follow's steps (party_follow_step) and what a found game allows
(party_game_verdict) are tools/harness/tests/test_party_follow.py's.
*/

#define PARTY_NAME "pc\\main_menu\\multiplayer_type_select\\party\\party_screen"
/* the members' lines and the panel's */
#define PARTY_TEXT_LENGTH 768
/* milliseconds: the host not reached, its game not found, not open, not
joined in this long, the follow gives up; a notice is shown this long */
#define PARTY_REACH_TIME 20000
#define PARTY_FIND_TIME 10000
#define PARTY_OPEN_WAIT 120000
#define PARTY_JOIN_TIME 20000
#define PARTY_READY_TIME 3000
#define PARTY_NOTICE_TIME 10000
#define PARTY_UPDATE_INTERVAL 100

/* a follow's steps: none; reaching the host; finding its game; joining it
(the A press posted); joined, waiting to be in it */
enum
{
	_party_follow_idle,
	_party_follow_reaching,
	_party_follow_finding,
	_party_follow_ready,
	_party_follow_joining,
};

/* what a follow's next step is made from */
struct party_follow_view
{
	short stage;
	/* milliseconds in this step */
	unsigned long waited;
	/* the Party screen is shown */
	boolean screen;
	/* p2p_join_status's stage */
	short join_stage;
	/* the leader's game is among the client's, and what it allows
	(party_game_verdict) */
	boolean found;
	short verdict;
	/* the client is in the leader's game; it is in none (searching, or
	gone) */
	boolean in_game;
	boolean in_none;
};

static struct
{
	/* the follow under way: the leader's game's order, its invite and its
	host's identifier */
	short stage;
	unsigned long order;
	char link[TEXT_FIELD_LENGTH];
	byte host[6];
	unsigned long stage_time;
	/* the controller of the Party screen's player */
	short controller;
	/* how the last follow ended, and of which order (the panel's) */
	short result;
	unsigned long result_order;
	/* the screens noted since the last frame: the Party screen, and the one
	it may be opened over (its tag, and the tags' generation that is of) */
	boolean screen_shown;
	boolean over_shown;
	long over_tag;
	unsigned long over_generation;
	/* the leader's game the screen was opened for (not again after B), and
	the order told busy */
	unsigned long opened_order;
	unsigned long busy_order;
	/* in a party at the last frame; a party made or joined here */
	int last_state;
	boolean asked_here;
	/* the newest notice shown, and since when */
	unsigned long notice_serial;
	unsigned long notice_time;
	/* the name told */
	char name[P2P_PARTY_NAME_SIZE];
	/* a party's link on the clipboard, as last read */
	unsigned long clipboard_time;
	boolean clipboard_link;
	/* debug.party taken */
	boolean debug_party_done;
	/* the last frame party_update did its work */
	unsigned long update_time;
} party_menu;

/* what a game found allows a member following its leader: _p2p_party_follow_joined
(join it), _p2p_party_follow_none (wait: it is not open yet), or why not */
static short party_game_verdict(boolean open, boolean in_progress, short players, short maximum, boolean compatible,
	boolean standard, boolean mixed)
{
	if (!compatible)
		return standard || mixed ? _p2p_party_follow_version : _p2p_party_follow_legacy;
	if (maximum > 0 && players >= maximum)
		return _p2p_party_follow_full;
	if (!open && !in_progress)
		return _p2p_party_follow_none;
	return _p2p_party_follow_joined;
}

/* a follow's next step, and once it ends how it went (result, else
_p2p_party_follow_none) */
static short party_follow_step(struct party_follow_view const *view, short *result)
{
	*result = _p2p_party_follow_none;
	switch (view->stage)
	{
	case _party_follow_reaching:
	case _party_follow_finding:
	case _party_follow_ready:
		/* (the Party screen left: given up, the member's own doing) */
		if (!view->screen)
			return _party_follow_idle;
		break;
	}
	switch (view->stage)
	{
	case _party_follow_reaching:
		if (view->join_stage == _p2p_join_reached)
			return _party_follow_finding;
		if ((view->join_stage == _p2p_join_idle && view->waited > 1000) || view->waited > PARTY_REACH_TIME)
		{
			*result = _p2p_party_follow_unreachable;
			return _party_follow_idle;
		}
		return _party_follow_reaching;
	case _party_follow_finding:
		if (!view->found)
		{
			if (view->waited <= PARTY_FIND_TIME)
				return _party_follow_finding;
			*result = _p2p_party_follow_unreachable;
			return _party_follow_idle;
		}
		if (view->verdict == _p2p_party_follow_joined)
			return _party_follow_ready;
		if (view->verdict == _p2p_party_follow_none && view->waited <= PARTY_OPEN_WAIT)
			return _party_follow_finding;
		*result = view->verdict == _p2p_party_follow_none ? _p2p_party_follow_full : view->verdict;
		return _party_follow_idle;
	case _party_follow_ready:
		/* (the press not taken: a dialog over the screen) posted again */
		return view->waited > PARTY_READY_TIME ? _party_follow_finding : _party_follow_ready;
	case _party_follow_joining:
		if (view->in_game)
		{
			*result = _p2p_party_follow_joined;
			return _party_follow_idle;
		}
		if ((view->in_none && view->waited > 2000) || view->waited > PARTY_JOIN_TIME)
		{
			*result = _p2p_party_follow_refused;
			return _party_follow_idle;
		}
		return _party_follow_joining;
	}
	return _party_follow_idle;
}

/* ---- the frame's */

/* a link's host's identifier (its XNADDR's abEnet: p2p.c's
p2p_identifier_from_hash of the invite's key hash): FALSE if it is no
invite link */
static boolean party_link_host(char const *link, byte *host)
{
	char const *start = strstr(link, "halo://join/");
	short index;

	if (!start)
		return FALSE;
	start += strlen("halo://join/");
	for (index = 0; index < 12; index++)
	{
		int digit = start[index];

		digit = digit >= '0' && digit <= '9' ? digit - '0' : (digit | 0x20) >= 'a' && (digit | 0x20) <= 'f' ?
			(digit | 0x20) - 'a' + 10 : -1;
		if (digit < 0)
			return FALSE;
		host[index / 2] = (byte)(index & 1 ? host[index / 2] | digit : digit << 4);
	}
	host[0] = (byte)((host[0] & 0xFC) | 0x02);
	return TRUE;
}

/* the value of an address the game keeps (host order), in network order */
static unsigned long party_network_order(unsigned long value)
{
	byte bytes[4];
	unsigned long result;

	bytes[0] = (byte)(value >> 24);
	bytes[1] = (byte)(value >> 16);
	bytes[2] = (byte)(value >> 8);
	bytes[3] = (byte)value;
	csmemcpy(&result, bytes, sizeof(result));
	return result;
}

/* the client's game state, NONE without one */
static short party_client_state(void)
{
	void *client = global_network_game_client_get();
	short state_data;

	return client ? network_game_client_get_state(client, &state_data) : NONE;
}

/* in the menus, in no game (a client looking for games at most: a game
backed out of may keep its server, paused, which is no game) */
static boolean party_in_menus(void)
{
	short state = party_client_state();

	return main_menu_is_active() && (state == NONE || state == _client_state_searching);
}

/* the game this machine is in, told the party: its place, what it is, and
its invite (link). A host's own client is in its game too */
static void party_game_tell(void)
{
	void *server = global_network_game_server_get();
	void *client = global_network_game_client_get();
	short state = party_client_state();
	char link[TEXT_FIELD_LENGTH];
	int place = _p2p_party_place_menus;
	int kind = _p2p_party_game_none;

	if (client && state >= _client_state_pregame)
	{
		unsigned long host = 0;

		if (!server)
		{
			struct transport_address address;

			csmemset(&address, 0, sizeof(address));
			network_game_client_get_remote_server_address(client, &address);
			host = party_network_order(address.address.long_words[0]);
		}
		if ((server || host) && p2p_game_invite(host, link, sizeof(link)))
		{
			place = _p2p_party_place_game;
			kind = _p2p_party_game_invite;
		}
		else
		{
			place = _p2p_party_place_local_game;
			kind = _p2p_party_game_local;
		}
	}
	else if (client && state == _client_state_joining)
		place = _p2p_party_place_joining;
	p2p_party_set_game(place, kind, kind == _p2p_party_game_invite ? link : NULL);
}

/* player 1's name (its profile's, else the one last used), told the party */
static void party_name_tell(void)
{
	struct player_profile profile;
	long profile_index = player_ui_get_active_player_profile_index(0);
	char name[P2P_PARTY_NAME_SIZE];
	short index;

	if (profile_index == NONE)
		profile_index = player_ui_get_player1_last_used_profile_index();
	name[0] = 0;
	if (profile_index != NONE && player_profile_get(profile_index, &profile))
	{
		for (index = 0; index < P2P_PARTY_NAME_SIZE - 1 && index < NUMBEROF(profile.player_name) &&
			profile.player_name[index]; index++)
		{
			wchar_t character = profile.player_name[index];

			name[index] = (char)(character >= 0x20 && character < 0x7F ? character : '?');
		}
		name[index] = 0;
	}
	if (!name[0])
		snprintf(name, sizeof(name), "Player");
	if (strcmp(name, party_menu.name))
	{
		snprintf(party_menu.name, sizeof(party_menu.name), "%s", name);
		p2p_party_set_name(name);
	}
}

/* the leader's game among the client's, NULL if it is not */
static struct advertised_game *party_found_game(void)
{
	void *client = global_network_game_client_get();
	struct advertised_game *games = client ? network_game_client_get_available_games(client) : NULL;
	short index;

	for (index = 0; games && index < MAXIMUM_ADVERTISED_GAMES; index++)
	{
		if (network_game_client_advertised_game_is_valid(&games[index]) &&
			!memcmp(games[index].xnaddr + 2, party_menu.host, sizeof(party_menu.host)))
		{
			return &games[index];
		}
	}
	return NULL;
}

/* whether the client is in the leader's game (its host is the follow's) */
static boolean party_in_leader_game(void)
{
	void *client = global_network_game_client_get();
	struct transport_address address;
	unsigned long address_value;
	byte identifier[6], key_hash[P2P_PLAYER_KEY_HASH_SIZE];

	if (!client || party_client_state() < _client_state_pregame)
		return FALSE;
	csmemset(&address, 0, sizeof(address));
	network_game_client_get_remote_server_address(client, &address);
	address_value = party_network_order(address.address.long_words[0]);
	return p2p_peer_key_hash(address_value, identifier, key_hash) &&
		!memcmp(identifier, party_menu.host, sizeof(identifier));
}

static void party_follow_end(short result)
{
	if (result != _p2p_party_follow_none)
	{
		p2p_party_follow_result(party_menu.order, result);
		party_menu.result = result;
		party_menu.result_order = party_menu.order;
		platform_log("menus: following the party's leader: %s", result == _p2p_party_follow_joined ? "joined" :
			result == _p2p_party_follow_full ? "the game is full (or never opened)" :
			result == _p2p_party_follow_version ? "another version" :
			result == _p2p_party_follow_legacy ? "a Legacy game this version cannot join" :
			result == _p2p_party_follow_unreachable ? "the host or its game was not reached" :
			"not joined (turned away, or no player profile)");
		if (result != _p2p_party_follow_joined)
		{
			p2p_join_cancel();
			ui_play_audio_feedback_sound(SOUND_ERROR);
		}
	}
	else if (party_menu.stage == _party_follow_reaching || party_menu.stage == _party_follow_finding)
		p2p_join_cancel();
	party_menu.stage = _party_follow_idle;
}

/* the follow's step this frame, or a new one begun (the Party screen
shown, in the menus) */
static void party_follow_update(void)
{
	char link[TEXT_FIELD_LENGTH];
	unsigned long order = 0;
	unsigned long now = system_milliseconds();
	int kind;

	if (party_menu.stage != _party_follow_idle)
	{
		struct party_follow_view view;
		struct p2p_join_status join;
		struct advertised_game *game = NULL;
		short stage, result;

		csmemset(&view, 0, sizeof(view));
		view.stage = party_menu.stage;
		view.waited = now - party_menu.stage_time;
		view.screen = party_menu.screen_shown;
		p2p_join_status(&join);
		view.join_stage = (short)join.stage;
		if (party_menu.stage == _party_follow_finding)
			game = party_found_game();
		view.found = game != NULL;
		if (game)
		{
			void *client = global_network_game_client_get();

			view.verdict = party_game_verdict(game->open, advertised_in_progress(game), (short)game->player_count,
				game->maximum_player_count, network_game_client_advertised_game_compatible(client, game, FALSE),
				advertised_game_standard(game), advertised_game_mixed(game));
		}
		view.in_game = party_in_leader_game();
		view.in_none = party_client_state() == NONE || party_client_state() == _client_state_searching;
		stage = party_follow_step(&view, &result);
		if (stage == _party_follow_idle)
			party_follow_end(result);
		else if (stage != party_menu.stage)
		{
			party_menu.stage = stage;
			party_menu.stage_time = now;
			/* (found, and open: joined by the screen's A, its player's) */
			if (stage == _party_follow_ready)
				ui_widget_port_post_button(party_menu.controller, BUTTON_A);
		}
		return;
	}
	kind = p2p_party_follow(link, sizeof(link), &order);
	if (kind == _p2p_party_game_none)
		return;
	if (kind == _p2p_party_game_local)
	{
		p2p_party_follow_result(order, _p2p_party_follow_local);
		party_menu.result = _p2p_party_follow_local;
		party_menu.result_order = order;
		return;
	}
	/* (in another game: joined once back in the menus) */
	if (!party_in_menus())
	{
		if (party_menu.busy_order != order)
		{
			party_menu.busy_order = order;
			p2p_party_follow_result(order, _p2p_party_follow_busy);
		}
		return;
	}
	if (!party_menu.screen_shown || !party_link_host(link, party_menu.host))
		return;
	if (!p2p_join_invite(link))
	{
		p2p_party_follow_result(order, _p2p_party_follow_unreachable);
		return;
	}
	platform_log("menus: following the party's leader into its game");
	snprintf(party_menu.link, sizeof(party_menu.link), "%s", link);
	party_menu.order = order;
	party_menu.stage = _party_follow_reaching;
	party_menu.stage_time = now;
}

/* the Party screen opened over the screen noted (the main menu,
Multiplayer, a browser), between frames: for a leader's game to follow (once
each), or a party joined by a link elsewhere */
static void party_screen_open(struct p2p_party_status const *status)
{
	char link[TEXT_FIELD_LENGTH];
	unsigned long order = 0;
	boolean wanted = FALSE;

	if (party_menu.screen_shown || !party_menu.over_shown ||
		party_menu.over_generation != cache_files_tags_generation() || !party_in_menus())
	{
		return;
	}
	if (status->state == _p2p_party_joining && party_menu.last_state == _p2p_party_none && !party_menu.asked_here)
		wanted = TRUE;
	if (p2p_party_follow(link, sizeof(link), &order) == _p2p_party_game_invite && order != party_menu.opened_order)
	{
		party_menu.opened_order = order;
		wanted = TRUE;
	}
	if (wanted && ui_widget_load_by_name_or_tag(PARTY_NAME, NONE, NULL, NONE, party_menu.over_tag, NONE, NONE))
		platform_log("menus: the Party screen opened (%s)", status->state == _p2p_party_joining ?
			"a party joined" : "following the leader");
}

/* main.c's, each frame: ten times a second (the party is internet play's,
under its lock; the screens noted meanwhile are kept) */
void party_update(void)
{
	struct p2p_party_status status;
	unsigned long now = system_milliseconds();

	if (party_menu.update_time && now - party_menu.update_time < PARTY_UPDATE_INTERVAL)
		return;
	party_menu.update_time = now | 1;
	if (!config_boolean("network.online") || !game_time_initialized())
		return;
	/* (debug.party: a test's copies of the game in one party, from the
	start) */
	if (!party_menu.debug_party_done)
	{
		char const *text = config_string("debug.party");

		party_menu.debug_party_done = TRUE;
		party_menu.asked_here = *text != 0;
		if (!strncmp(text, "lead:", 5))
			p2p_party_lead_link(text + 5);
		else if (*text)
			p2p_join_invite(text);
	}
	party_name_tell();
	party_game_tell();
	party_follow_update();
	p2p_party_status(&status);
	party_screen_open(&status);
	party_menu.last_state = status.state;
	if (status.state != _p2p_party_joining)
		party_menu.asked_here = FALSE;
	party_menu.screen_shown = party_menu.over_shown = FALSE;
}

/* a screen the Party screen may open over (the main menu, Multiplayer, a
browser) shown this frame */
static void party_screen_noted(struct widget_instance *widget)
{
	while (widget->parent)
		widget = widget->parent;
	party_menu.over_shown = TRUE;
	party_menu.over_tag = widget->definition_tag_index;
	party_menu.over_generation = cache_files_tags_generation();
}

/* ---- the Party screen */

/* why a member is not in the leader's game, as the panel and the members'
lines say it */
static wchar_t const *party_result_text(int result)
{
	switch (result)
	{
	case _p2p_party_follow_unreachable:
		return L"couldn't reach the host";
	case _p2p_party_follow_full:
		return L"the game is full";
	case _p2p_party_follow_version:
		return L"another version of OpenH1";
	case _p2p_party_follow_legacy:
		return L"a Legacy game this version can't join";
	case _p2p_party_follow_local:
		return L"a local network game";
	case _p2p_party_follow_busy:
		return L"in another game";
	case _p2p_party_follow_refused:
		return L"the host didn't let them in";
	}
	return L"";
}

static void party_wide(char const *text, wchar_t *wide, short size)
{
	short index;

	for (index = 0; text[index] && index < size - 1; index++)
		wide[index] = (wchar_t)(unsigned char)text[index];
	wide[index] = 0;
}

/* a notice's words */
static void party_notice_text(struct p2p_party_notice const *notice, wchar_t *text, short size)
{
	wchar_t name[P2P_PARTY_NAME_SIZE];

	party_wide(notice->name[0] ? notice->name : "Someone", name, NUMBEROF(name));
	switch (notice->kind)
	{
	case _p2p_party_notice_made:
		usnprintf(text, size - 1, L"Party made. Its link is copied:\r\nsend it to your friends.");
		break;
	case _p2p_party_notice_joined:
		usnprintf(text, size - 1, L"You joined %s's party.", name);
		break;
	case _p2p_party_notice_member_joined:
		usnprintf(text, size - 1, L"%s joined the party.", name);
		break;
	case _p2p_party_notice_member_left:
		usnprintf(text, size - 1, L"%s left the party.", name);
		break;
	case _p2p_party_notice_member_lost:
		usnprintf(text, size - 1, L"Lost contact with %s.", name);
		break;
	case _p2p_party_notice_leader:
		usnprintf(text, size - 1, L"You lead the party now.");
		break;
	case _p2p_party_notice_new_leader:
		usnprintf(text, size - 1, L"%s leads the party now.", name);
		break;
	case _p2p_party_notice_disbanded:
		usnprintf(text, size - 1, L"The leader ended the party.");
		break;
	case _p2p_party_notice_ended:
		usnprintf(text, size - 1, L"You ended the party.");
		break;
	case _p2p_party_notice_you_left:
		usnprintf(text, size - 1, L"You left the party.");
		break;
	case _p2p_party_notice_full:
		usnprintf(text, size - 1, L"That party is full.");
		break;
	case _p2p_party_notice_unreachable:
		usnprintf(text, size - 1, L"Couldn't reach that party.\r\nIt may have ended.");
		break;
	case _p2p_party_notice_version:
		usnprintf(text, size - 1, L"%s has another version\r\nof OpenH1: update to play\r\ntogether.", name);
		break;
	case _p2p_party_notice_follow_failed:
		usnprintf(text, size - 1, L"%s couldn't join:\r\n%s.", name, party_result_text(notice->result));
		break;
	default:
		text[0] = 0;
		break;
	}
	text[size - 1] = 0;
}

/* a member's line: its name, and where it is */
static void party_member_line(struct p2p_party_status const *status, struct p2p_party_member const *member,
	wchar_t *line, short size)
{
	wchar_t name[P2P_PARTY_NAME_SIZE];
	wchar_t const *where;

	party_wide(member->name[0] ? member->name : "Player", name, NUMBEROF(name));
	if (member->leader)
		where = L"Leader";
	else if (member->self && status->state == _p2p_party_joining)
		where = L"Joining...";
	else if (member->place == _p2p_party_place_joining)
		where = L"Joining a game...";
	else if (member->with_leader)
		where = status->target_kind == _p2p_party_game_none ? L"Ready" : L"In the game";
	else if (member->place != _p2p_party_place_menus)
		where = L"In another game";
	else if (member->result != _p2p_party_follow_none && member->result != _p2p_party_follow_joined &&
		member->result != _p2p_party_follow_busy)
	{
		usnprintf(line, size - 1, L"%s%s: Couldn't join (%s)%s\r\n", name, member->self ? L" (you)" : L"",
			party_result_text(member->result), member->other_version ? L", another version" : L"");
		line[size - 1] = 0;
		return;
	}
	else
		where = L"In the menus";
	usnprintf(line, size - 1, L"%s%s: %s%s\r\n", name, member->self ? L" (you)" : L"", where,
		member->other_version ? L" (another version)" : L"");
	line[size - 1] = 0;
}

/* the leader in a game this machine is not in (JOIN LEADER) */
static boolean party_leader_elsewhere(struct p2p_party_status const *status)
{
	short index;

	for (index = 0; index < status->count; index++)
	{
		if (status->members[index].self)
		{
			return !status->leader && status->target_kind == _p2p_party_game_invite &&
				!status->members[index].with_leader;
		}
	}
	return FALSE;
}

/* the panel's words: the party's link, where the leader is, the follow,
and the newest notice */
static void party_panel_text(struct p2p_party_status const *status, wchar_t *text, short size)
{
	wchar_t part[ROW_TEXT_LENGTH * 3];
	unsigned long now = system_milliseconds();
	short length;

	text[0] = 0;
	if (status->state == _p2p_party_none)
	{
		usnprintf(text, size - 1, L"Make a party, then send your\r\nfriends its link. When you\r\n"
			L"host or join a game, they\r\njoin it with you.\r\n\r\nGot a party link? Copy it,\r\nthen JOIN FROM LINK.");
	}
	else if (status->state == _p2p_party_joining)
		usnprintf(text, size - 1, L"Joining the party...");
	else
	{
		char shown[32];
		wchar_t link[32];

		/* (its first digits: the whole link is on the clipboard, and the
		screen may be streamed) */
		snprintf(shown, sizeof(shown), "%.21s...", status->link);
		party_wide(shown, link, NUMBEROF(link));
		usnprintf(text, size - 1, L"PARTY LINK\r\n%s\r\nCOPY LINK puts it on the\r\nclipboard.\r\n\r\n", link);
		length = (short)ustrlen(text);
		if (status->leader)
		{
			usnprintf(text + length, size - 1 - length, status->count > 1 ?
				L"You lead. Host or join a game\r\nand your party follows." :
				L"You lead. Waiting for friends\r\nto join.");
		}
		else if (party_menu.stage == _party_follow_reaching || party_menu.stage == _party_follow_finding ||
			party_menu.stage == _party_follow_ready)
		{
			usnprintf(text + length, size - 1 - length, party_menu.stage == _party_follow_reaching ?
				L"Following your leader:\r\nfinding the host..." : L"Following your leader:\r\njoining the game...");
		}
		else if (party_menu.result_order && party_menu.result != _p2p_party_follow_joined &&
			party_menu.result != _p2p_party_follow_none && party_leader_elsewhere(status))
		{
			usnprintf(text + length, size - 1 - length, L"Couldn't join your leader's\r\ngame: %s.",
				party_result_text(party_menu.result));
		}
		else if (status->target_kind == _p2p_party_game_invite)
			usnprintf(text + length, size - 1 - length, L"Your leader is in a game.");
		else if (status->target_kind == _p2p_party_game_local)
		{
			usnprintf(text + length, size - 1 - length,
				L"Your leader is in a local\r\nnetwork game, which the\r\nparty can't follow.");
		}
		else
			usnprintf(text + length, size - 1 - length, L"Waiting for your leader to\r\nhost or join a game.");
	}
	/* the newest notice, a while */
	if (status->notice_count)
	{
		struct p2p_party_notice const *notice = &status->notices[status->notice_count - 1];

		if (notice->serial != party_menu.notice_serial)
		{
			party_menu.notice_serial = notice->serial;
			party_menu.notice_time = now;
		}
		if (now - party_menu.notice_time < PARTY_NOTICE_TIME)
		{
			party_notice_text(notice, part, NUMBEROF(part));
			length = (short)ustrlen(text);
			usnprintf(text + length, size - 1 - length, L"\r\n\r\n%s", part);
		}
	}
	text[size - 1] = 0;
}

/* "port party open" (the screen's created): the client that finds the
leader's game, as a browser's */
static boolean party_screen_open_event(struct widget_instance *widget, struct event_record *event, short controller,
	boolean *widget_deleted)
{
	party_menu.controller = controller >= 0 && controller < 4 ? controller : 0;
	return ui_widget_port_browse(widget, event, widget_deleted);
}

/* "port party update": the members, the panel, the buttons shown */
static void party_screen_update(struct widget_instance *list)
{
	struct widget_instance *screen = screen_of(list);
	struct p2p_party_status status;
	wchar_t text[PARTY_TEXT_LENGTH];
	short index, length = 0;
	boolean in_party, member;

	party_menu.screen_shown = TRUE;
	p2p_party_status(&status);
	in_party = status.state != _p2p_party_none;
	member = status.state == _p2p_party_member;
	text[0] = 0;
	if (!config_boolean("network.online"))
		usnprintf(text, NUMBEROF(text) - 1, L"Internet play is off.\r\nTurn it on in Settings,\r\nNetwork Setup.");
	for (index = 0; index < status.count; index++)
	{
		party_member_line(&status, &status.members[index], text + length, (short)(NUMBEROF(text) - length));
		length = (short)ustrlen(text);
	}
	text_set_length(named(screen, "party_members", 0), text, PARTY_TEXT_LENGTH);
	usnprintf(text, NUMBEROF(text) - 1, in_party ? L"PARTY (%d OF %d)" : L"PARTY", status.count,
		P2P_PARTY_MAXIMUM_MEMBERS);
	text_set(named(screen, "party_title", 0), text);
	party_panel_text(&status, text, NUMBEROF(text));
	text_set_length(named(screen, "party_panel", 0), text, PARTY_TEXT_LENGTH);
	/* (the buttons: the device's own, drawn as the input in use has them) */
	text_set(named(screen, "party_prompts", 0), L"%a-button Select    %b-button Back");
	/* (JOIN FROM LINK while a party's link is on the clipboard, read again
	twice a second) */
	if (!party_menu.clipboard_time || system_milliseconds() - party_menu.clipboard_time > 500)
	{
		char clipboard[TEXT_FIELD_LENGTH];
		unsigned char token[16];

		party_menu.clipboard_time = system_milliseconds() | 1;
		party_menu.clipboard_link = platform_clipboard_get(clipboard, sizeof(clipboard)) &&
			p2p_party_parse_link(clipboard, token);
	}
	visible_set(named(list, "party_button_make", 0), !in_party);
	visible_set(named(list, "party_button_join", 0), !in_party && party_menu.clipboard_link);
	visible_set(named(list, "party_button_copy", 0), member);
	visible_set(named(list, "party_button_follow", 0), member && party_leader_elsewhere(&status) &&
		party_menu.stage == _party_follow_idle);
	visible_set(named(list, "party_button_end", 0), member && status.leader && status.count > 1);
	visible_set(named(list, "party_button_leave", 0), in_party);
	focus_off_hidden(named(list, "party_button_bar", 0));
}

/* the leader's game found: joined (or, under way, its lobby shown first),
as the server browser joins its games */
static boolean party_join_found(struct widget_instance *widget, short controller, boolean *widget_deleted)
{
	struct advertised_game *game = party_found_game();

	if (!game)
	{
		party_menu.stage = _party_follow_finding;
		party_menu.stage_time = system_milliseconds();
		return campaign_fail();
	}
	/* (player 1's profile made the active one first: with none active yet,
	as when the game was started by a party's link, multiplayer_player's
	own reading of it finds none the first time. No profile to play with
	is said by multiplayer_player) */
	{
		struct player_profile profile;

		campaign_profile(controller, &profile);
	}
	if (!multiplayer_player(controller))
	{
		platform_log("menus: following the party's leader: no player profile to join with");
		party_follow_end(_p2p_party_follow_refused);
		return FALSE;
	}
	party_menu.stage = _party_follow_joining;
	party_menu.stage_time = system_milliseconds();
	if (advertised_in_progress(game))
	{
		csmemcpy(multiplayer.preview_key_id, game->key_id, sizeof(multiplayer.preview_key_id));
		csmemcpy(multiplayer.preview_xnaddr, game->xnaddr, sizeof(multiplayer.preview_xnaddr));
		return ui_widget_port_open(widget, PREVIEW_NAME, widget_deleted);
	}
	if (!ui_widget_port_join(widget, game, LOBBY_NAME, widget_deleted))
	{
		platform_log("menus: following the party's leader: the game could not be joined");
		party_follow_end(_p2p_party_follow_refused);
		return FALSE;
	}
	return TRUE;
}

/* "port party button" (each of the screen's buttons; the A press posted
once the leader's game is found joins it) */
static boolean party_screen_button(struct widget_instance *widget, short controller, boolean *widget_deleted)
{
	char text[TEXT_FIELD_LENGTH];

	if (party_menu.stage == _party_follow_ready)
		return party_join_found(widget, controller, widget_deleted);
	party_menu.controller = controller >= 0 && controller < 4 ? controller : 0;
	if (!strcmp(widget->name, "party_button_make"))
	{
		if (!config_boolean("network.online") || !p2p_party_make())
			return campaign_fail();
		party_menu.asked_here = TRUE;
	}
	else if (!strcmp(widget->name, "party_button_join"))
	{
		if (!platform_clipboard_get(text, sizeof(text)) || !p2p_party_join_link(text))
			return campaign_fail();
		party_menu.asked_here = TRUE;
	}
	else if (!strcmp(widget->name, "party_button_copy"))
	{
		if (!p2p_party_copy_link())
			return campaign_fail();
	}
	else if (!strcmp(widget->name, "party_button_follow"))
	{
		party_menu.result_order = 0;
		p2p_party_follow_again();
	}
	else if (!strcmp(widget->name, "party_button_end"))
	{
		p2p_party_disband();
	}
	else if (!strcmp(widget->name, "party_button_leave"))
	{
		party_follow_end(_p2p_party_follow_none);
		p2p_party_leave();
	}
	else if (!strcmp(widget->name, "party_button_back"))
	{
		event_manager_post_button(controller, BUTTON_B);
		return TRUE;
	}
	ui_play_audio_feedback_sound(SOUND_FORWARD);
	return TRUE;
}
