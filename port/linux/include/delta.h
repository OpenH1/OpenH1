/*
DELTA.H

From ChupathingyCE (https://github.com/ChupathingyCE/chupathingyce,
port/linux/include/delta.h at commit 724968e), under GPL-3.0-or-later.
OpenH1 keeps only what Delta Peer's handshake and platform badges need (its
major, the advertisement's tag, its port, the capability and platform
registries, the platform key); OpenH1's changes to it are marked "OpenH1:".

Delta is ChupathingyCE's network family (its docs/delta.md): what its
machines say to each other beside the game protocol OpenCE defines, which
stays OpenCE's byte for byte. OpenH1 speaks its first layer, Delta Peer's
handshake (port/linux/src/delta_peer.c), so that OpenH1 and the ports that
speak Delta know each other in a game; NETCODE.md, "Delta Peer", says how.

Plain C89 (no // comments, no stdint.h), as ChupathingyCE keeps it.
*/

#ifndef HALO_DELTA_H
#define HALO_DELTA_H

/* ---------- Delta Peer */

/* Delta Peer's major: raised only for a breaking change to its own framing
or handshake. Machines of different majors fall back to the legacy protocol
alone. */
#define DELTA_MAJOR 1

/* A host that speaks Delta Peer says so in its advertisement's 0x1A reserved
bytes with a tagged block at their TAIL (docs/delta.md, "The advertisement"):
OpenCE writes its own fields there from the front (the version, the flags,
since its version 37 the game's kind at byte 3; OpenH1 its rate, protocol
version and bots after them, halo_port_limits.h), so the tail stays clear of
them. The block: 'D', 'L', the host's Delta major, the block's layout
version (1), the host's own wire (its number, then its letter), its oldest
compatible wire (number, letter), and a check byte (0xA5 xor the eight
before). A reader knows Delta by a valid tag and check, never by a flag bit;
OpenCE's machines leave and ignore the bytes. */
#define DELTA_ADVERTISED_RESERVED_SIZE 0x1A
#define DELTA_ADVERTISED_TAG_SIZE 9
#define DELTA_ADVERTISED_TAG_OFFSET (DELTA_ADVERTISED_RESERVED_SIZE - DELTA_ADVERTISED_TAG_SIZE)
#define DELTA_ADVERTISED_TAG_FIRST 'D'
#define DELTA_ADVERTISED_TAG_SECOND 'L'
#define DELTA_ADVERTISED_TAG_VERSION 1
#define DELTA_ADVERTISED_TAG_CHECK_SEED 0xA5

/* A host listens for Delta Peer on its game's port plus this (5160 beside
the Xbox's 5150); a client sends from any port the system gives it. */
#define DELTA_PEER_PORT_OFFSET 10

/* The capability registry: one bit per optional feature. A bit's number is
never reused, a machine ignores the bits it doesn't know, and adding one
doesn't raise DELTA_MAJOR. OpenH1: the whole registry is kept, so that no
number is ever taken again by mistake; OpenH1 offers platform alone
(delta_peer.c's DELTA_PEER_OPENH1_CAPABILITIES). */
enum delta_capability
{
	/* each player's platform, for the scoreboard's badges */
	_delta_capability_platform = 0,
	/* a player's ID and profile revision (the profile is ChupathingyCE's
	site's) */
	_delta_capability_profile = 1,
	/* a host's messages to its players: welcome, notices, the next map */
	_delta_capability_server_messages = 2,
	/* text chat between Delta players */
	_delta_capability_chat = 3,
	/* Halo PC maps' identity (MAP) */
	_delta_capability_ce_maps = 4,
	/* retired before use: never set */
	_delta_capability_md_maps = 5,
	/* network co-op beyond OpenCE's */
	_delta_capability_coop = 6,
	/* AI sync beyond OpenCE's */
	_delta_capability_ai_sync = 7,
	/* map and game type votes */
	_delta_capability_vote = 8,
	/* retired before use: never set */
	_delta_capability_console_slots = 9,
	/* a dedicated server's moderators */
	_delta_capability_moderation = 10,
	/* Bomb game types and their player traits */
	_delta_capability_bomb = 11,
	/* a host answers pings before joining: announced on Delta List alone,
	never in a HELLO or WELCOME */
	_delta_capability_ping = 12,
	/* TRAITS' movement and melee traits */
	_delta_capability_bomb_movement = 13,
	/* TRAITS at its own number */
	_delta_capability_bomb_traits = 14,
	/* changing teams during a game */
	_delta_capability_team_change = 15,
	/* reserved for server badges (a draft in ChupathingyCE, not built
	there yet): never set until it is */
	_delta_capability_server_badge = 16,
	/* units' vitals and objects' names, as the host has them */
	_delta_capability_object_sync = 17,
	/* not a capability: the host's cheats on (Delta List alone) */
	_delta_capability_host_cheats = 18,
	/* the host's rules for its players' extras */
	_delta_capability_host_rules = 19,

	NUMBER_OF_DELTA_CAPABILITIES
};

/* The platform registry: one byte in every machine's platform key. A
number is never reused; new platforms are added at the end, and a port asks
ChupathingyCE's maintainers for its number rather than picking one
(docs/delta.md, "Platform numbers"). */
enum delta_platform
{
	_delta_platform_unknown = 0,
	_delta_platform_pc_windows = 1,
	_delta_platform_pc_macos = 2,
	_delta_platform_pc_linux = 3,
	_delta_platform_android = 4,
	_delta_platform_steam_deck = 5,
	_delta_platform_xbox = 6,
	_delta_platform_xbox360 = 7,
	_delta_platform_wiiu = 8,
	_delta_platform_switch = 9,
	_delta_platform_psp = 10,
	_delta_platform_vita = 11,
	_delta_platform_xbox_uwp = 12,
	_delta_platform_pc_freebsd = 13,
	_delta_platform_ps3 = 14,
	_delta_platform_ps4 = 15,
	_delta_platform_ps5 = 16,
	_delta_platform_n64 = 17,
	_delta_platform_pc_legacy = 18,
	_delta_platform_steamlink = 19,
	_delta_platform_ios = 20,
	_delta_platform_tvos = 21,

	NUMBER_OF_DELTA_PLATFORMS
};

/* OpenH1: the platform OpenH1's machines say they are. OpenH1 has asked
ChupathingyCE's maintainers for a number of its own; until it has one, it
says unknown (0), which plays everywhere with no badge and the unknown
platform's policy row (no limits). TODO: the number given, once it is. */
#define DELTA_OPENH1_PLATFORM _delta_platform_unknown

/* OpenH1: the wire an OpenH1 host's tag says it plays, its own and its
oldest: OpenCE's network version (HALO_PORT_NETWORK_VERSION) and this
letter. A ChupathingyCE client joins a host whose tag it reads by the wire
alone, whatever OpenCE number it announces (docs/delta.md, "The legacy
number"), so the tag says no more than an OpenCE host of that number plays:
the first wire of OpenCE's version, which a ChupathingyCE build of that
version joins, as it joins the OpenCE host by its number today. Only hosts
of games OpenCE's machines may join carry it (delta_peer_game.c). */
#define DELTA_OPENH1_WIRE_LETTER 'a'

/* A machine's platform key: its platform and what it can take, 8 bytes on
the wire in this order (delta_wire.c). Claims, never trusted for game
state. */
#define DELTA_PLATFORM_KEY_SIZE 8
/* the limits' layout; a machine reads the fields it knows of a newer one */
#define DELTA_PLATFORM_KEY_VERSION 1
/* ... the player opted in to games above join_players */
#define DELTA_PLATFORM_KEY_OPTED_IN 0x01
/* ... a dedicated server: hosts, and has no players of its own */
#define DELTA_PLATFORM_KEY_DEDICATED 0x02

struct delta_platform_key
{
	/* enum delta_platform */
	unsigned char platform;
	/* DELTA_PLATFORM_KEY_VERSION */
	unsigned char version;
	/* DELTA_PLATFORM_KEY_* */
	unsigned char flags;
	/* the most players a game this machine hosts can have (0: it does not
	host) */
	unsigned char host_players;
	/* the most players of a game it joins, by default; and with its
	player's opt-in (at least join_players) */
	unsigned char join_players;
	unsigned char join_players_opt_in;
	/* its memory, as log2 of megabytes; 0 unknown */
	unsigned char memory_class;
	unsigned char reserved;
};

/* OpenH1: the unknown platform's row of ChupathingyCE's platform policy
(DELTA_PLATFORM_POLICY there): the limits OpenH1's key claims, which a
Delta host applies to keep weaker machines safe. OpenH1 plays games of up
to 128 players, as its PCs and phones do; 0 says its memory is unknown. */
#define DELTA_OPENH1_HOST_PLAYERS 128
#define DELTA_OPENH1_JOIN_PLAYERS 128
#define DELTA_OPENH1_MEMORY_CLASS 0

#endif
