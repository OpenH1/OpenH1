/*
BATTERY_SAVER.H

display.battery_saver (battery_saver.c): the game draws less while it is on,
for a longer battery and a cooler machine.
*/

#ifndef BATTERY_SAVER_H
#define BATTERY_SAVER_H

/* (between frames: halo_screen_commit) display.battery_saver read, and for
"auto" whether the machine runs on its battery; battery saver turned on or
off as they say */
void battery_saver_update(void);
/* battery saver is on */
int battery_saver_active(void);

#endif
