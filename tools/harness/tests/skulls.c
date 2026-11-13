#include "harness.h"
#include "enums.inc"
/* The campaign's skulls (port/linux/game/skulls.c): which a game plays (a
campaign game of this computer's, a network co-op host's in an OpenH1 game
closed to OpenCE, a client's from the host's message, a demo's from its
recording; none in multiplayer or a Legacy game), and each hook in the
game's systems: an enemy's difficulty values (game_globals.c's
game_difficulty_get_team_value: Mythic, Catch), the damage modifiers (Tilt),
the AI's evasion chances (Tough Luck), the major upgrade (Thunderstorm), the
shields' recharge (Black Eye), the motion tracker and the HUD (Fog, Blind),
and the ammunition halved (Famine, weapons.c's weapon_halve_ammunition).
With no skull, every hook gives back the game's own value. */
typedef unsigned short word;
typedef word message_header;
#define GET_MESSAGE_TYPE(message) (((message) >> 2) & 3)
#define SET_FLAG(flags, bit, value) ((value) ? ((flags) |= FLAG(bit)) : ((flags) &= ~FLAG(bit)))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define _error_silent 0
#define error(kind, ...) ((void)0)
struct distributed_message_header { message_header header; byte type; byte count; long game_time; };

/* the world: the scenario, the connection, the settings */
struct scenario { short type; };
static struct scenario scenario = { _scenario_type_solo };
static struct scenario *global_scenario_get(void){return &scenario;}
static short connection = _game_connection_local;
static short game_connection(void){return connection;}
static boolean engine_running;
static boolean game_engine_running(void){return engine_running;}
static char const *setting = "";
static char const *config_string(char const *name){(void)name;return setting;}
static boolean hosting_standard, hosting_mixed;
static boolean network_standard_hosting(void){return hosting_standard;}
static boolean network_standard_hosting_mixed(void){return hosting_mixed;}
static boolean demo_playing; static unsigned long demo_skulls;
static boolean demo_playback_skulls(unsigned long *skulls){if (!demo_playing) return FALSE;*skulls=demo_skulls;return TRUE;}
static boolean unreliable;
static boolean network_connection_last_read_was_unreliable(void){return unreliable;}
/* the teams: the player and the human team are allies, the Covenant their enemy */
static boolean game_team_is_enemy(short a, short b)
{
 boolean a_human=a==_game_team_player||a==_game_team_human, b_human=b==_game_team_player||b==_game_team_human;
 return a_human!=b_human;
}
/* the players' units: 100 and 101 */
static long player_index_from_unit_index(long unit_index){return unit_index==100||unit_index==101?unit_index-100:NONE;}

/* the difficulty values: each value v at difficulty d is 1 + v + d/10 */
static short difficulty = _game_difficulty_level_hard;
static short game_difficulty_level_get(void){return difficulty;}
struct tag_block { long count; void *address; };
struct game_globals_difficulty_information { real values[NUMBER_OF_GAME_DIFFICULTY_VALUES][4]; };
struct game_globals { struct tag_block difficulty_information; };
static struct game_globals_difficulty_information difficulty_information;
static struct game_globals game_globals = { { 1, &difficulty_information } };
static struct game_globals *scenario_get_game_globals(void){return &game_globals;}
#define TAG_BLOCK_GET_ELEMENT(block, index, type) ((type *)(block)->address + (index))

/* a weapon: its magazines and triggers, its age */
struct weapon_magazine { short state, state_timer, original_time, rounds_total, rounds_loaded, rounds_fractional_recharged; };
struct weapon_trigger_definition { real age_generated_per_round; };
struct weapon_definition { struct { struct tag_block magazines; struct tag_block triggers; } weapon; };
struct weapon_datum { long definition_index; struct { real age; struct weapon_magazine magazines[2]; } weapon; };
static struct weapon_trigger_definition triggers[2];
static struct weapon_definition weapon_definition = { { { 1, NULL }, { 1, triggers } } };
static struct weapon_datum weapon;
static struct weapon_datum *weapon_get(long index){(void)index;return &weapon;}
static struct weapon_definition *weapon_definition_get(long index){(void)index;return &weapon_definition;}
static short weapon_magazine_count(struct weapon_definition const *definition){return (short)definition->weapon.magazines.count;}
static short weapon_trigger_count(struct weapon_definition const *definition){return (short)definition->weapon.triggers.count;}
static struct weapon_magazine *weapon_magazine_get(struct weapon_datum *datum, short index){return index<2?&datum->weapon.magazines[index]:NULL;}

#include "under_test.inc"

static real table(short value, short level){return 1.0f+value+level/10.0f;}

static void fill_table(void)
{
 short value, level;

 for (value=0;value<NUMBER_OF_GAME_DIFFICULTY_VALUES;value++)
  for (level=0;level<4;level++)
   difficulty_information.values[value][level]=table(value,level);
}

static void new_map(short with_connection, char const *with_setting)
{
 connection=with_connection;setting=with_setting;
 skulls_initialize_for_new_map();
}

static struct skulls_message message_of(word skulls)
{
 struct skulls_message message;
 memset(&message,0,sizeof(message));
 message.header.header=2<<2;message.header.type=_distributed_message_skulls;message.header.count=1;
 message.skulls=skulls;
 return message;
}

static void take(long machine_index, struct skulls_message message)
{
 network_skulls_handle_message(machine_index,(word const *)&message,sizeof(message));
}

static boolean near(real a, real b){return a-b<0.0001f&&b-a<0.0001f;}

int main(int argc,char **argv)
{
 const char *case_name=argc>1?argv[1]:"";
 char text[128];

 fill_table();
 CASE("names") {
  CHECK(skulls_parse("iron, mythic  blind,bogus")==(FLAG(_skull_iron)|FLAG(_skull_mythic)|FLAG(_skull_blind)),
   "the names, by spaces or commas; an unknown one left out");
  CHECK(skulls_parse("")==0&&skulls_parse(NULL)==0,"none");
  CHECK(skulls_parse("mythi ironclad")==0,"only whole names");
  skulls_format(SKULLS_PRESET_LASO,text,sizeof(text));
  CHECK(skulls_parse(text)==SKULLS_ALL,"every skull's name written and read back: %s",text);
  skulls_format(SKULLS_PRESET_MYTHIC,text,sizeof(text));
  CHECK(!strcmp(text,"mythic thunderstorm catch"),"the Mythic preset: %s",text);
  skulls_format(SKULLS_ALL,text,12);
  CHECK(strlen(text)<12,"a short buffer is never overrun: %s",text);
  CHECK(skull_name(NUMBER_OF_SKULLS)==NULL&&!strcmp(skull_name(_skull_tough_luck),"tough_luck"),"the names");
 } else CASE("local") {
  new_map(_game_connection_local,"mythic catch");
  CHECK(skulls_game()==(FLAG(_skull_mythic)|FLAG(_skull_catch)),"a campaign game of this computer's: game.skulls");
  CHECK(skull_active(_skull_mythic)&&!skull_active(_skull_iron),"those skulls alone");
  engine_running=TRUE;
  CHECK(!skull_active(_skull_mythic),"none with a game engine (multiplayer)");
  engine_running=FALSE;
  scenario.type=_scenario_type_multiplayer;
  new_map(_game_connection_local,"mythic catch");
  CHECK(skulls_game()==0,"none on a multiplayer map");
  scenario.type=_scenario_type_solo;
  new_map(_game_connection_local,"");
  CHECK(skulls_game()==0,"none by default");
 } else CASE("host") {
  hosting_standard=TRUE;
  new_map(_game_connection_network_server,"iron");
  CHECK(skulls_game()==FLAG(_skull_iron),"an OpenH1 co-op host's: its game.skulls");
  hosting_mixed=TRUE;
  new_map(_game_connection_network_server,"iron");
  CHECK(skulls_game()==0,"none in a game OpenCE's players may join");
  hosting_mixed=FALSE;hosting_standard=FALSE;
  new_map(_game_connection_network_server,"iron");
  CHECK(skulls_game()==0,"none in a Legacy game");
 } else CASE("client") {
  new_map(_game_connection_network_client,"blind");
  CHECK(skulls_game()==0,"a client's own game.skulls is not taken: none until the host's word");
  take(NONE,message_of(FLAG(_skull_fog)|FLAG(_skull_mythic)));
  CHECK(skulls_game()==(FLAG(_skull_fog)|FLAG(_skull_mythic)),"the host's skulls");
  new_map(_game_connection_network_client,"");
  CHECK(skulls_game()==0,"a new map's none until the host's word again");
 } else CASE("client_untrusted") {
  struct skulls_message message;
  new_map(_game_connection_network_client,"");
  take(3,message_of(FLAG(_skull_fog)));
  CHECK(skulls_game()==0,"not from another machine");
  unreliable=TRUE;take(NONE,message_of(FLAG(_skull_fog)));unreliable=FALSE;
  CHECK(skulls_game()==0,"not unreliably");
  take(NONE,message_of((word)(SKULLS_ALL+1)));
  CHECK(skulls_game()==0,"no skull past the last");
  message=message_of(FLAG(_skull_fog));message.reserved=1;take(NONE,message);
  CHECK(skulls_game()==0,"its reserved word empty");
  message=message_of(FLAG(_skull_fog));message.header.count=2;take(NONE,message);
  CHECK(skulls_game()==0,"one entry");
  network_skulls_handle_message(NONE,(word const *)&message,sizeof(message)-1);
  CHECK(skulls_game()==0,"whole");
  connection=_game_connection_network_server;take(NONE,message_of(FLAG(_skull_fog)));
  CHECK(skulls_game()==0,"never taken by a host");
 } else CASE("demo") {
  demo_playing=TRUE;demo_skulls=FLAG(_skull_tilt)|0x80000000UL;
  new_map(_game_connection_local,"iron");
  CHECK(skulls_game()==FLAG(_skull_tilt),"a demo plays its recording's skulls, not this computer's");
 } else CASE("off") {
  /* the negative control: with no skull, every hook gives back the game's own */
  short value;
  new_map(_game_connection_local,"");
  for (value=0;value<NUMBER_OF_GAME_DIFFICULTY_VALUES;value++)
  {
   CHECK(game_difficulty_get_team_value(value,_game_team_covenant)==table(value,difficulty),"an enemy's value %d",value);
   CHECK(skulls_enemy_difficulty_value(value,0.3f)==0.3f,"value %d unchanged",value);
  }
  CHECK(skulls_material_modifier(1.5f)==1.5f&&skulls_material_modifier(0.5f)==0.5f,"modifiers unchanged");
  CHECK(skulls_evasion_chance(0.25f)==0.25f&&skulls_evasion_chance(0.0f)==0.0f,"chances unchanged");
  CHECK(!skulls_force_major_upgrade(_game_team_covenant),"no forced upgrade");
  CHECK(skulls_shields_recharge(100),"a player's shields recharge");
  CHECK(!skulls_melee_recharges_shields(TRUE,100,_game_team_player,_game_team_covenant),"no melee recharge");
  CHECK(!skulls_motion_sensor_hidden()&&!skulls_hud_hidden(),"the HUD drawn");
  weapon.weapon.magazines[0].rounds_total=60;weapon.weapon.magazines[0].rounds_loaded=32;
  skulls_actor_dropped_weapon(7);
  CHECK(weapon.weapon.magazines[0].rounds_total==60&&weapon.weapon.magazines[0].rounds_loaded==32,"the ammunition kept");
 } else CASE("mythic") {
  new_map(_game_connection_local,"mythic");
  CHECK(near(game_difficulty_get_team_value(_game_difficulty_value_enemy_vitality,_game_team_covenant),
   2.0f*table(_game_difficulty_value_enemy_vitality,difficulty)),"an enemy's vitality doubled");
  CHECK(near(game_difficulty_get_team_value(_game_difficulty_value_enemy_shield,_game_team_flood),
   2.0f*table(_game_difficulty_value_enemy_shield,difficulty)),"an enemy's shields doubled");
  CHECK(game_difficulty_get_team_value(_game_difficulty_value_enemy_vitality,_game_team_player)==
   table(_game_difficulty_value_friend_vitality,difficulty),"the player's own, as without");
  CHECK(game_difficulty_get_team_value(_game_difficulty_value_enemy_shield,_game_team_human)==
   table(_game_difficulty_value_friend_shield,difficulty),"an ally's, as without");
  CHECK(game_difficulty_get_team_value(_game_difficulty_value_enemy_damage,_game_team_covenant)==
   table(_game_difficulty_value_enemy_damage,difficulty),"the enemy's damage, as without");
 } else CASE("catch") {
  new_map(_game_connection_local,"catch");
  CHECK(near(game_difficulty_get_team_value(_game_difficulty_value_grenade_chance_scale,_game_team_covenant),
   SKULLS_CATCH_GRENADE_CHANCE_SCALE*table(_game_difficulty_value_grenade_chance_scale,difficulty)),
   "an enemy's grenade chance raised");
  CHECK(near(game_difficulty_get_team_value(_game_difficulty_value_grenade_timer_scale,_game_team_covenant),
   SKULLS_CATCH_GRENADE_TIMER_SCALE*table(_game_difficulty_value_grenade_timer_scale,difficulty)),
   "an encounter's wait between throws cut");
  CHECK(game_difficulty_get_team_value(_game_difficulty_value_grenade_chance_scale,_game_team_human)==
   table(_game_difficulty_value_grenade_chance_scale,_game_difficulty_level_normal),"an ally's, as without");
 } else CASE("tilt") {
  new_map(_game_connection_local,"tilt");
  CHECK(near(skulls_material_modifier(1.5f),2.25f)&&near(skulls_material_modifier(0.5f),0.25f),
   "a strength stronger, a weakness weaker");
  CHECK(skulls_material_modifier(1.0f)==1.0f,"a neutral modifier as it was");
 } else CASE("tough_luck") {
  new_map(_game_connection_local,"tough_luck");
  CHECK(skulls_evasion_chance(0.25f)==1.0f,"every chance its definition gives taken");
  CHECK(skulls_evasion_chance(0.0f)==0.0f,"none where it has no chance");
 } else CASE("thunderstorm") {
  new_map(_game_connection_local,"thunderstorm");
  CHECK(skulls_force_major_upgrade(_game_team_covenant)&&skulls_force_major_upgrade(_game_team_flood),
   "the enemies upgraded");
  CHECK(!skulls_force_major_upgrade(_game_team_human),"the allies not");
 } else CASE("black_eye") {
  new_map(_game_connection_local,"black_eye");
  CHECK(!skulls_shields_recharge(100),"a player's shields do not recharge on their own");
  CHECK(skulls_shields_recharge(55),"an AI's still do");
  CHECK(skulls_melee_recharges_shields(TRUE,101,_game_team_player,_game_team_covenant),"a melee hit on an enemy");
  CHECK(!skulls_melee_recharges_shields(FALSE,101,_game_team_player,_game_team_covenant),"not a shot");
  CHECK(!skulls_melee_recharges_shields(TRUE,101,_game_team_player,_game_team_human),"not an ally");
  CHECK(!skulls_melee_recharges_shields(TRUE,55,_game_team_covenant,_game_team_player),"not an AI's melee");
  CHECK(!skulls_melee_recharges_shields(TRUE,NONE,_game_team_player,_game_team_covenant),"not no one's");
 } else CASE("fog_blind") {
  new_map(_game_connection_local,"fog");
  CHECK(skulls_motion_sensor_hidden()&&!skulls_hud_hidden(),"Fog: no motion tracker, the HUD drawn");
  new_map(_game_connection_local,"blind");
  CHECK(!skulls_motion_sensor_hidden()&&skulls_hud_hidden(),"Blind: no HUD");
 } else CASE("famine") {
  new_map(_game_connection_local,"famine");
  weapon.weapon.magazines[0].rounds_total=61;weapon.weapon.magazines[0].rounds_loaded=33;
  skulls_actor_dropped_weapon(7);
  CHECK(weapon.weapon.magazines[0].rounds_total==30&&weapon.weapon.magazines[0].rounds_loaded==16,
   "a magazine's rounds halved, rounded down");
  weapon_definition.weapon.magazines.count=0;weapon.weapon.age=0.2f;
  skulls_actor_dropped_weapon(7);
  CHECK(near(weapon.weapon.age,0.6f),"a battery's charge halved (0.8 left: 0.4)");
  skulls_actor_dropped_weapon(NONE);
 }
 else { fprintf(stderr,"unknown case %s\n",case_name); return 2; }
 return 0;
}
