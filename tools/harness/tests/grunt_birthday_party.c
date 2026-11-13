#include "harness.h"
#include <math.h>
#include "enums.inc"
/* The Grunt Birthday Party skull's confetti (port/linux/game/grunt_birthday_party.c):
whether a map's tags get it (a campaign map, the skull chosen here, a demo
recorded with it, or a network client, whose host's skulls come later; never
a multiplayer map), which units are grunts (those a grunt actor variant
places, by the map's tags, as every machine has them), and the size the
tools give a particle of a bitmap's sprites (1 over the largest sprite's
pixels: render_particles.c). */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define NUMBEROF(a) ((long)(sizeof(a) / sizeof((a)[0])))
#define OBJECT_DEFINITION_TAG 'obje'

/* the map's tags: its table (a scenario) */
struct scenario { short type; };
struct cache_file_tag_instance { long group_tag; void *base_address; };
struct confetti_tags;
static struct scenario scenario = { _scenario_type_solo };
static struct cache_file_tag_instance table[] = { { 'bitm', &scenario }, { 'scnr', &scenario } };
static void *cache_files_tag_instances(long *count){*count=NUMBEROF(table);return table;}

/* the machine: its connection, a demo played, the skulls chosen */
static short connection = _game_connection_local;
static short game_connection(void){return connection;}
static boolean demo_playing; static unsigned long demo_skulls;
static boolean demo_playback_skulls(unsigned long *skulls){if (!demo_playing) return FALSE;*skulls=demo_skulls;return TRUE;}
static unsigned long chosen;
static unsigned long skulls_chosen(void){return chosen;}

/* the actor variants (tags 100-104), their actors (200-202: grunt, elite, a
tag that is no actor) and units (300-302) */
struct tag_reference { long index; };
struct actor_variant_definition { struct tag_reference actor_reference, unit_reference; };
struct actor_definition { short type; };
struct tag_iterator { long next; };
static struct actor_variant_definition variants[] =
{
	{ { 200 }, { 300 } }, /* a grunt */
	{ { 200 }, { 300 } }, /* a grunt major, the same unit */
	{ { 201 }, { 301 } }, /* an elite */
	{ { 202 }, { 302 } }, /* a variant of no actor */
	{ { 200 }, { NONE } }, /* a grunt of no unit */
};
static struct actor_definition actors[] = { { _actor_grunt }, { _actor_elite } };
static void tag_iterator_new(struct tag_iterator *iterator, long group){(void)group;iterator->next=0;}
static long tag_iterator_next(struct tag_iterator *iterator){return iterator->next<NUMBEROF(variants)?100+iterator->next++:NONE;}
static struct actor_variant_definition *actor_variant_definition_get(long index){return &variants[index-100];}
static struct actor_definition *actor_definition_get(long index){return &actors[index-200];}
static boolean tag_index_is_group(long index, long group)
{
	if (group=='actr') return index==200||index==201;
	if (group==OBJECT_DEFINITION_TAG) return index>=300&&index<=302;
	return FALSE;
}

/* the objects: 1 of unit 300 (a grunt), 2 of 301 (an elite) */
struct object_datum { long definition_index; };
static struct object_datum objects[] = { { 0 }, { 300 }, { 301 } };
static struct object_datum *object_try_and_get(long index){return index>=1&&index<=2?&objects[index]:NULL;}

/* a bitmap: its sprites in the first sequence, on a 64 by 64 bitmap */
typedef struct { real x0, x1, y0, y1; } real_rectangle2d;
struct tag_block { long count; void *address; };
struct bitmap_data { short width, height; };
struct bitmap_group_sprite { short bitmap_index; real_rectangle2d bounds; };
struct bitmap_group_sequence { struct tag_block sprites; };
struct bitmap_group { struct tag_block sequences, bitmaps; };
#define TAG_BLOCK_GET_ELEMENT(block, index, type) ((type *)(block)->address + (index))
static struct bitmap_data bitmap_datas[] = { { 64, 64 } };
static struct bitmap_group_sprite sprites[] =
{
	{ 0, { 0.3125f, 0.640625f, 0.6875f, 0.90625f } }, /* 21 by 14 */
	{ 0, { 0.25f, 0.46875f, 0.0f, 0.359375f } }, /* 14 by 23: the largest, its height */
	{ 1, { 0.0f, 1.0f, 0.0f, 1.0f } }, /* of no bitmap of the group's */
};
static struct bitmap_group_sequence sequences[] = { { { 3, sprites } } };
static struct bitmap_group bitmap = { { 1, sequences }, { 1, bitmap_datas } };
static struct bitmap_group empty_bitmap = { { 0, NULL }, { 1, bitmap_datas } };
static struct bitmap_group const *bitmap_group_get(long index){return index==1?&bitmap:&empty_bitmap;}

#include "under_test.inc"

static boolean near(real a, real b){return a-b<0.00001f&&b-a<0.00001f;}

int main(int argc,char **argv)
{
 const char *case_name=argc>1?argv[1]:"";

 CASE("may_play") {
  chosen=FLAG(_skull_grunt_birthday_party);
  CHECK(grunt_birthday_party_may_play(),"a campaign game with the skull chosen");
  chosen=FLAG(_skull_mythic);
  CHECK(!grunt_birthday_party_may_play(),"none with it not chosen");
  connection=_game_connection_network_client;
  CHECK(grunt_birthday_party_may_play(),"a network client, whose host's skulls come after its map loads");
  connection=_game_connection_network_server;
  CHECK(!grunt_birthday_party_may_play(),"a host by its own choice");
  connection=_game_connection_local;
  demo_playing=TRUE;demo_skulls=FLAG(_skull_grunt_birthday_party);chosen=0;
  CHECK(grunt_birthday_party_may_play(),"a demo recorded with it");
  demo_skulls=0;chosen=FLAG(_skull_grunt_birthday_party);
  CHECK(!grunt_birthday_party_may_play(),"a demo recorded without it, whatever is chosen");
  demo_playing=FALSE;
  scenario.type=_scenario_type_multiplayer;
  CHECK(!grunt_birthday_party_may_play(),"never a multiplayer map");
 } else CASE("grunts") {
  grunt_units_find();
  CHECK(grunt_birthday_party.grunt_unit_count==1&&grunt_birthday_party.grunt_units[0]==300,
   "the grunts' unit, once: %d",grunt_birthday_party.grunt_unit_count);
  CHECK(grunt_birthday_party_grunt(1),"a grunt");
  CHECK(!grunt_birthday_party_grunt(2),"not an elite");
  CHECK(!grunt_birthday_party_grunt(7),"not an object that is none");
 } else CASE("sprite_size") {
  CHECK(near(confetti_inverse_sprite_size(1),1.0f/23.0f),"1 over the largest sprite's pixels: %f",
   confetti_inverse_sprite_size(1));
  CHECK(confetti_inverse_sprite_size(2)==0.0f,"none for a bitmap of no sprites");
 }
 else { fprintf(stderr,"unknown case %s\n",case_name); return 2; }
 return 0;
}
