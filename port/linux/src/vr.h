/*
VR.H

The VR mode (HALO_VR; port/linux/README.md, "VR"): what the game's code
(port/linux/game/vr_render.c, render.c, player_control.c), the renderer
(d3d8_gl.c) and the input (xinput_sdl.c) ask of vr.c. Plain C types only:
the game's code includes it as well as the platform layer.

The game draws one view from the centre of the head, its field of view the
union of both eyes', and the renderer draws each of its draws into both eyes
at once (GL_OVR_multiview2), each vertex's clip position moved from the
centre view's to its eye's by a fixed transform per eye (halo_vr_set_eye_
transforms). The HUD and the menus go to a layer of their own.

Game axes: forward, left, up (the game's x, y, z at a yaw of 0).
*/

#ifndef __HALO_VR_H
#define __HALO_VR_H

/* the renderer's passes (d3d8_gl.c, halo_vr_pass) */
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
smart-linked, on a quad on the visor just above the aim, facing the head,
seen by both */
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

/* ---------- vr.c */

/* the VR build with vr.enabled */
int halo_vr_enabled(void);
/* the session is open: the game plays in the headset (not flat) */
int halo_vr_running(void);
/* a VR frame is begun and is to be drawn */
int halo_vr_frame_active(void);
/* begins the VR frame before the game's clock is read (main.c): 1 and the
seconds from the last frame's display to this one's when it did */
int halo_vr_frame_clock(float *seconds);
/* the views, located again as late as possible (just before the frame's
draws); 0 without a VR frame */
int halo_vr_view(struct halo_vr_view *view);
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
/* the scopes are on (vr.scopes, with vr.aim = "controller"), and how they
show their view (vr.scope_view): HALO_VR_SCOPES_EYE raised to an eye, in
their lens; HALO_VR_SCOPES_SMARTLINK on the visor, as the game zooms; 0 off.
And the smart-link's view: how wide (degrees) and how far before the eyes
(metres: the HUD's distance) */
#define HALO_VR_SCOPES_EYE 1
#define HALO_VR_SCOPES_SMARTLINK 2
int halo_vr_scopes(void);
void halo_vr_smartlink_view(float *degrees, float *distance);
/* this frame's scope, drawn into its image (the scope pass): shown where
it says at the present; NULL, none */
void halo_vr_scope(const struct halo_vr_scope *scope);
/* d3d8_gl.c, as it makes the scope's image: the zoom shown in its lens
(halo_vr_scope's; 0: none), and how tall (of the lens's width) */
float halo_vr_scope_zoom(float *height);
/* and whether it is the smart-link's, its reticle's place (halo_vr_scope's) */
int halo_vr_scope_smartlink(float reticle[2]);
/* the weapon in hand, for the reload by hand, once a frame; and the reload
by hand as it is (0 when it is off) */
void halo_vr_reload_weapon(const struct halo_vr_reload_weapon *weapon);
int halo_vr_reload(struct halo_vr_reload *reload);

/* the player, for the visor's effects (vr_visor.c) */
void halo_vr_visor_state(const struct halo_vr_visor_state *state);
/* the HUD's draws from now on are of this group (HALO_VR_HUD_GROUP_*), or
(HALO_VR_HUD_BY_ANCHOR) of the group of the anchor of the element drawn
(halo_vr_hud_anchor: hud_draw.c tells the hud_anchor of each element it
places, 0 to 4). d3d8_gl.c draws each group into an image of its own, for
the HUD's panels (vr_visor.c, vr.hud_depth); the frame starts with
HALO_VR_HUD_GROUP_CENTRE */
#define HALO_VR_HUD_BY_ANCHOR (-1)
void halo_vr_hud_group(int group);
void halo_vr_hud_anchor(int corner);

/* the platform layer's (d3d8_gl.c, xinput_sdl.c) */
/* opens the session once the GL context is current; 0 if VR cannot run */
int halo_vr_initialize(void);
void halo_vr_eye_size(int *width, int *height);
void halo_vr_hud_size(int *width, int *height);
/* a HUD panel's image (vr_host.h's VR_SWAPCHAIN_PANEL + panel); 0 by 0 if
there is none */
void halo_vr_panel_size(int panel, int *width, int *height);
/* the scope's image, square (vr_host.h's VR_SWAPCHAIN_SCOPE); 0 if there is
none */
int halo_vr_scope_size(void);
/* the swapchain's image for this frame (vr_host.h's VR_SWAPCHAIN_*), a GL
texture: a 2-layer array for the eyes; 0 if there is none */
unsigned int halo_vr_image(int swapchain);
/* the frame is presented with what was drawn (it ends as the next begins,
which halo_vr_frame_active waits for, as vsync would) */
void halo_vr_present(int eyes_drawn, int hud_drawn, int scope_drawn);
/* the controllers as port 0's gamepad (an XINPUT_GAMEPAD), merged in */
void halo_vr_gamepad(void *gamepad);
void halo_vr_rumble(float left, float right);
/* the menus are up: their layer stays in front of the recentred place,
not of the head */
void halo_vr_menus(int active);
/* the aim follows the head: the controller's magnetism leaves it alone */
int halo_vr_aiming(void);
/* the aim is the gamepad's (vr.aim = "gamepad"), magnetism and all */
int halo_vr_gamepad_aiming(void);
/* whether a gamepad's Back (View) is held, each poll: held a second, it
recentres */
void halo_vr_gamepad_back(int held);

/* the eyes' depth: the projection's range (vr_render.c, in metres) and
that this frame's is in its swapchain (d3d8_gl.c) */
void halo_vr_depth_range(float near_metres, float far_metres);
int halo_vr_depth_wanted(void);
void halo_vr_depth_written(void);

/* ---------- d3d8_gl.c */

void halo_vr_pass(int pass);
/* row-major 4x4s: an eye's clip position from the centre view's, for the
projection the game has just set (rasterizer_set_frustum_z) */
void halo_vr_set_eye_transforms(const float transforms[2][16]);
/* each eye's scale and offset (x, y, then x, y) of the draws in the screen,
from the centre view's (identity: 1, 1, 0, 0), for what the game projects
to the screen itself (vr_render.c, vr_render_screen_point) */
void halo_vr_set_screen_corrections(const float corrections[2][4]);
/* the draws from now on go into that eye's layer alone (0 left, 1 right),
or both at once (-1, multiview) */
void halo_vr_draw_eye(int eye);
/* the sky is being drawn (vr_render.c): with depth clamping, not clipped */
void halo_vr_sky(int sky);
/* this frame's eyes' images multiplied by this colour as they are
presented, all of each, before the helmet's rim; each call multiplies it
further (vr_seat.c's blinks, vr_death.c's dimming: the game's screen flash
covers its viewport alone, which the eyes' images reach past) */
void halo_vr_eye_tint(float red, float green, float blue);

#endif
