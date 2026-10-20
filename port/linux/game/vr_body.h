/*
VR_BODY.H

The VR mode's body in a vehicle's seat (HALO_VR; first_person_legs.c), and
what it asks of the view (vr_render.c).
*/

#ifndef __VR_BODY_H
#define __VR_BODY_H

#ifdef HALO_VR

/* the seat the player's body is drawn in (vr_render_body_seat) */
struct vr_body_seat
{
	/* where the top of the real neck is with the head as it was recentred:
	below and behind the seat's eye (or its cabin's), in the world */
	real_point3d rest_neck;
	/* the way the body faces, level, and its left: the vehicle's forward,
	or standing, the body's heading (vr.c's) */
	real_vector3d forward;
	real_vector3d left;
	/* the way the game poses the body facing, level: the vehicle's forward */
	real_vector3d pose_forward;
	/* it stands (the Scorpion's hatch: vr_cabin.c), its legs hanging
	straight down; else it sits as the game poses it */
	boolean standing;
	/* the VR mode draws arms of its own in the seat (vr_hands.c: the hands
	alone, or the first-person weapon's), so the body's are folded away */
	boolean arms;
};

#endif

#endif
