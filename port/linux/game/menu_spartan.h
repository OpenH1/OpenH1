/*
MENU_SPARTAN.H

The menus' Spartan (menu_spartan.c): drawn by the engine, from ui.map's
Spartan tags (menu_models.h), in a view of its own among the menus' widgets.
*/

#ifndef MENU_SPARTAN_H
#define MENU_SPARTAN_H

enum
{
	/* side by side, for the split screen's four players */
	MAXIMUM_MENU_SPARTANS = 4,
};

/* whether ui.map has the Spartan's tags (else the screens draw the
original pictures) */
boolean menu_spartan_available(void);
/* a Spartan asked for this frame (the screen that shows it asks each frame
it draws): placed in the menus' 640 by 480, in its screen's units (its
screen's place on the screen is menu_spartan_draw's), its feet's middle at
x, bottom, as tall as height, in the profile color color_index */
void menu_spartan_place(short index, short color_index, real x, real bottom, real height);
/* at the start of each frame the menus draw: the Spartans asked for made
and placed on their stage, the others deleted */
void menu_spartan_frame(void);
/* the Spartans' view drawn now, among the menus' widgets (above those
drawn before, under those after): their screen at x, y of the menus' 640
by 480 */
void menu_spartan_draw(short x, short y);
/* one of the Spartans' objects, or what one carries (the scene's view
leaves them out: render_objects.c) */
boolean menu_spartan_object(long object_index);
/* debug: HALO_MENU_SPARTAN_TEST's colors ("<color> ...", up to four), in
place of a <spartan>'s players' (0: not set) */
short menu_spartan_test_colors(short colors[MAXIMUM_MENU_SPARTANS]);

#endif
