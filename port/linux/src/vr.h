/*
VR.H

The VR mode (HALO_VR; port/linux/README.md, "VR"): what the game's code
(port/linux/game/vr_render.c, render.c, player_control.c), the renderer
(d3d8_device.c, xgpu_gl.c) and the input (xinput_sdl.c) ask of vr.c. Plain C
types only: the game's code includes it as well as the platform layer.

The game draws one view from the centre of the head, its field of view the
union of both eyes', and the renderer draws each of its draws into both eyes
at once (GL_OVR_multiview2), each vertex's clip position moved from the
centre view's to its eye's by a fixed transform per eye (halo_vr_set_eye_
transforms). The HUD and the menus go to a layer of their own.

Game axes: forward, left, up (the game's x, y, z at a yaw of 0).
*/

#ifndef __HALO_VR_H
#define __HALO_VR_H

/* the renderer's passes (d3d8_device.c, halo_vr_pass) */
enum
{
	HALO_VR_PASS_NONE = 0, /* not drawing a VR frame: the flat window */
	HALO_VR_PASS_EYES,     /* the 3D view, into both eyes */
	HALO_VR_PASS_HUD,      /* the HUD and the menus, into the quad layer */
	HALO_VR_PASS_SCOPE,    /* the view in a weapon's scope, into its own image */
};

struct halo_vr_view
{
	/* the head, from where and how it was at the last recentring (game
	axes, metres) */
	float head_forward[3];
	float head_up[3];
	float head_position[3];
	/* each eye from the head's centre, along the head's axes (metres),
	and its field of view (tangents: left, right, up, down) */
	float eye_offset[2][3];
	float fov[2][4];
	/* the turn of the controllers' stick (radians, the game's yaw) */
	float body_yaw;
	/* the body's heading (radians, game axes from the recentred head: the
	head's, which it follows only past a turn of the head, and slowly), the
	one of the holsters, the body drawn under the head and the arms'
	shoulders; and whether it is known (a head tracked since recentring) */
	int body_heading_valid;
	float body_heading;
	/* where the body hangs from the head (game axes from the recentred
	head, metres): the top of the neck, which the head nods and turns about,
	and the eyes as they would be from it with the head level and turned to
	the body's heading (the holsters and the arms' shoulders are placed from
	them) */
	float body_neck[3];
	float body_eyes[3];
	/* the HUD's layer: the tangent of half its vertical field of view, so
	that its camera's projection matches (waypoints, the reticle) */
	float hud_tangent;
	float world_units_per_metre;
	/* the right controller's pointing pose, as the head's (vr.aim =
	"controller": the aim and the first-person weapon follow it) */
	int controller_aim;
	float hand_forward[3];
	float hand_up[3];
	float hand_position[3];
	/* where the aim points from hand_position: the line the reticle is
	drawn along and the game is aimed by (halo_vr_aim_line), the weapon's
	pointing steadied while the smart-link magnifies it */
	float aim_forward[3];
	/* where the weapon's hand is from the eye it is drawn for (metres:
	right, up, forward) */
	float weapon_offset[3];
	/* the first-person hands follow the controllers (vr.hands = "arms" or
	"floating", with vr.aim = "controller"), and arms reach them from the
	shoulders (arms): where each hand is (the controllers' grip poses'
	positions, as hand_position), the left controller's pointing pose (as
	the right's), how far the left hand holds the long gun's foregrip
	(vr.two_handed: 0 to 1, eased), the right trigger's and the left grip's
	pull (0 to 1), the right index finger on the trigger (0, off it, to 1,
	on it: eased, as the trigger senses the finger; 1 where it senses none),
	and the shoulders from the eyes (vr.shoulders: metres
	out to each side, up, forward) */
	int floating_hands;
	int arms;
	float right_grip_position[3];
	int left_hand_valid;
	float left_hand_forward[3];
	float left_hand_up[3];
	float left_grip_position[3];
	float two_handed;
	float right_trigger;
	float right_trigger_touch;
	float left_grip;
	float shoulder_offset[3];
	/* the time from the last frame's display to this one's (seconds) */
	float seconds;
	/* in a vehicle's seat, the view is the seat's (vr.vehicle_view =
	"first_person"): from the player's head where it sits, else the game's
	camera following the vehicle */
	int vehicle_first_person;
	/* vr.aim = "gamepad": the gamepad aims as on the flat screen, and
	body_yaw turns after the aim (with it or in steps, vr.gamepad_view); the
	first-person weapon and the reticle follow the player's facing */
	int gamepad_aim;
	/* vr.gamepad_view = "camera": the view is pitched with the facing too
	(before the head's turn), as the flat screen's camera is */
	int gamepad_pitch;
	/* in a vehicle's seat with handles (vr_vehicle.c: a Ghost's handlebars, a
	Warthog's wheel, its turret's handles): the hands are on them, each hand
	that holds its handle placed there (its grip's place and its pointing the
	handle's, its fingers closed about it), the other where its controller
	is, and no weapon drawn */
	int handlebars;
	/* the HUD on the visor (vr.hud = "visor"): curved about the eyes, laid
	out in angles, hud_half_angles across and up (radians); its camera
	looks along hud_forward and hud_up (game axes from the recentred head:
	the head's, a moment behind it with vr.hud_lag) */
	int hud_visor;
	float hud_half_angles[2];
	float hud_forward[3];
	float hud_up[3];
};

/* the player as the visor shows it (vr.hud = "visor", vr.visor_effects),
told once a frame (vr_render.c) */
struct halo_vr_visor_state
{
	/* the player's unit is alive and plays (no cinematic); the player is
	dead, the view the death's (vr.death_view: the visor's last flare as it
	goes) */
	int active;
	int dead;
	/* its shields (0 to 1, more when overcharged; has_shield: the game has
	them), recharging, and its body's health (0 to 1) */
	int has_shield;
	float shield;
	int charging;
	float body;
	/* the shields' look on the armour, from the unit's own modifier shader
	(a plasma: characters\cyborg\shaders\shield hit for the Chief): its
	glow now (0 to 1: the game's intensity for it, from the unit's function
	that drives it, as the hits light it up), its colours facing and
	grazing, and its two noise maps' scales, periods (seconds) and
	directions, the noise map's texture (a D3DBaseTexture; NULL: none), and
	the game's time (seconds) that moves them */
	int look;
	float glow;
	float perpendicular[3], parallel[3];
	float noise_scale[2], noise_period[2], noise_direction[2][3];
	void *noise_texture[2];
	float time;
};

/* the HUD's elements in groups, each drawn at its own depth on the visor
(vr_visor.c): by where the game anchors them (hud_draw.c), and what it
places in the world (the waypoints, the players' names) */
enum
{
	HALO_VR_HUD_GROUP_CENTRE = 0, /* the crosshair, the messages, the rest */
	HALO_VR_HUD_GROUP_WORLD,      /* the waypoints and the players over their heads */
	HALO_VR_HUD_GROUP_WEAPON,     /* top left: the weapon, its ammunition, the grenades */
	HALO_VR_HUD_GROUP_STATUS,     /* top right: the shields and the health */
	HALO_VR_HUD_GROUP_TRACKER,    /* bottom left: the motion tracker */
	HALO_VR_HUD_GROUPS,
};

/* the grenade thrown with the left hand (vr.grenade_throw = "gesture"):
the left grip holds it, a swing starts the game's throw (as the left trigger
does), and letting go releases it (port/linux/game/vr_grenade.c) */
enum
{
	HALO_VR_THROW_NONE = 0,
	HALO_VR_THROW_SWING,    /* the hand swings, holding the grenade */
	HALO_VR_THROW_RELEASED, /* the hand has let go of it */
};

struct halo_vr_throw
{
	int phase;
	/* the hand has a grenade: taken (the grip held, before any swing),
	swinging, or let go of and not yet thrown by the game (drawn in it,
	vr_grenade.c) */
	int held;
	/* where the grenade goes: the hand's velocity (as it let go, else as
	it swings now), lifted a little, as a direction in the game's axes from
	the recentred head (turned by body_yaw in the world) and as the
	player's facing (yaw: body_yaw added; pitch) */
	float direction[3];
	float yaw, pitch;
	/* the throw's speed, of the game's (0 to 1), and the hand's (metres a
	second) */
	float power;
	float speed;
	/* where the hand let go, from the recentred head (game axes, metres) */
	float position[3];
	float body_yaw;
	float world_units_per_metre;
	/* the first-person hands are the controllers' (vr.hands = "arms",
	"floating"): the left one throws as the hand does, the weapon stays in
	the right */
	int hands;
	/* debug.vr_throw_log: the game logs its side of each throw too */
	int log;
};

/* the holsters (vr.holsters, port/linux/game/vr_holster.c): the weapon the
player carries but does not hold is on the body, a long gun over the right
shoulder, a pistol at the right hip; the right hand reaching there and
gripping draws it (the game's weapon switch, as Y) */
enum
{
	HALO_VR_HOLSTER_NONE = 0,
	HALO_VR_HOLSTER_BACK, /* over the right shoulder: a long gun */
	HALO_VR_HOLSTER_HIP,  /* at the right hip: a pistol, the needler */
	HALO_VR_HOLSTERS,
};

struct halo_vr_holsters
{
	/* the holsters are on (vr.holsters, the controller aiming) */
	int enabled;
	/* the body's heading (halo_vr_view's body_heading), and where each
	holster is (game axes from the recentred head, metres) */
	float body_yaw;
	float place[HALO_VR_HOLSTERS][3];
	/* the holster the right hand is in, if any (HALO_VR_HOLSTER_*), and how
	far the hand is from each one's reach (metres; 0 within it, large when
	the hand is not tracked): the stowed weapon lights up as it comes */
	int reach;
	float distance[HALO_VR_HOLSTERS];
};

/* the magnified view in a weapon's scope (vr.scopes, vr.scope_view,
port/linux/game/vr_scope.c): raised to an eye, its image on a quad at the
scope's eyepiece, facing back along the scope, seen by that eye alone;
smart-linked, a window about the reticle, at its distance, facing back along
the aim from between the eyes, drawn into both eyes' images (xgpu_gl.c) */
struct halo_vr_scope
{
	/* the eye (0 left, 1 right; -1 both: the smart-link's) */
	int eye;
	/* the lens's middle, the way the scope points and its up (game axes
	from the recentred head, metres), and the lens's width (metres) */
	float position[3];
	float forward[3];
	float up[3];
	float size;
	/* the zoom shown in the lens, as the game's HUD labels the zoom level
	(0: unzoomed, none shown), and how tall (of the lens's width) */
	float zoom;
	float zoom_height;
	/* the smart-link's: where its reticle (the aim) is, right of and above
	the view's middle, as fractions of its radius (its middle: the view is
	drawn toward the aim, beside it) */
	float reticle[2];
	/* the smart-link's window: the quad's height of its width (size: the
	window and the glow about it; the image's square holds the quad in its
	middle rows), and the window's half width and half height within it (of
	the quad's half width; 0: no window);
	how far the aim meets the world from the weapon (metres; 0: nothing in
	range); the rounds in the weapon and carried besides (-1: none to show) */
	float height;
	float window[2];
	float range;
	int rounds_loaded;
	int rounds_carried;
	/* its animation: how far it has opened (its eased progress, 0 to 1:
	the smart-link connecting), how far it has closed (0 to 1; 0: not
	closing), and the seconds since the zoom last changed level while open */
	float opened_seconds;
	float closing;
	float switched_seconds;
	/* the smart-link's window drawn as the flat screen's zoomed view (the
	weapon's scope's own mask and HUD in its image, vr_scope.c): the
	corner's radius of its rounded window (of the image's half width: as
	large as the window's half extents, a circle; less than 0: the
	smart-link's own look instead), how far the world about it is dimmed
	(0 to 1), and how wide its edge fades into the world (of the image's
	half width; 0: sharp) */
	float corner;
	float dim;
	float feather;
};

/* xgpu_gl.c: the smart-link's window as each eye sees it, in the eye's
space (OpenXR's view space: x right, y up, z back; metres): its middle, its
right and up, the quad's half width and half height, and the eye's field of
view (tangents: left, right, up, down); the window within the quad (of the
quad's half width and half height); and the eyes' depth range (metres) */
struct halo_vr_scope_window
{
	float centre[2][3];
	float right[2][3];
	float up[2][3];
	float half[2];
	float fov[2][4];
	float window[2];
	float near_z;
	float far_z;
};

/* a feed cockpit's display (vr.scorpion_view = "feed", port/linux/game/
vr_cabin.c): an opening in the cockpit's front wall, which curves about an
upright axis behind the eye, the world seen through it; xgpu_gl.c draws the
feed's look over it. The axis at the display's middle height, the way to the
display's middle from it and the axis's up (game axes from the recentred
head, metres), the wall's radius, the display's half angle about the axis
(radians) and its half height (metres); how far the feed has washed over it
(0: the world through the opening, as it is; 1: all the feed) and how far
it has gone dark (0 on; 1 dark, as a screen switched off); the range
where the aim meets the world (metres; 0: nothing in range) and the rounds of
the vehicle's weapon and its second (-1: none) */
struct halo_vr_cockpit_display
{
	float position[3];
	float forward[3];
	float up[3];
	float radius;
	float half_angle;
	float half_height;
	float wash;
	float off;
	float range;
	int rounds_loaded;
	int rounds_carried;
	int second_loaded;
	int second_carried;
};

/* climbing a vehicle by its holds (vr.vehicle_climb, port/linux/game/
vr_climb.c): told the holds near (each a line: its ends, game axes from the
recentred head, metres, and how far about it a hand takes it); given each
hand's hold (-1 none), the nearest hold to each hand and how far it is
(metres beyond its reach; 0 or less within it), and how far the eye moves
this frame by the hands (game axes, metres: against the holding hands'
motion); and each hand's place (game axes from the recentred head, metres),
whether both hands hold the seat's own handles (halo_vr_seat_place: they
steer), and told, whether a hand whose grip is held takes a hold within
reach as a press would (grab: the handles it pushed off); and the frame
(presented: debug.vr_test_hands poses by it) */
#define HALO_VR_CLIMB_HOLDS 24
struct halo_vr_climb
{
	int count;
	float ends[HALO_VR_CLIMB_HOLDS][2][3];
	float radius[HALO_VR_CLIMB_HOLDS];
	int held[2];
	int nearest[2];
	float distance[2];
	float moved[3];
	float hand[2][3];
	int seat_held;
	int grab;
	unsigned long frame;
};

/* xgpu_gl.c: the display as each eye sees it, in the eye's space (as
halo_vr_scope_window's): the axis's point, its up, the way to the display's
middle and its right, and the eye's field of view (tangents) */
struct halo_vr_cockpit_window
{
	float centre[2][3];
	float up[2][3];
	float forward[2][3];
	float right[2][3];
	float fov[2][4];
	float near_z;
	float far_z;
};

/* the reload by hand (vr.manual_reload, port/linux/game/vr_reload.c): the
right stick pushed down ejects the magazine (or the rocket launcher's
tubes), the left hand takes another from its pouch on the body and pushes
it into the weapon, which presses the game's reload; the shotgun takes its
shells one at a time, each a reload of one */
enum
{
	HALO_VR_RELOAD_NONE = 0, /* no reload (a plasma weapon), or none by hand */
	HALO_VR_RELOAD_MAGAZINE, /* a magazine, from the left hip */
	HALO_VR_RELOAD_SHELL,    /* a shell at a time, from the left hip */
	HALO_VR_RELOAD_TUBE,     /* the rocket launcher's tubes, from behind the left shoulder */
};

/* the weapon in hand as the game has it, told once a frame */
struct halo_vr_reload_weapon
{
	/* HALO_VR_RELOAD_*, and the weapon (its object's index: another one
	ends what the hand was doing with the last) */
	int kind;
	long weapon;
	/* the game would take a reload now (rounds to load, the magazine not
	full) and is reloading */
	int can_reload;
	int reloading;
	/* where the magazine goes in (or the shell, the tubes): from the right
	controller's grip along the weapon's axes (metres forward, left, up);
	well_known: told by the first-person weapon as it was drawn */
	int well_known;
	float well[3];
};

struct halo_vr_reload
{
	/* the reload by hand is on (vr.manual_reload, the controller aiming, the
	body's heading known), and the weapon's kind (HALO_VR_RELOAD_*) */
	int enabled;
	int kind;
	/* the weapon's magazine is out (ejected), waiting for another; the left
	hand holds one (a magazine, a shell, the tubes); and the magazines
	pushed home so far (each is a press of the game's reload) */
	int ejected;
	int held;
	unsigned long inserted;
	/* the pouch it is taken from (game axes from the recentred head,
	metres), whether the hand takes one there now (wanted: the weapon has
	room for one and rounds to fill it; the magazine in it, if not out
	already, ejects itself as one is taken), how far the left hand is from
	its reach (metres; 0 within it, large when the hand is not tracked), and
	how far the one held is from the well (large when none is held) */
	float body_yaw;
	float pouch[3];
	int wanted;
	float pouch_distance;
	float well_distance;
	/* the one held is drawn this far from the left controller's grip along
	the hand's axes (metres forward, left, up) */
	float held_offset[3];
	/* the first-person hands are the controllers' (vr.hands = "arms",
	"floating"): the first-person weapon plays no reload after the hand's,
	it stays in the hand */
	int hands;
};

/* flipping an overturned vehicle (vr.vehicle_flip, port/linux/game/
vr_flip.c): what flips one the game offers to flip, X (the game's action) or
a lift of both hands from low beside it, which presses the game's action */
#define HALO_VR_FLIP_BUTTON 1
#define HALO_VR_FLIP_LIFT 2

struct halo_vr_flip
{
	/* told: the game offers the first player a flip (its action would flip
	a vehicle), and where the vehicle's middle is (game axes from the
	recentred head, metres; vehicle_known FALSE: not known) */
	int offered;
	int vehicle_known;
	float vehicle[3];
	/* given: what flips it now (HALO_VR_FLIP_*), and the lift pressing
	the game's action */
	int flips;
	int pressed;
};

/* grabbing the world with the hands (vr.grab_world, port/linux/game/
vr_grab.c): a weapon on the ground the game offers to pick up, the right
hand reaching to it and gripping takes it (the game's pick-up, as X held);
a control the game would let the player use (a door's panel, a switch),
either hand's fingers pushed into it press it (the game's action, as X) */
#define HALO_VR_GRAB_THINGS 8
enum
{
	HALO_VR_GRAB_WEAPON = 1,
	HALO_VR_GRAB_CONTROL,
};

struct halo_vr_grab_thing
{
	/* HALO_VR_GRAB_*, and which (the game's object index, told back as it
	is taken) */
	int kind;
	long object;
	/* the line along it (its ends: game axes from the recentred head,
	metres), and how far about that line it is (metres); a control's face,
	both ends its middle, and the way it faces (0 0 0: either way) */
	float ends[2][3];
	float radius;
	float front[3];
};

struct halo_vr_grab
{
	/* told: the things to grab, this frame */
	int count;
	struct halo_vr_grab_thing things[HALO_VR_GRAB_THINGS];
	/* given: the grab is on (vr.grab_world, the controller aiming, on
	foot); the weapon the right hand is nearest (its object; -1 none) and
	how far it is from its reach (metres; 0 within it); and the weapon its
	grip took since the last call (-1 none), and the control a hand pressed
	(-1 none); the world's units a metre */
	int enabled;
	long nearest;
	float distance;
	long taken;
	long pressed;
	float world_units_per_metre;
};

/* ---------- vr.c */

/* the VR build with vr.enabled */
int halo_vr_enabled(void);
/* the session is open: the game plays in the headset (not flat) */
int halo_vr_running(void);
/* a VR frame is begun and is to be drawn */
int halo_vr_frame_active(void);
/* whether a frame of the runtime's is being drawn (waited for and not yet
presented), without waiting for one: the renderer's command stream tells
the host (xgpu_stream_encode.c) */
int halo_vr_frame_waited(void);
/* begins the VR frame before the game's clock is read (main.c): 1 and the
seconds from the last frame's display to this one's when it did */
int halo_vr_frame_clock(float *seconds);
/* the views, located again as late as possible (just before the frame's
draws); 0 without a VR frame */
int halo_vr_view(struct halo_vr_view *view);
int halo_vr_refresh_tracking(struct halo_vr_view *view);
/* the head as located when the frame began, as halo_vr_view has it
(head_forward, head_up, head_position): the sounds' listener (sound_manager.c,
vr_render.c), placed before the frame is drawn; 0 without a VR frame */
int halo_vr_head(float forward[3], float up[3], float position[3]);
/* the player's aim on foot follows the head or the right controller (1,
and yaw, pitch: the game's facing); 2 when the VR mode aims but the facing
stays the game's (vr.aim = "gamepad"); 0 when it does not (no VR frame, a
cinematic). With a long gun, the left hand on its foregrip aims it along
both hands (vr.two_handed). */
int halo_vr_aim(float current_yaw, int long_gun, float *yaw, float *pitch);
/* the aim no longer follows the head (a cinematic, a vehicle's camera
taking over): the next halo_vr_aim starts again from the player's facing */
void halo_vr_aim_release(void);
/* with the right controller aiming (vr.aim = "controller"), the line
halo_vr_aim aimed along last: from the controller (game axes from the
recentred head, metres) along its aim (halo_vr_view's aim_forward, as it is
now), and the turn of the stick (radians) the head's axes are turned by in
the world; 0 without it. The game aims from its own eye at where this line
meets the world (port/linux/game/vr_aim.c), so that the shot goes where the
reticle is, not along a line beside it. */
int halo_vr_aim_line(float position[3], float forward[3], float *body_yaw);
/* a vehicle's seat ridden first-person (port/linux/game/vr_vehicle.c),
placed each frame: its controls (HALO_VR_SEAT_*: none, a passenger's or a
gunner's hands as on foot; a Ghost's handlebars, a Warthog's wheel, which
steer; a turret's handles, which the hands hold as it aims where the right
controller points), the handles' places (left, right: the game's axes from
the recentred head, metres; handles_valid FALSE: none) and how a hand
holding one is turned (its forward and up, the player's axes), a wheel's
axis (toward the driver), the heading the view turns with the vehicle
(radians, the world's: the turn of the player's axes, which the stick turns
on foot), the vehicle's as the game has it this tick (the view's is eased
after it), which the controls steer from, -1 while it reverses (a wheel
turns it the other way), else 1, and its wheels' lock (radians, left and
right). Whether vr.c rides and steers such a seat (vr.ghost_steering,
vr.warthog_steering, vr.aim) */
enum
{
	HALO_VR_SEAT_RIDE,
	HALO_VR_SEAT_BARS,
	HALO_VR_SEAT_WHEEL,
	HALO_VR_SEAT_TURRET,
};
struct halo_vr_seat
{
	int kind;
	int handles_valid;
	float handles[2][3];
	float handle_forward[3];
	float handle_up[3];
	float wheel_axis[3];
	float heading;
	float steer_heading;
	float steer_sign;
	float lock[2];
};
int halo_vr_seat_enabled(int kind);
void halo_vr_seat_place(const struct halo_vr_seat *seat);
/* the first player drives a vehicle the game steers toward the facing, not
placed as a seat (the Scorpion: port/linux/game/vr_vehicle.c), told each
frame: the move stick's sideways push turns the player there, as the turn
stick does, and does not reach the game's sideways throttle */
void halo_vr_seat_drive(void);
/* the smart-link's view on the visor magnifies the aim this many times
this frame (1: it is not shown), told by the game (vr_aim.c): the aim is
steadied the more, the more it is magnified */
void halo_vr_aim_zoom(float magnification);
/* the aim assist (vr.aim_assist), told by the game each frame (vr_aim.c):
a target near the aim that the game's magnetism would be drawn to (as a
gamepad's aim is), which it is (another starts anew), how near the aim (its
magnetism level, 0 to 1), and the directions from the right controller (game
axes from the recentred head) of its middle (how it moves) and of the ends
of the part of its body in sight from the controller (low, high: from its
pelvis toward its head); NULL, none. The aim is slow to leave that part, and
zoomed, is drawn toward its nearest point (vr.c, aim_assist_step) */
void halo_vr_aim_assist(long target, float level, const float middle[3], const float low[3], const float high[3]);
/* debug.vr_test_hands holds the controllers (vr_hands.c marks them) */
int halo_vr_test_hands(void);
/* a left-hander's controllers (vr.dominant_hand = "left", the controllers
aiming or the head): the left one holds the weapon. All this file says of
the right hand (the right controller's poses, its trigger, its grip) is the
left's then, and of the left hand the right's; what is on the body is on
its other side (the holsters at the left shoulder and hip, the pouches at
the right); and the first-person weapon and hands are drawn mirrored
(vr_render.c), as the game's are right-handed */
int halo_vr_left_handed(void);
/* debug.vr_test_target: the right controller held by debug.vr_test_hands
points along this (game axes from the recentred head: at the target the
game placed, vr_aim.c), from the next frame; NULL: as debug.vr_test_hands
has it */
void halo_vr_test_point(const float direction[3]);
/* the throw with the left hand: its phase (HALO_VR_THROW_*), and what it
is; whether the game would throw a grenade now (the first player on foot,
with one, not throwing), each frame, which the hand needs to take one; and
that the game's throw is over (the grenade thrown, or not to be), which
ends the gesture's */
int halo_vr_throw(struct halo_vr_throw *throw_state);
void halo_vr_throw_ready(int ready);
void halo_vr_throw_done(void);

/* the holster of the weapon the player carries but does not hold, once a
frame (HALO_VR_HOLSTER_*: NONE when there is none, or it cannot be drawn
now); and the holsters as they are */
void halo_vr_holster_stowed(int holster);
int halo_vr_holsters(struct halo_vr_holsters *holsters);

/* the physical crouch (vr.physical_crouch): whether the game could crouch
the player now (the first player on foot, alive, the VR mode aiming), each
frame; and the crouch as it is: HALO_VR_CROUCH_PHYSICAL while the head is
low enough to crouch (vr.c presses the game's crouch for it while the game
could crouch the player), HALO_VR_CROUCH_BUTTON while a hand presses it (the
left stick's click, a gamepad's) */
#define HALO_VR_CROUCH_PHYSICAL 1
#define HALO_VR_CROUCH_BUTTON 2
void halo_vr_crouch_able(int able);
int halo_vr_crouch(void);
/* the first player's melee landed on something (units.c, as its sound
plays): a swing's or B's, a knock in the right controller */
void halo_vr_melee_hit(void);
/* the weapons' feel (port/linux/game/vr_feel.c): whether it is the VR
mode's (the controllers aim: vr.aim = "controller"), and how far the
first-person weapon kicks as it fires (vr.recoil, 0 to 1; 0 without it) */
int halo_vr_weapon_feel(void);
float halo_vr_recoil(void);
/* ... each shot of the first player's weapon in hand, felt in the hands as
its kind (HALO_VR_FEEL_*), in place of the game's rumble for it
(vr.weapon_haptics); and the plasma pistol charging, once a frame (0 to 1;
0: not) */
enum
{
	HALO_VR_FEEL_TAP = 0, /* any other weapon */
	HALO_VR_FEEL_PISTOL,
	HALO_VR_FEEL_RIFLE,   /* the assault rifle */
	HALO_VR_FEEL_SHOTGUN,
	HALO_VR_FEEL_SNIPER,
	HALO_VR_FEEL_ROCKET,  /* the rocket launcher, the fuel rod gun */
	HALO_VR_FEEL_PLASMA,  /* the plasma pistol, the plasma rifle */
	HALO_VR_FEEL_CHARGED, /* the plasma pistol's charged shot */
	HALO_VR_FEEL_NEEDLER,
	HALO_VR_FEEL_FLAME,   /* the flamethrower */
	HALO_VR_FEELS,
};
void halo_vr_weapon_fire(int feel);
void halo_vr_weapon_charge(float charge);
/* ... a hit on the player, from that direction (game axes from the
recentred head), this hard (0 to 1), felt in the hand on its side; and the
player's shields breaking */
void halo_vr_hit(const float from[3], float strength);
void halo_vr_shields_broken(void);
/* the scopes are on (vr.scopes, with vr.aim = "controller"), and how they
show their view (vr.scope_view): HALO_VR_SCOPES_EYE raised to an eye, in
their lens; HALO_VR_SCOPES_SMARTLINK on the visor, as the game zooms; 0 off.
And the smart-link's view: how wide (degrees) and how far before the eyes
(metres: the HUD's distance) */
#define HALO_VR_SCOPES_EYE 1
#define HALO_VR_SCOPES_SMARTLINK 2
int halo_vr_scopes(void);
void halo_vr_smartlink_view(float *degrees, float *distance);
/* the HUD's width (vr.hud_size, degrees: its height three quarters of it),
which the smart-link's framed window is as large as; whether the window is
on the visor where the HUD is (vr.smartlink_place = "visor"), else at the
reticle */
int halo_vr_smartlink_hud(float *hud_degrees);
/* this frame's scope, drawn into its image (the scope pass): shown where
it says at the present; NULL, none */
void halo_vr_scope(const struct halo_vr_scope *scope);
/* xgpu_gl.c, as it makes the scope's image: the zoom shown in its lens
(halo_vr_scope's; 0: none), and how tall (of the lens's width) */
float halo_vr_scope_zoom(float *height);
/* and whether it is the smart-link's, its reticle's place (halo_vr_scope's) */
int halo_vr_scope_smartlink(float reticle[2]);
/* whether this frame's scope is the smart-link's window, and if so where
the eyes see it (halo_vr_scope_window); its range and rounds; and how the
weapon in hand shows over it (vr.scope_gun: 0 hidden, 1 ghost, 2 visible) */
int halo_vr_scope_window(struct halo_vr_scope_window *window);
/* a feed cockpit's display (NULL: none this frame); for xgpu_gl.c, as each
eye sees it, and what it shows */
void halo_vr_cockpit_display(const struct halo_vr_cockpit_display *display);
/* vr_climb.c, once a frame: the holds near told, the hands' holds and the
eye's move given; how many hands hold */
int halo_vr_climb(struct halo_vr_climb *climb);
int halo_vr_cockpit_window(struct halo_vr_cockpit_window *window, struct halo_vr_cockpit_display *display);
void halo_vr_scope_readout(float *range, int *rounds_loaded, int *rounds_carried);
void halo_vr_scope_animation(float *opened_seconds, float *closing, float *switched_seconds);
/* and its window's shape and the world's dimming about it (halo_vr_scope's
corner, dim and feather) */
void halo_vr_scope_shape(float *corner, float *dim, float *feather);
int halo_vr_scope_gun(void);
/* the weapon in hand, for the reload by hand, once a frame; and the reload
by hand as it is (0 when it is off) */
void halo_vr_reload_weapon(const struct halo_vr_reload_weapon *weapon);
int halo_vr_reload(struct halo_vr_reload *reload);

/* the flip of an overturned vehicle, once a frame: the offer told, what
flips it given (0 when the VR mode is not running); and what flips one now
(HALO_VR_FLIP_*: the button alone when the VR mode is not running) */
int halo_vr_flip(struct halo_vr_flip *flip);
int halo_vr_flips(void);

/* the grab, once a frame (port/linux/game/vr_grab.c): the things to grab
told, the right hand's reach and grip given, and a press of a control (0
when the VR mode is not running) */
int halo_vr_grab(struct halo_vr_grab *grab);
/* debug.vr_test_grab: the player turned (as the stick turns) so that the
head faces this way (radians, the world's yaw), to face what the test
stood them before */
void halo_vr_test_face(float yaw);

/* the player, for the visor's effects (vr_visor.c) */
void halo_vr_visor_state(const struct halo_vr_visor_state *state);
/* the HUD's draws from now on are of this group (HALO_VR_HUD_GROUP_*), or
(HALO_VR_HUD_BY_ANCHOR) of the group of the anchor of the element drawn
(halo_vr_hud_anchor: hud_draw.c tells the hud_anchor of each element it
places, 0 to 4). xgpu_gl.c draws each group into an image of its own, for
the HUD's panels (vr_visor.c, vr.hud_depth); the frame starts with
HALO_VR_HUD_GROUP_CENTRE */
#define HALO_VR_HUD_BY_ANCHOR (-1)
void halo_vr_hud_group(int group);
void halo_vr_hud_anchor(int corner);

/* the platform layer's (d3d8_device.c, xinput_sdl.c) */
/* opens the session once the host's renderer has its device; 0 if VR cannot run */
int halo_vr_initialize(void);
void halo_vr_eye_size(int *width, int *height);
void halo_vr_hud_size(int *width, int *height);
/* a HUD panel's image (vr_host.h's VR_SWAPCHAIN_PANEL + panel); 0 by 0 if
there is none */
void halo_vr_panel_size(int panel, int *width, int *height);
/* the scope's image, square (vr_host.h's VR_SWAPCHAIN_SCOPE); 0 if there is
none */
int halo_vr_scope_size(void);
/* whether the swapchain (vr_host.h's VR_SWAPCHAIN_*) has an image for this
frame, which the host draws: 1, or 0 if there is none */
unsigned int halo_vr_image(int swapchain);
/* the frame is presented with what was drawn (it ends as the next begins,
which halo_vr_frame_active waits for, as vsync would) */
void halo_vr_present(int eyes_drawn, int hud_drawn, int scope_drawn);
/* the controllers as port 0's gamepad (an XINPUT_GAMEPAD), merged in */
void halo_vr_gamepad(void *gamepad);
void halo_vr_rumble(float left, float right);
/* the name of what presses one of the game's controller buttons (its index,
input.h's _gamepad_ buttons: A, B, X, Y, black, white, the triggers, the
d-pad, Start, Back, the sticks' clicks) on the controllers in the hands, as
the runtime names it ("A Button", "Right Hand Trigger") or a fixed name, or
the gesture that does ("Left Hand Throw"), Latin-1, for the HUD's prompts
(port/linux/game/hud_key_prompts.c); 0 outside VR */
int halo_vr_button_name(int button, char *name, unsigned long size);
/* the menus are up: their layer stays in front of the recentred place,
not of the head */
void halo_vr_menus(int active);
/* the aim follows the head: the controller's magnetism leaves it alone */
int halo_vr_aiming(void);
int halo_vr_aimed(void);
/* the aim is the gamepad's (vr.aim = "gamepad"), magnetism and all */
int halo_vr_gamepad_aiming(void);
/* whether a gamepad's Back (View) is held, each poll: held a second, it
recentres */
void halo_vr_gamepad_back(int held);

/* the poses this frame's eyes are drawn from, for the host's late latch
(vr_host.h's struct vr_host_latch; with vr.late_latch off, none to move):
0 if the frame has no views located */
struct vr_host_latch;
int halo_vr_latch(struct vr_host_latch *latch);
/* the late latch is on, and its head the runtime's (vr_render.c culls
wider for it) */
int halo_vr_late_latching(void);

/* the eyes' depth: the projection's range (vr_render.c, in metres) and
that this frame's is in its swapchain (xgpu_gl.c) */
void halo_vr_depth_range(float near_metres, float far_metres);
int halo_vr_depth_wanted(void);
void halo_vr_depth_written(void);

/* ---------- d3d8_device.c, xgpu_gl.c */

void halo_vr_pass(int pass);
/* row-major 4x4s: an eye's clip position from the centre view's, for the
projection the game has just set (rasterizer_set_frustum_z) */
void halo_vr_set_eye_transforms(const float transforms[2][16]);
/* each eye's scale and offset (x, y, then x, y) of the draws in the screen,
from the centre view's (identity: 1, 1, 0, 0), for what the game projects
to the screen itself (vr_render.c, vr_render_screen_point) */
void halo_vr_set_screen_corrections(const float corrections[2][4]);
/* for the late latch (vr.late_latch; xgpu_stream.h), before
halo_vr_set_eye_transforms: what the projection's draws move with
(XGPU_STREAM_VR_LATCH_*) and each eye's clip transform from its own space
(row-major: OpenXR's axes, metres); and before
halo_vr_set_screen_corrections, the point they are for (its clip position
in the centre view, w 0 none, in a viewport this wide and high) */
void halo_vr_eye_latch(int latch, const float clip_from_eye[2][16]);
void halo_vr_screen_point(const float clip[4], float width, float height);
/* the draws from now on go into that eye's layer alone (0 left, 1 right),
or both at once (-1, multiview) */
void halo_vr_draw_eye(int eye);
/* the sky is being drawn (vr_render.c): with depth clamping, not clipped */
void halo_vr_sky(int sky);
/* the model drawn from now on is mirrored (its nodes' axes left-handed: a
left-hander's first-person weapon and hands), its triangles' winding the
other way; told by the rasterizer as each model's nodes are set, and as it
ends (rasterizer_xbox.c) */
void halo_vr_mirrored_draws(int mirrored);
int halo_vr_mirrored(void);
/* the effect drawn from now on (an effect shader's: particles, contrails,
weather) is drawn soft against the scene's depth (1), and thick (2), in the
eyes' pass; 0 for the draws after it. Told by the rasterizer for each
effect's group, and as it ends (rasterizer_xbox_transparent_geometry.c);
render_sprite.c chooses thick for sprites large enough on the screen. */
void halo_vr_effect_depth(int mode);
/* the comfort vignette (vr.comfort_vignette): asked for each frame by
the game (vr_vehicle.c: how fast the view turns with a vehicle, how hard
the vehicle speeds up or slows), 0 to 1; and as it is drawn this frame,
eased (0: none), with the eyes' fields of view (tangents: left, right, up,
down) */
void halo_vr_comfort_vignette(float amount);
float halo_vr_vignette(float fov[2][4]);
/* this frame's eyes' images multiplied by this colour as they are
presented, all of each, before the helmet's rim; each call multiplies it
further (vr_seat.c's blinks, vr_death.c's dimming: the game's screen flash
covers its viewport alone, which the eyes' images reach past) */
void halo_vr_eye_tint(float red, float green, float blue);
/* the foveation (vr.foveation): the inset's part of each eye's image across
and up, which is the periphery's resolution too (0: off), as the session
was made, and what places the inset (for the log); and this frame's inset
middles, in each eye's image (0 to 1 from its top left), with 1 (fixed:
the middle of each eye's view) or 2 (the eyes' gaze, smoothed), or 0 (off,
or no frame) */
float halo_vr_foveation_inset(void);
const char *halo_vr_foveation_name(void);
int halo_vr_foveation(float centres[2][2]);

#endif
