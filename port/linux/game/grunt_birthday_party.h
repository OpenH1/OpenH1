/*
GRUNT_BIRTHDAY_PARTY.H

The Grunt Birthday Party skull's confetti and cheer (grunt_birthday_party.c;
skulls.c decides when they burst).
*/

#ifndef GRUNT_BIRTHDAY_PARTY_H
#define GRUNT_BIRTHDAY_PARTY_H
#include "cseries.h"

/* the confetti's tags made for a campaign map as its tags load
(cache_files.c's scenario_tags_load, before the menus' tags), where the
skull may play in its game; and let go as they unload, after the menus' */
void grunt_birthday_party_tags_loaded(void);
void grunt_birthday_party_tags_unloaded(void);

/* whether a unit is a grunt: its unit is one a grunt's actor variant
places (the map's tags, the same on every machine) */
boolean grunt_birthday_party_grunt(long unit_index);
/* a grunt's head burst into confetti, with the grunts' cheer: TRUE where
either was made */
boolean grunt_birthday_party_burst(long unit_index);

/* (for the checks) the confetti's effect and particle tags, NONE where the
map has none */
long grunt_birthday_party_effect(void);
long grunt_birthday_party_particle(void);

#endif
