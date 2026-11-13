#ifndef NETWORK_SKULLS_H
#define NETWORK_SKULLS_H
#include "cseries.h"
/* the campaign's skulls in network co-op (network_skulls.c): the host's, sent
to its OpenH1 clients */
void network_skulls_reset(void);
void network_skulls_update(void);
void network_skulls_send(long machine_index);
boolean network_skulls_handles_message(word const *message, word size);
void network_skulls_handle_message(long machine_index, word const *message, word size);
#endif
