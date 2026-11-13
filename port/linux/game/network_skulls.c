/* The campaign's skulls in network co-op (skulls.c): the host's, taken as
its map began, play on every machine, which must agree on them (the
enemies' vitality, the HUD drawn). The host sends them to each client
reliably when it is ready and whenever they change from what it last sent;
a client plays none until they come. The message is one of OpenH1's own
(FIRST_OPENH1_DISTRIBUTED_MESSAGE and after), which never goes to an OpenCE
machine; a host plays skulls only in an OpenH1 game that no OpenCE machine
can join, and a Legacy host sends nothing, so its clients play stock. */
#include "cseries.h"
#include "game/game.h"
#include "interface/ui_widget.h"
#include "main/main.h"
#include "networking/network_game_globals.h"
#include "network_distributed.h"
#include "network_skulls.h"
#include "skulls.h"
#include <string.h>

static unsigned long sent_skulls = (unsigned long)NONE;

struct skulls_message
{
	struct distributed_message_header header;
	/* (a bit each, skulls.h's order) */
	word skulls;
	word reserved;
};

void network_skulls_reset(void)
{
	sent_skulls = (unsigned long)NONE;
}

static void skulls_message_make(struct skulls_message *message, unsigned long skulls)
{
	memset(message, 0, sizeof(*message));
	message->skulls = (word)skulls;
}

void network_skulls_send(long machine_index)
{
	struct skulls_message message;

	if (game_connection() != _game_connection_network_server)
		return;
	skulls_message_make(&message, skulls_game());
	distributed_send_to_machine_reliably(machine_index, &message, _distributed_message_skulls, 1, sizeof(message));
}

void network_skulls_update(void)
{
	struct skulls_message message;
	unsigned long skulls;

	if (!game_in_progress() || main_menu_is_active() || game_connection() != _game_connection_network_server)
		return;
	skulls = skulls_game();
	if (sent_skulls == skulls)
		return;
	sent_skulls = skulls;
	skulls_message_make(&message, skulls);
	distributed_send(&message, _distributed_message_skulls, 1, sizeof(message), _distributed_to_clients_reliably);
}

boolean network_skulls_handles_message(word const *message, word size)
{
	struct distributed_message_header header;

	if (size < sizeof(header))
		return FALSE;
	memcpy(&header, message, sizeof(header));
	return header.type == _distributed_message_skulls;
}

/* (the host's alone, over its stream, whole, and no skull past the last) */
void network_skulls_handle_message(long machine_index, word const *data, word size)
{
	struct skulls_message message;

	if (machine_index != NONE || game_connection() != _game_connection_network_client ||
		network_connection_last_read_was_unreliable() || size != sizeof(message))
		return;
	memcpy(&message, data, sizeof(message));
	if (GET_MESSAGE_TYPE(message.header.header) != 2 || message.header.type != _distributed_message_skulls ||
		message.header.count != 1 || (message.skulls & ~SKULLS_ALL) != 0 || message.reserved != 0)
		return;
	skulls_set_game(message.skulls);
}
