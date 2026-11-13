/*
MENU_SPARTAN.H

The menus' Spartan in the menus' scene (menu_spartan.c): drawn by the
engine, from ui.map's Spartan tags (menu_models.h).
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
it draws): its feet at x, y in the view (-1 to 1 across and up), as tall as
height of the view's height (0 to 2), in the profile color color_index */
void menu_spartan_place(short index, short color_index, real x, real y, real height);
/* ... placed in the menus' 640 by 480 (centred on the screen at every
aspect ratio, as the menus' widgets are): its feet's middle at x, bottom,
as tall as height */
void menu_spartan_place_in_menus(short index, short color_index, real x, real bottom, real height);
/* at the start of each frame the menus draw: the Spartans asked for made
and placed before the camera, the others deleted */
void menu_spartan_frame(void);

#endif
