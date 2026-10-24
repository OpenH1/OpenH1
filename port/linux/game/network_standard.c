/*
NETWORK_STANDARD.C

OpenH1's two kinds of network game (NETCODE.md, "Standard and Legacy
games"):

- Legacy: OpenCE's netcode at HALO_PORT_NETWORK_VERSION, at 30 ticks a
  second, which OpenCE builds join. Nothing of it changes here: its
  advertisement, listing, join token and game record are as they were.
- Standard: OpenH1's own (network.game_mode "standard", the default when
  hosting), whose host picks the simulation's rate (network.sim_rate: 30,
  60 or 120, sim_rate.h). Its host advertises
  HALO_PORT_STANDARD_ADVERTISED_VERSION where a Legacy host advertises the
  network version, and the rate beside it (HALO_PORT_ADVERTISED_RATE_OFFSET,
  the internet listing's stamp), so a build that knows nothing of Standard
  games (OpenCE, an older OpenH1) takes it for a newer version and does
  not join. Its host takes a join only with a Standard game's join token
  (network_standard_join_token), so nothing else gets in either way.

The rate is agreed before the game's first tick and holds for its length:
the host reads network.sim_rate in the lobby, and takes the last it read as
the game starts (network_game_server_start_network_game sends the game
record then); the record carries it to every machine, a late joiner's
too, in a short the Xbox game left unused (struct network_game's
standard_rate, which a Legacy host sends as it always has). sim_rate.c's
sim_rate_network is the one place that turns it into the rate a tick
takes.
*/

#include "cseries.h"
#include "game/game.h"
#include "networking/network_game_manager.h"
#include "networking/network_game_globals.h"
#include "sim_rate.h"
#include "network_standard.h"
#include "halo_port_limits.h"

/* port/linux/src/port_config.c */
char const *config_string(char const *name);
long config_integer(char const *name);

/* ---------- globals */

static struct
{
	/* hosting: Standard (network.game_mode as the server was made), and
	the rate as last read in the lobby */
	boolean hosting;
	long hosting_rate;
	/* joining: the game joined is Standard (its advertisement said so) */
	boolean joined;
} network_standard_globals = { FALSE, SIM_RATE_CLASSIC, FALSE };

/* ---------- private code */

/* network.sim_rate, a rate sim_rate.c steps at, else 30 */
static long network_standard_rate_setting(
	void)
{
	long rate = config_integer("network.sim_rate");

	return sim_rate_supported(rate) ? rate : SIM_RATE_CLASSIC;
}

/* ---------- public code */

void network_standard_server_created(
	void)
{
	char const *mode = config_string("network.game_mode");

	/* (anything but "legacy" is the default, Standard) */
	network_standard_globals.hosting = !(mode && !csstrcmp(mode, "legacy"));
	network_standard_globals.hosting_rate = network_standard_rate_setting();
}

boolean network_standard_hosting(
	void)
{
	return network_standard_globals.hosting;
}

long network_standard_server_rate(
	boolean in_lobby)
{
	if (!network_standard_globals.hosting)
		return 0;
	if (in_lobby)
		network_standard_globals.hosting_rate = network_standard_rate_setting();
	return network_standard_globals.hosting_rate;
}

void network_standard_stamp_game(
	struct network_game *game,
	boolean in_lobby)
{
	if (game && network_standard_globals.hosting)
		game->standard_rate = (short)network_standard_server_rate(in_lobby);
}

void network_standard_join_token(
	byte *join_token,
	long token_size)
{
	/* (its last two bytes: "S" and the protocol's version) */
	if (join_token && token_size >= 2)
	{
		join_token[token_size - 1] ^= 'S';
		join_token[token_size - 2] ^= (byte)HALO_PORT_STANDARD_NETWORK_VERSION;
	}
}

void network_standard_client_joining(
	boolean standard,
	byte *join_token,
	long token_size)
{
	network_standard_globals.joined = standard;
	if (standard)
		network_standard_join_token(join_token, token_size);
}

long network_standard_agreed_rate(
	void)
{
	switch (game_connection())
	{
	case _game_connection_network_server:
		return network_standard_globals.hosting ? network_standard_globals.hosting_rate : 0;
	case _game_connection_network_client:
	{
		struct network_game *game = network_game_get_game();

		/* (the host's word: a rate this machine steps at, else 30) */
		if (!network_standard_globals.joined || !game)
			return 0;
		return sim_rate_supported(game->standard_rate) ? game->standard_rate : SIM_RATE_CLASSIC;
	}
	default:
		return 0;
	}
}
