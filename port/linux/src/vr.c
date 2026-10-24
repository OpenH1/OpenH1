/*
VR.C

The VR mode's platform side (HALO_VR; vr.h, port/linux/README.md "VR"): the
OpenXR session through the host (vr_host.h, port/linux/host/host_vr.c),
the frame loop, the head's pose in the game's axes, the controllers as a
gamepad, turning and recentring, the left hand's grenade throw, the
holsters and the reload by hand.

Frames: the runtime paces the game. The present of one frame ends it and
waits for the next (halo_vr_present), whose head pose then aims the player
(halo_vr_aim) while the game updates; just before the frame's draws the
views are located again for the display time (halo_vr_view), and those are
the poses the frame is drawn and submitted with.

The head moves the view about the player's eye, which the game keeps (it
crouches and rides as before): from where the head was at the last
recentring (seated), or from vr.player_height above the floor (standing).
The controllers' right stick turns the player (snap or smooth), and the
aim on foot is where the head looks, so the reticle stays in the middle of
the view and the netcode sees an ordinary player's aim.

With vr.aim = "gamepad" a gamepad plays as on the flat screen: its stick
turns and aims the player (the game's own look code), and the head looks
about from the aim: the view is the facing, yaw and pitch, turned and moved
by the head (vr.gamepad_view = "camera"), or the facing's yaw alone, with
it or in steps ("level", "snap"). The controllers are a gamepad's halves.

The player crouches as the head does (vr.physical_crouch): the head low
enough below the recentred eyes presses the game's crouch (the left stick's
click), which Halo holds only while it is pressed; standing up lets it go.
The game lowers its eye as it crouches, which vr_render.c takes back out of
the view while the crouch is the head's: the head has already come down.
*/

#ifdef HALO_VR
#include "platform.h"
#include "port_config.h"
#include "vr.h"
#include "vr_host.h"
#include "vr_visor.h"

extern int halo_load_trace_enabled(void);
extern void halo_load_trace(const char *phase, const char *detail);

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* the HUD's and menus' image: the 640x480 screen at twice its size */
#define HUD_WIDTH 1280
#define HUD_HEIGHT 960
/* the scope's image (vr.scopes), square: about as many pixels as the eye's
image has over the lens (a lens of 28 degrees, an eye of 1728 lines over
about 100), or two thirds as many as it has over the smart-link's window
(70 degrees: the window is drawn into the eyes, filtered), times
vr.resolution_scale, within these */
#define SCOPE_IMAGE_SIZE 480
#define SCOPE_WINDOW_IMAGE_SIZE 1024
#define SCOPE_IMAGE_SMALLEST 256
#define SCOPE_IMAGE_LARGEST 1536
/* the smart-link's window about the reticle (vr.smartlink_size): no
narrower nor wider than these (degrees) */
#define SMARTLINK_NARROWEST 30.0f
#define SMARTLINK_WIDEST 100.0f
/* a world unit is 10 feet */
#define METRES_PER_WORLD_UNIT 3.048f
/* a melee by the right hand's swing (vr.melee_gesture), in any direction:
its speed past MELEE_SPEED, or past MELEE_GRIP_SPEED with the right grip
held past MELEE_GRIP_PRESSED, once it has gone MELEE_TRAVEL since it was
faster than MELEE_REARM_SPEED, and for MELEE_SUSTAIN_SECONDS and
MELEE_SUSTAIN_FRAMES frames (a late frame is no longer a swing), still
moving faster than that from the frame before (a twitch, or the tracking
jumping in a frame, is no swing), presses B for MELEE_PRESS_MS; once a
swing: again only after the hand has slowed below MELEE_REARM_SPEED, and no
sooner than MELEE_INTERVAL_MS after the last. Its speed is the hand's own,
measured over MELEE_VELOCITY_SECONDS. Not a swing: the hand carried by the
head (its speed from the head, as the head is turned and moved, less than
MELEE_CARRIED of its own: a turn of the body, a step, a crouch; the head
turning alone does not move the hand); going back toward the body, the
grip not held as it began (its speed backward, along where the head faces,
more than MELEE_BACKWARD of it: a hand drawn back, reaching for a holster,
which goes back to the hip or over the shoulder, to grip there); faster than MELEE_FASTEST (the tracking lost and
found); nor while the right grip holds a holster (taken there, as a draw
is: holsters_update) or for HOLSTER_NO_MELEE_MS after a draw, nor while the
left hand throws a grenade (its swing and its release) or holds a magazine,
or for MELEE_GUARD_MS after (the throw's swing, the reload's push). A swing
passing by a holster, or through one with the grip held, is a swing.
(Speeds in metres a second.) */
#define MELEE_SPEED 2.6f
#define MELEE_GRIP_SPEED 1.8f
#define MELEE_GRIP_PRESSED 0.6f
#define MELEE_REARM_SPEED 1.0f
#define MELEE_TRAVEL 0.10f
#define MELEE_SUSTAIN_SECONDS 0.04f
#define MELEE_SUSTAIN_FRAMES 3
#define MELEE_VELOCITY_SECONDS 0.03f
#define MELEE_BACKWARD 0.6f
#define MELEE_FASTEST 15.0f
#define MELEE_CARRIED 0.5f
#define MELEE_GUARD_MS 400
#define MELEE_PRESS_MS 70
#define MELEE_INTERVAL_MS 600
/* the right hand's and the head's places kept for it (a power of two: more
than MELEE_VELOCITY_SECONDS's frames at 144 Hz) */
#define MELEE_SAMPLES 16
/* the right index finger on the trigger (halo_vr_view's right_trigger_touch):
on it while the trigger senses it, or is pulled past TRIGGER_TOUCH_PULL (a
finger pulling is on it, whatever the touch says), or always where the
trigger senses no touch; it comes on and off over TRIGGER_TOUCH_SECONDS */
#define TRIGGER_TOUCH_PULL 0.02f
#define TRIGGER_TOUCH_SECONDS 0.08f
/* a melee's hit (halo_vr_melee_hit): a knock in the right controller */
#define MELEE_HIT_AMPLITUDE 0.8f
#define MELEE_HIT_SECONDS 0.06f
/* holding both grips this long recentres, or a gamepad's Back (View) */
#define RECENTRE_SECONDS 0.75f
/* a vehicle's seat ridden first-person (port/linux/game/vr_vehicle.c, which
places it each frame and turns the view with the vehicle: the stick turns
the view nothing there, but steers a seat steered by its controls while the
hands do not both hold them: STICK_STEER_*), and its controls (HALO_VR_SEAT_*): a Ghost's handlebars
(vr.ghost_steering = "handlebars"), a Warthog's wheel (vr.warthog_steering =
"wheel"), a turret's handles, or none (a passenger's: the hands as on foot).
Each hand takes its handle as its grip closes past HANDLEBAR_GRIP_PRESSED
within HANDLEBAR_REACH of it (metres; the hand is then drawn on it), and
lets go below HANDLEBAR_GRIP_RELEASED; both hands on them are not a
recentring. With both held:
- the handlebars' turn (the line between the hands, turned about the
  vehicle's up from straight across, less HANDLEBAR_DEADZONE radians: hands
  are never quite still) asks for a turn of HANDLEBAR_TURN_RATE times as
  many radians a second, HANDLEBAR_TURN_FASTEST at most, as a bike's bars
  do; the facing is the vehicle's heading turned by as much as keeps the
  Ghost turning so (the game turns it toward the facing at the lesser of
  sqrt(2 a e) and e / 2 radians a tick for an error of e, a being
  HANDLEBAR_GHOST_TURN_ACCELERATION, update_alien_scout_physics: held off
  its heading as it turns, the error stays, and so does the turn), and
  pitched as the hands are, from how they pointed as they took hold (within
  HANDLEBAR_PITCH_LIMIT radians). Turned by the bars themselves, the Ghost
  turned as fast as it can, past 100 degrees a second for a 35 degree turn
  of the bars;
- the wheel's turn (the line between the hands, turned about the wheel's
  axis from where it was as both took hold, less HANDLEBAR_DEADZONE) turns
  the vehicle's wheels by a WHEEL_RATIO'th of it, to their lock (the
  vehicle's tag's, the Warthog's 30 degrees either way): the game turns
  them to the facing's error itself (vehicle_update, the other way as it
  reverses), so the facing is the heading turned so (WHEEL_RATIO is a car's
  steering made quick for arms that do not cross: the Warthog's wheels
  turn it some 80 degrees a second at speed for 7 of them);
- a turret's handles steer nothing: the right controller aims it, as it
  aims a weapon.
Let go, the turn and the pitch ease out over HANDLEBAR_RELEASE_SECONDS (the
time constant): the vehicle holds its course. A placing older than
HANDLEBAR_PLACED_FRAMES frames is no seat. */
#define HANDLEBAR_GRIP_PRESSED 0.6f
#define HANDLEBAR_GRIP_RELEASED 0.3f
#define HANDLEBAR_REACH 0.30f
#define HANDLEBAR_DEADZONE 0.035f
#define HANDLEBAR_TURN_RATE 2.0f
#define HANDLEBAR_TURN_FASTEST 1.5708f
#define HANDLEBAR_GHOST_TURN_ACCELERATION 0.0034906587f
#define HANDLEBAR_TICKS_PER_SECOND 30.0f
#define HANDLEBAR_PITCH_LIMIT 0.6f
#define HANDLEBAR_RELEASE_SECONDS 0.15f
#define HANDLEBAR_PLACED_FRAMES 2
#define WHEEL_RATIO 4.0f
/* with the hands off a Ghost's handlebars or a Warthog's wheel (not both
held), either stick pushed sideways steers, the stronger push of the two
(the left stick's then does not strafe), past STICK_STEER_DEADZONE: the
Ghost asks for a turn of up to HANDLEBAR_TURN_FASTEST radians a second at a
full push, as the bars turned 45 degrees do; the Warthog turns its wheels as
far as their lock at a full push (the other way as it reverses, as the
wheel does); eased in over STICK_STEER_SECONDS (the time constant), and out
over HANDLEBAR_RELEASE_SECONDS as the stick comes back: the vehicle then
holds its course. Driving a vehicle the game steers toward the aim (the
Scorpion: halo_vr_seat_drive), the left stick's sideways push turns the
player smoothly, at up to STICK_DRIVE_FASTEST radians a second: the aim
turns, and the vehicle after it, as the right stick turns them. */
#define STICK_STEER_DEADZONE 0.2f
#define STICK_STEER_SECONDS 0.08f
#define STICK_DRIVE_FASTEST 1.5708f
/* the comfort vignette (vr.comfort_vignette): eased toward what the game
asks for over VIGNETTE_IN_SECONDS as it grows and VIGNETTE_OUT_SECONDS as it
falls (time constants), to nothing when the game asks for none */
#define VIGNETTE_IN_SECONDS 0.12f
#define VIGNETTE_OUT_SECONDS 0.4f
#define GAMEPAD_RECENTRE_SECONDS 1.0f
/* both hands on a long gun (vr.two_handed): the left hand takes the
foregrip when its grip is pressed this near the line ahead of the right hand
(metres), and lets it go when the grip is let go; the aim turns to the line
between the hands, smoothed over this time constant (seconds) */
#define FOREGRIP_NEAREST 0.08f
#define FOREGRIP_FARTHEST 0.70f
#define FOREGRIP_RADIUS 0.18f
#define FOREGRIP_PRESSED 0.6f
#define FOREGRIP_RELEASED 0.3f
#define TWO_HANDED_SMOOTHING 0.05f
#define TWO_HANDED_BLEND 0.15f
/* a grenade thrown with the left hand (vr.grenade_throw = "gesture", the
hand alone, or "both", the hand or the left trigger; the names are the ones
debug.vr_throw_log uses):
- held: the left grip pulled past THROW_GRIP_PRESSED (not on a long gun's
  foregrip, not with the right grip too, which recentres), with a grenade
  to throw;
- the swing: once held THROW_HOLD_SECONDS, the hand moving forward (along
  where the head faces, level) faster than THROW_SWING_SPEED, from no
  farther ahead of the eyes than THROW_WINDUP_REACH since it was taken (a
  throw starts from a hand drawn back, not one held out): the game's throw
  begins, as with the left trigger;
- let go: the grip below THROW_GRIP_RELEASED releases it, along the
  hand's fastest velocity over the last THROW_RELEASE_LOOKBACK seconds (each
  measured over THROW_VELOCITY_SECONDS), lifted THROW_LIFT_DEGREES, as hard
  as the game throws at THROW_FULL_SPEED and up (as hard as THROW_LEAST_POWER
  of it at the least); a swing not let go of in THROW_SWING_SECONDS throws
  anyway, at its fastest (the game's throw has begun: its grenade is spent);
- a release the game has not thrown in THROW_RELEASED_SECONDS is dropped.
A grip let go of before any swing throws nothing. (Speeds in metres a
second.) */
#define THROW_GRIP_PRESSED 0.6f
#define THROW_GRIP_RELEASED 0.3f
#define THROW_HOLD_SECONDS 0.10f
#define THROW_SWING_SPEED 1.6f
#define THROW_WINDUP_REACH 0.15f
#define THROW_VELOCITY_SECONDS 0.035f
#define THROW_RELEASE_LOOKBACK 0.10f
#define THROW_LIFT_DEGREES 8.0f
#define THROW_FULL_SPEED 4.0f
#define THROW_LEAST_POWER 0.3f
#define THROW_SWING_SECONDS 0.75f
#define THROW_RELEASED_SECONDS 0.5f
/* the left hand's places and velocities kept for it (a power of two: more
than THROW_RELEASE_LOOKBACK's frames at 144 Hz) */
#define THROW_SAMPLES 32
/* the left trigger held for the game's throw at least this many frames (a
tick is 33 ms; a frame at 144 Hz 7) */
#define THROW_PRESS_FRAMES 6
/* the body's heading (body_update): the head's, which it follows only
past a turn of BODY_TURN_FREE (radians), and the rest of the way over
BODY_TURN_SECONDS; from the head alone, so that a hand reaching back for a
holster does not turn the holster away from it */
#define BODY_TURN_FREE 0.70f
#define BODY_TURN_SECONDS 1.5f
/* where the body hangs from the head (body_place): the top of the neck,
which the head nods and turns about, NECK_BACK behind the eyes and NECK_DOWN
below them along the head's own axes (metres) */
#define NECK_BACK 0.10f
#define NECK_DOWN 0.10f
/* the holsters (vr.holsters): each one's middle from the head, along the
body's heading (metres forward, left, up), and how near it the right hand
reaches it (metres): over the right shoulder, behind it, and at the right
hip. The right grip pulled past HOLSTER_GRIP_PRESSED in
the stowed weapon's holster switches to it (Y, for HOLSTER_PRESS_FRAMES
frames), the hand there all but still (slower than HOLSTER_STILL_SPEED,
metres a second) and not in a swing that melee'd: a grip squeezed by a
swing passing through is no draw. The hand's swing does not melee while
that grip is held, nor for HOLSTER_NO_MELEE_MS after a draw, nor the swing
that brings the drawn weapon out of its holster, however long the hand
waits there (drawing a gun is a fast move forward). A left-hander's are
mirrored, over the left shoulder and at the left hip (hand_left). */
static const float holster_places[HALO_VR_HOLSTERS][4] =
{
	{ 0.0f, 0.0f, 0.0f, 0.0f },
	{ -0.12f, -0.14f, -0.08f, 0.20f },
	{ 0.02f, -0.24f, -0.68f, 0.20f },
};
#define HOLSTER_GRIP_PRESSED 0.6f
#define HOLSTER_GRIP_RELEASED 0.3f
#define HOLSTER_PRESS_FRAMES 6
#define HOLSTER_NO_MELEE_MS 500
#define HOLSTER_STILL_SPEED 2.0f
/* the shoulder's holster is out of sight: the right hand nearer it than
HOLSTER_NEAR_DISTANCE (metres from its reach) with the stowed weapon there
ticks its controller, a tick each HOLSTER_NEAR_SLOWEST seconds at that
distance down to each HOLSTER_NEAR_FASTEST at the reach, and stronger (as
strong as holster_near_amplitude says, there and at the reach), until the
tick of the reach. Not the hip's, which the hand passes and rests by, and
which the eyes find. Nor while the hand aims (holster_near_aiming): a long
gun held close, the hand up by the shoulder, is not a reach for it. */
#define HOLSTER_NEAR_DISTANCE 0.30f
#define HOLSTER_NEAR_SLOWEST 0.25f
#define HOLSTER_NEAR_FASTEST 0.06f
static const float holster_near_amplitude[2] = { 0.08f, 0.18f };
#define HOLSTER_NEAR_TICK_SECONDS 0.012f
/* the physical crouch (vr.physical_crouch): the head this far below the
recentred eyes (metres; standing, from vr.player_height; seated, from where
they were) crouches, and back up within CROUCH_HYSTERESIS of it stands (a
head bobbing about the line does not crouch and stand each frame) */
#define CROUCH_STANDING_DROP 0.35f
#define CROUCH_SEATED_DROP 0.25f
#define CROUCH_HYSTERESIS 0.07f
/* the reload by hand (vr.manual_reload): the right stick pushed down past
RELOAD_STICK_PRESSED ejects the magazine (once, until it comes back within
RELOAD_STICK_RELEASED). The pouches, each one's middle from the head along
the body's heading (metres forward, left, up) and how near it the left hand
reaches it (metres): the magazines' and the shells' at the front of the left
hip, the rocket launcher's tubes behind the left shoulder (the holsters'
mirrored). The left grip pulled past RELOAD_GRIP_PRESSED in the pouch takes
one whenever the weapon has room for it and rounds to fill it (the magazine
in the weapon, if not out already, ejects itself as the new one is taken).
A grip there is the reload's whether or not it takes one, and a grip at the
hip whatever the weapon: no foregrip, no grenade, until the grip is let go
(the grenades are taken anywhere else). So is a grip within
RELOAD_POUCH_MARGIN beyond the reach (a hand closing as it arrives, or a
little short of where the pouch is: with nothing to take there is no light
nor tick to guide it in), and the hand that closed there takes one as it
gets to the pouch. With nothing to take, the left controller ticks twice,
weakly (RELOAD_FULL_*): full. Let go below RELOAD_GRIP_RELEASED,
the one held is dropped; brought within RELOAD_WELL_REACH of where it goes
in, it is pushed home. It is held RELOAD_HELD from the grip along the hand's
axes (forward, left, up): its top, which goes in, above the fist. A
left-hander's pouches are mirrored, at the right hip and behind the right
shoulder (hand_left). */
static const float reload_pouches[2][4] =
{
	{ 0.06f, 0.22f, -0.66f, 0.18f },
	{ -0.12f, 0.14f, -0.08f, 0.20f },
};
#define RELOAD_STICK_PRESSED 0.7f
#define RELOAD_STICK_RELEASED 0.3f
#define RELOAD_GRIP_PRESSED 0.6f
#define RELOAD_GRIP_RELEASED 0.3f
#define RELOAD_POUCH_MARGIN 0.04f
#define RELOAD_FULL_AMPLITUDE 0.12f
#define RELOAD_FULL_SECONDS 0.012f
#define RELOAD_FULL_GAP_MS 90
#define RELOAD_WELL_REACH 0.07f
static const float reload_held[3] = { 0.0f, 0.0f, 0.08f };
/* flipping an overturned vehicle by lifting it (vr.vehicle_flip,
flip_update): while the game offers the player on foot a flip, a grip
pulled past FLIP_GRIP_PRESSED with both hands low (FLIP_LOW below the
recentred eyes: below the waist) and out toward the vehicle (their middle
FLIP_OUT from the eyes toward it, level) takes hold of it, as of a bumper
or the frame's edge: the grip is the lift's until it is let go (below
FLIP_GRIP_RELEASED), whatever else it might have done (a grenade, a
magazine from the pouch, the holster's weapon, the foregrip, with the other
grip a recentring). Both hands holding, both lifted FLIP_LIFT_HEIGHT within
FLIP_LIFT_SECONDS (about 0.7 m/s upward, or faster) flip it: the game's
action pressed for FLIP_PRESS_FRAMES frames, as X would. A swing does not
melee while the grips hold, nor for FLIP_NO_MELEE_MS after the lift. The
hands' heights are kept for FLIP_SAMPLES frames (a power of two: more than
FLIP_LIFT_SECONDS's frames at 144 Hz) */
#define FLIP_GRIP_PRESSED 0.6f
#define FLIP_GRIP_RELEASED 0.3f
#define FLIP_LOW 0.5f
#define FLIP_OUT 0.25f
#define FLIP_LIFT_HEIGHT 0.20f
#define FLIP_LIFT_SECONDS 0.30f
#define FLIP_PRESS_FRAMES 6
#define FLIP_NO_MELEE_MS 500
#define FLIP_SAMPLES 64
/* and its states: the hands not both holding; both holding (the grips
pulled low beside it); lifted (the game's action pressed), until let go */
#define FLIP_IDLE 0
#define FLIP_HOLDING 1
#define FLIP_LIFTED 2
/* grabbing the world (vr.grab_world, grab_update): a weapon the game offers
to pick up is within the right hand's reach with the hand's grip within
GRAB_REACH (metres) of it (of the line along it, less its thickness), a
tick of the controller as the hand gets there; the right grip pulled past
GRAB_GRIP_PRESSED there takes it, the hand all but still (slower than
GRAB_STILL_SPEED, metres a second: a squeeze in a swing passing by is no
grab), and not in a holster (a draw: holsters_update). A swing does not
melee while that grip is held, nor for GRAB_NO_MELEE_MS after. The things
told are the game's for GRAB_TOLD_FRAMES frames. */
#define GRAB_REACH 0.20f
#define GRAB_GRIP_PRESSED 0.6f
#define GRAB_GRIP_RELEASED 0.3f
#define GRAB_STILL_SPEED 2.0f
#define GRAB_NO_MELEE_MS 500
#define GRAB_TOLD_FRAMES 2
/* a control the game would let the player use (a door's panel, a switch) is
pressed by either hand's fingers, PRESS_FINGERS ahead of its grip along
where it points (metres; with the right hand, the weapon's hand, its fist
pushed in), pushed into its face: within its radius of the face's middle,
across it, and from PRESS_BEHIND before the face to PRESS_DEPTH into it
(metres; a control without a face, within its radius of the middle), the
fingers coming there from outside (a click of the controller); again once
they have left it by PRESS_REARM (metres). Not the left hand on a long gun's
foregrip, holding a grenade or a magazine. A swing does not melee for
PRESS_NO_MELEE_MS after a press (a brisk push into it). */
#define PRESS_FINGERS 0.09f
#define PRESS_BEHIND 0.10f
#define PRESS_DEPTH 0.02f
#define PRESS_REARM 0.05f
#define PRESS_NO_MELEE_MS 400
/* the reload's feel in the hands, each a few pulses (their amplitude, their
length and their start after the first, seconds), firmer and longer than the
pouch's ticks (a tick as the hand gets there, a firmer one as it takes one,
two weak ones: full): the magazine ejecting, in the weapon's hand (a clack,
then the magazine sliding out); pushed home, a firm click in the hand
pushing it, settling, and a lighter one in the weapon's; each of the
shotgun's shells, a shorter click in each */
struct haptic_pulse
{
	float amplitude, seconds, start;
};
static const struct haptic_pulse reload_eject_feel[] = { { 0.55f, 0.025f, 0.0f }, { 0.30f, 0.045f, 0.06f } };
static const struct haptic_pulse reload_seat_feel[] = { { 0.90f, 0.03f, 0.0f }, { 0.40f, 0.04f, 0.045f } };
static const struct haptic_pulse reload_seat_weapon_feel[] = { { 0.50f, 0.03f, 0.0f } };
static const struct haptic_pulse reload_shell_feel[] = { { 0.70f, 0.025f, 0.0f } };
static const struct haptic_pulse reload_shell_weapon_feel[] = { { 0.30f, 0.025f, 0.0f } };
/* and the lift's: a hand taking hold (a tick), both holding (a firmer
squeeze in each), and the vehicle lifted over (a heavy thump in each, and
its weight landing) */
static const struct haptic_pulse flip_hand_feel[] = { { 0.25f, 0.025f, 0.0f } };
static const struct haptic_pulse flip_hold_feel[] = { { 0.50f, 0.05f, 0.0f } };
static const struct haptic_pulse flip_lift_feel[] = { { 1.0f, 0.08f, 0.0f }, { 0.6f, 0.06f, 0.11f },
	{ 0.3f, 0.05f, 0.20f } };
/* and a control pressed: a button's click, and its spring back */
static const struct haptic_pulse press_feel[] = { { 0.7f, 0.015f, 0.0f }, { 0.25f, 0.01f, 0.05f } };
#define HAPTIC_PULSES 4
/* a pattern and its count of pulses, for haptic_play */
#define HAPTIC_FEEL(feel) (feel), (int)(sizeof(feel) / sizeof((feel)[0]))
/* each weapon's feel as it fires (vr.weapon_haptics, halo_vr_weapon_fire:
HALO_VR_FEEL_*), in the weapon's hand, the left on a long gun's foregrip
WEAPON_FOREGRIP as strong: any other's tap; the pistol's sharp tap (a short
hard pulse, the slide settling); the assault rifle's fast buzz (a short
pulse a round, the rounds' pulses running together); the shotgun's heavy
thump, long and slow to die; the sniper rifle's crack, very short and full,
and its decay; the rocket launcher's long push, rising and falling; the
plasma weapons' soft pulse; the plasma pistol's charged shot's surge,
rising to full and ebbing; the needler's ticks (a double tick a needle);
the flamethrower's rumble (each shot's pulse longer than the time to the
next, so that they run on). Never over the reload's or the lift's patterns
(haptic_play) while they play, but over the game's rumble. The plasma
pistol charging hums in its hand, WEAPON_CHARGE_HUM rising to its full as
it charges (halo_vr_weapon_charge). */
static const struct haptic_pulse weapon_tap_feel[] = { { 0.50f, 0.03f, 0.0f } };
static const struct haptic_pulse weapon_pistol_feel[] = { { 1.0f, 0.018f, 0.0f }, { 0.25f, 0.03f, 0.025f } };
static const struct haptic_pulse weapon_rifle_feel[] = { { 0.60f, 0.035f, 0.0f } };
static const struct haptic_pulse weapon_shotgun_feel[] = { { 1.0f, 0.07f, 0.0f }, { 0.65f, 0.06f, 0.08f },
	{ 0.30f, 0.08f, 0.15f } };
static const struct haptic_pulse weapon_sniper_feel[] = { { 1.0f, 0.02f, 0.0f }, { 0.70f, 0.04f, 0.025f },
	{ 0.40f, 0.06f, 0.07f }, { 0.15f, 0.10f, 0.14f } };
static const struct haptic_pulse weapon_rocket_feel[] = { { 0.60f, 0.06f, 0.0f }, { 1.0f, 0.15f, 0.05f },
	{ 0.60f, 0.12f, 0.20f }, { 0.25f, 0.15f, 0.32f } };
static const struct haptic_pulse weapon_plasma_feel[] = { { 0.45f, 0.05f, 0.0f } };
static const struct haptic_pulse weapon_charged_feel[] = { { 0.50f, 0.05f, 0.0f }, { 0.90f, 0.08f, 0.04f },
	{ 1.0f, 0.12f, 0.12f }, { 0.40f, 0.15f, 0.24f } };
static const struct haptic_pulse weapon_needler_feel[] = { { 0.40f, 0.012f, 0.0f }, { 0.20f, 0.01f, 0.03f } };
static const struct haptic_pulse weapon_flame_feel[] = { { 0.45f, 0.08f, 0.0f } };
static const struct
{
	const struct haptic_pulse *pulses;
	int count;
} weapon_feels[HALO_VR_FEELS] =
{
	{ HAPTIC_FEEL(weapon_tap_feel) },
	{ HAPTIC_FEEL(weapon_pistol_feel) },
	{ HAPTIC_FEEL(weapon_rifle_feel) },
	{ HAPTIC_FEEL(weapon_shotgun_feel) },
	{ HAPTIC_FEEL(weapon_sniper_feel) },
	{ HAPTIC_FEEL(weapon_rocket_feel) },
	{ HAPTIC_FEEL(weapon_plasma_feel) },
	{ HAPTIC_FEEL(weapon_charged_feel) },
	{ HAPTIC_FEEL(weapon_needler_feel) },
	{ HAPTIC_FEEL(weapon_flame_feel) },
};
#define WEAPON_FOREGRIP 0.35f
#define WEAPON_CHARGE_HUM 0.35f
/* a hit on the player (halo_vr_hit): a pulse and its echo, in the hand on
the side it came from, from where the head faces (as far to the side as
it came from: from straight ahead or behind, or from above, in both
alike), from HIT_LEAST as strong for a graze to full (vr_feel.c's
HIT_DAMAGE); and the shields breaking (halo_vr_shields_broken), in both
hands, a crackle of three sharp pulses and a fading rumble, stronger on the
side of a hit HIT_RECENT_MS before, which broke them */
static const struct haptic_pulse hit_feel[] = { { 1.0f, 0.05f, 0.0f }, { 0.40f, 0.06f, 0.06f } };
static const struct haptic_pulse shields_broken_feel[] = { { 1.0f, 0.03f, 0.0f }, { 0.80f, 0.03f, 0.05f },
	{ 0.60f, 0.03f, 0.10f }, { 0.30f, 0.20f, 0.15f } };
#define HIT_LEAST 0.35f
#define HIT_RECENT_MS 300
/* debug.vr_test_hands: the poses it gives the controllers (enough for a
swing posed frame by frame), each held this many frames unless it says */
#define TEST_HANDS_POSES 512
#define TEST_HANDS_FRAMES 288
/* and its buttons (vr_host.h's VR_BUTTON_*, with the triggers as these)
pressed for the first frames of the pose */
#define TEST_HANDS_PRESS_FRAMES 20
#define TEST_HANDS_RIGHT_TRIGGER (1 << 16)
#define TEST_HANDS_LEFT_TRIGGER (1 << 17)
#define TEST_HANDS_RIGHT_GRIP (1 << 18)
/* and its numbers in each pose */
#define TEST_HANDS_NUMBERS 25
#define TEST_HANDS_RIGHT_STICK_DOWN (1 << 19)
/* and where the head nods about, as a person's does: the lower neck, so far
behind and below the eyes (metres) */
#define TEST_HEAD_NECK_BACK 0.08f
#define TEST_HEAD_NECK_DOWN 0.15f
/* the aim steadied while the smart-link magnifies it (aim_steady: a
one-euro filter of its direction): smoothed with a cutoff of STEADY_HZ over
the magnification less one (2 Hz at 2x, 0.29 Hz at 8x), less still
(STEADY_TWO_HANDED times) with the left hand on the foregrip, raised by
STEADY_GAIN for each degree a second the hand turns (its turn smoothed with
a cutoff of STEADY_VELOCITY_HZ, so that a tremor's back and forth cancels and
a swing does not); the magnification eased over STEADY_ZOOM_SECONDS as it
changes (frequencies in Hz) */
#define STEADY_HZ 2.0f
#define STEADY_TWO_HANDED 1.5f
#define STEADY_GAIN 0.15f
#define STEADY_VELOCITY_HZ 1.5f
#define STEADY_ZOOM_SECONDS 0.1f
/* the aim assist (vr.aim_assist; aim_assist_step): near a target the
game's magnetism would be drawn to, the aim's turn away from it (the
hand's, as far as it does not follow the target) slowed by ASSIST_FRICTION of
it at the target's full magnetism level, and zoomed, the aim drawn toward it
at up to ASSIST_PULL degrees a second (both times vr.aim_assist; the turn
slowed by ASSIST_FRICTION_MOST at most); the aim no more than ASSIST_MOST
degrees from the hand's, and back to it, away from a target, by half the
hand's own turn (a still hand's aim stays still); with the left hand on a long
gun's foregrip (vr.two_handed), all of it ASSIST_TWO_HANDED stronger, as a
braced gun is steadier on its target */
#define ASSIST_FRICTION 0.5f
#define ASSIST_FRICTION_MOST 0.8f
#define ASSIST_PULL 1.0f
#define ASSIST_MOST 3.0f
#define ASSIST_TWO_HANDED 0.5f
/* debug.vr_test_shake: a hand's shake, half a slow wander (noise smoothed
with a cutoff of SHAKE_WANDER_HZ) and half a tremor (SHAKE_TREMOR_HZ) */
#define SHAKE_WANDER_HZ 1.5f
#define SHAKE_TREMOR_HZ 9.0f
/* vr.foveation: the inset's part of each eye's image across (and the
periphery's resolution), by default and at least and most. The inset
follows the gaze over FOVEA_FOLLOW_SECONDS (a time constant) after a move of
FOVEA_SACCADE_DEGREES or more (a saccade: there before the eyes see again),
over FOVEA_SETTLE_SECONDS after smaller ones (the eyes' drift and the
tracker's noise, which would move it for nothing); with the gaze lost (a
blink), it stays FOVEA_LOST_SECONDS, then returns to the middle of the view
over FOVEA_RETURN_SECONDS */
#define FOVEA_INSET 0.5f
#define FOVEA_INSET_SMALLEST 0.3f
#define FOVEA_INSET_LARGEST 0.8f
#define FOVEA_SACCADE_DEGREES 3.0f
#define FOVEA_FOLLOW_SECONDS 0.012f
#define FOVEA_SETTLE_SECONDS 0.08f
#define FOVEA_LOST_SECONDS 0.3f
#define FOVEA_RETURN_SECONDS 0.5f

static struct
{
	int initialized;
	int failed;
	struct vr_host_info info;
	int frame; /* a frame is begun */
	unsigned long presents;
	struct vr_host_views views;
	/* the views as located last, for the host's late latch (halo_vr_latch) */
	struct vr_host_latch latch;
	/* debug.vr_test_head: the head's place, and the time it turns by */
	int test_head_placed;
	float test_head_centre[3];
	double test_head_seconds;
	/* debug.vr_test_jitter: this frame's move and turn of the head */
	float jitter_offset[3];
	float jitter_turn[4];
	struct vr_host_input input;
	/* the controllers' poses in views are as the runtime (or debug.vr_test_
	hands) gave them, each hand's its own controller's: not yet assigned to
	the hands (hands_assign_views) */
	int views_unassigned;
	/* the recentred origin, in the play space */
	int origin_valid;
	/* whether the session had the focus last frame: the headset going on
	(the focus coming) recentres, as the pose before it may be the headset
	lying wherever it was */
	int was_focused;
	float origin_position[3];
	float origin_yaw;
	/* the turn, and whether the aim follows the head */
	float body_yaw;
	int aiming;
	int aimed;
	int aim_synced;
	/* the right controller's line halo_vr_aim aimed along last (game axes
	from the recentred head), if it did (halo_vr_aim_line) */
	int aim_line_valid;
	float aim_line_position[3];
	float aim_line_forward[3];
	/* a vehicle's handlebars: when they were placed last (vr.presents), the
	handles (game axes from the recentred head, metres), each hand's hold,
	the seat as placed (its controls, the vehicle's heading as the game has
	it now, which the bars and the wheel steer from), the turn and pitch
	they steer by now (radians: the facing's from that heading), how the
	hands pointed as both took hold (their pitch), and the line between
	them then across the wheel's axis */
	unsigned long bars_placed;
	int bars_placed_valid;
	struct halo_vr_seat seat;
	float bars_wheel_zero[3];
	int bars_held[2];
	float bars_turn;
	float bars_pitch;
	float bars_pitch_zero;
	/* driving a vehicle steered toward the aim (halo_vr_seat_drive): when
	told last (vr.presents) */
	unsigned long drive_told;
	int drive_told_valid;
	/* the comfort vignette: asked for (and at which frame), and as drawn */
	float vignette_asked;
	unsigned long vignette_asked_at;
	float vignette;
	/* debug.vr_test_target: the right controller pointed along this
	(halo_vr_test_point: game axes from the recentred head), if set */
	int test_point_valid;
	float test_point[3];
	/* the aim steadied (aim_steady): as the last frame left it, and as this
	frame began from it (each frame's poses are located twice, the aim's
	and the draws', both steadied from the same state): the controller's
	direction (OpenXR axes), its turn (radians a second, smoothed), the
	steadied direction and the magnification it was steadied for (eased);
	the frame it is of (vr.presents); and this frame's magnification
	(halo_vr_aim_zoom: 1, none) */
	struct aim_steady
	{
		int valid;
		float raw[3];
		float velocity[3];
		float out[3];
		float magnification;
		/* the aim assist's: the aim, assisted, and the target it was drawn
		to, as the direction of its middle from the controller (OpenXR
		axes) */
		float assisted[3];
		long target;
		float target_middle[3];
	} steady, steady_before;
	unsigned long steady_frame;
	float steady_zoom;
	/* this frame's target for the aim assist (halo_vr_aim_assist): which
	(-1: none), how near the aim (its magnetism level), and the direction of
	its middle from the controller (OpenXR axes) */
	long assist_target;
	float assist_level;
	float assist_middle[3];
	/* and the ends of the part of it in sight from the controller, along its
	body (OpenXR axes): the aim is held to, and drawn to, the nearest point
	of it */
	float assist_low[3];
	float assist_high[3];
	/* debug.vr_test_shake: the shake's wander (degrees: yaw, pitch;
	millimetres: each hand's), the tremor's time and phases, and this
	frame's turn of the right controller and move of each */
	float shake_wander[2];
	float shake_wander_place[2][3];
	double shake_time;
	float shake_phase[2];
	float shake_turn[2];
	float shake_offset[2][3];
	int snap_latched;
	float recentre_time;
	/* a gamepad's Back held (halo_vr_gamepad_back), and for how long */
	int gamepad_back;
	float gamepad_recentre_time;
	unsigned long last_ticks;
	/* the image of each swapchain for this frame */
	int acquired[VR_SWAPCHAIN_COUNT];
	int force_render;
	int menus;
	/* the eyes' depth this frame, and its range */
	int depth_written;
	/* the melee by a swing: the right hand's places from the head and its
	own, each at its time (seconds), whether the hand swings, whether the
	swing has melee'd or not yet (armed), whether it began in the holster a
	weapon was just drawn from (bringing it out), how far it has gone and
	for how long, its fastest, whether the grip was held (and as it began)
	and why it has not melee'd (debug.vr_melee_log), when the B button it
	presses is let go, when the last melee was and when a guard last held
	(GetTickCount) */
	unsigned int melee_sample;
	double melee_clock;
	float melee_hand[MELEE_SAMPLES][3];
	float melee_from_head[MELEE_SAMPLES][3];
	double melee_times[MELEE_SAMPLES];
	int melee_valid[MELEE_SAMPLES];
	int melee_swinging;
	int melee_fired;
	int melee_from_holster;
	float melee_travel;
	float melee_seconds;
	int melee_frames;
	float melee_fastest;
	int melee_grip;
	int melee_grip_before;
	const char *melee_guard;
	unsigned long melee_release;
	unsigned long melee_ticks;
	unsigned long melee_guard_ticks;
	/* the right hand's speed at the last frame (metres a second: from the
	melee's samples, for holsters_update) */
	float hand_speed;
	/* the right index finger on the trigger (0 off, 1 on, eased), and
	whether the trigger senses touch (-1 not known yet), as last logged */
	float trigger_touch;
	int trigger_touch_sensed;
	/* both hands on a long gun: whether the left holds the foregrip, how
	far the aim has turned to the line between the hands (0 to 1), that
	line, smoothed (OpenXR axes), and whether the weapon is one */
	int two_handed;
	float two_handed_weight;
	float two_handed_line[3];
	int long_gun;
	/* the grenade thrown with the left hand: the gesture's state (THROW_*),
	how long it has been in it, the left hand's last places (OpenXR axes)
	and velocities, each at its time (seconds), whether the grip was
	pressed last frame, the farthest forward of the eyes the hand has been
	since it took the grenade, the left trigger's frames left, the game's
	readiness, and the throw; whether the hand went forward faster than a
	swing last frame (debug.vr_throw_log: a swing not taken, once) */
	int throw_state;
	int throw_fast;
	float throw_seconds;
	double throw_clock;
	unsigned int throw_sample;
	float throw_places[THROW_SAMPLES][3];
	float throw_velocities[THROW_SAMPLES][3];
	double throw_times[THROW_SAMPLES];
	int throw_places_valid[THROW_SAMPLES];
	int throw_grip_pressed;
	float throw_nearest;
	int throw_press_frames;
	int throw_ready;
	struct halo_vr_throw throw_out;
	/* the body's heading (body_update: radians, game axes from the
	recentred head) and whether it is known */
	int body_heading_valid;
	float body_heading;
	/* the holsters: the stowed weapon's (the game's, HALO_VR_HOLSTER_*), the
	holsters as they are this frame, whether the right grip was pressed last
	frame, the Y button's frames left, when a weapon was last drawn
	(GetTickCount), and the seconds since the hand nearing the shoulder's
	holster last ticked */
	int holster_stowed;
	struct halo_vr_holsters holsters;
	int holster_grip_pressed;
	/* the right grip was taken in a holster, as a draw is, and is held; a
	weapon was drawn, and the hand has not left the holster since */
	int holster_gripped;
	int holster_drawn;
	int holster_press_frames;
	unsigned long holster_ticks;
	float holster_near_seconds;
	/* the physical crouch: whether the game could crouch the player this
	frame (halo_vr_crouch_able) and the frame before, the head low (with
	CROUCH_HYSTERESIS), the game's crouch pressed for it, and pressed by a
	hand (the left stick, a gamepad) */
	int crouch_able;
	int crouch_able_frame;
	int head_low;
	int crouching;
	int crouch_button;
	/* the reload by hand: the weapon as the game has it, the reload as it
	is this frame, the weapon it is of, whether the right stick was down
	last frame, whether the left grip was pressed last frame and was the
	reload's (it took one, or was pressed at a pouch: nothing else takes
	that grip until it is let go) and whether it was pressed by a pouch and
	has taken nothing yet, whether the hand was in the pouch last frame, and
	when the second tick of "full" is due (GetTickCount, 0 none) */
	struct halo_vr_reload_weapon reload_weapon;
	struct halo_vr_reload reload;
	long reload_weapon_index;
	int reload_stick_down;
	int reload_grip_pressed;
	int reload_grip_taken;
	int reload_grip_waiting;
	int reload_in_pouch;
	unsigned long reload_full_tick;
	/* each hand's haptic pattern playing (haptic_play): its pulses, how many
	have been sent, when it began and when it ends (GetTickCount) */
	struct haptic_pulse haptic_pulses[2][HAPTIC_PULSES];
	int haptic_count[2];
	int haptic_sent[2];
	unsigned long haptic_ticks[2];
	unsigned long haptic_end[2];
	/* ... and when the last of the reload's or the lift's ends, which the
	weapons' and the hits' feel (haptic_feel) does not play over */
	unsigned long haptic_kept_end[2];
	/* the last hit's side (-1 left to 1 right) and when (GetTickCount), for
	the shields breaking under it */
	float hit_side;
	unsigned long hit_ticks;
	/* the lift of an overturned vehicle (flip_update): the game's offer
	(halo_vr_flip) and where the vehicle is (game axes from the recentred
	head), the offer logged; each grip held, and whether it is the lift's;
	the lift's state (FLIP_*), the frames the game's action is pressed for
	it, and when the last one was; the hands' heights kept, and their times */
	int flip_offered;
	int flip_offer_logged;
	int flip_vehicle_known;
	float flip_vehicle[3];
	int flip_grip_pressed[2];
	int flip_grip[2];
	/* climbing a vehicle by its holds (halo_vr_climb): each hand's grip
	held on a hold (the climb's alone), and each hand at a hold (its grip
	the climb's when taken), as the game last told them */
	int climb_grip[2];
	int climb_near[2];
	int flip_state;
	int flip_press_frames;
	unsigned long flip_ticks;
	unsigned int flip_sample;
	float flip_clock;
	float flip_held_clock;
	float flip_times[FLIP_SAMPLES];
	float flip_heights[2][FLIP_SAMPLES];
	int flip_heights_valid[FLIP_SAMPLES];
	/* the grab (grab_update): the things told (halo_vr_grab) and when
	(vr.presents); the weapon the right hand is nearest and how far from
	its reach, the one it is in reach of (-1 none), whether the right grip
	was pressed last frame and was taken at a weapon (held since), the
	weapon taken and not yet told to the game (-1 none), and when the last
	was taken (GetTickCount) */
	struct halo_vr_grab grab_told;
	unsigned long grab_told_at;
	long grab_nearest;
	float grab_distance;
	long grab_reach;
	int grab_grip_pressed;
	int grab_gripped;
	long grab_taken;
	unsigned long grab_ticks;
	/* and each hand's fingers in a control (pressing it, until they leave
	it), the control pressed not yet told to the game (-1 none), and when
	the last was pressed (GetTickCount) */
	int press_in[2];
	long grab_pressed;
	unsigned long press_ticks;
	float depth_near, depth_far;
	/* this frame's scope (halo_vr_scope), if it was drawn */
	int scope_set;
	struct halo_vr_scope scope;
	/* the presented frame, ended when the next begins (halo_vr_present) */
	struct vr_host_layers pending_layers;
	int pending;
	int attempted;
	/* vr.foveation (halo_vr_foveation): 0 off, 1 fixed, 2 by the eyes'
	gaze (from the runtime, or debug.vr_test_gaze); the inset's part of
	the eyes; the gaze followed (tangents right and up from the head's
	forward), the frame it was last followed in, the seconds it has been
	lost, and whether it was ever seen */
	int foveation;
	float foveation_inset;
	float fovea_gaze[2];
	unsigned long fovea_frame;
	float fovea_lost;
	int fovea_seen;
	/* debug.gpu_stats: the frames' timing */
	int statistics;
	Uint64 frame_start, wait_start;
	unsigned long timed_frames, late_frames;
	double frame_seconds, wait_seconds, worst_seconds;
} vr;

/* ---------- settings */

static struct
{
	unsigned long read_at;
	int smooth_turn;
	float snap_angle;
	float smooth_speed;
	int standing;
	float player_height;
	float world_scale;
	float hud_distance;
	float hud_size;
	float menu_distance;
	float menu_width;
	int controller_aim;
	int gamepad_aim;
	/* vr.dominant_hand = "left" (not with vr.aim = "gamepad", where the
	controllers are a gamepad's halves): the left controller is the weapon's
	hand, which the rest of this file calls the right (hands_assign); and
	vr.swap_sticks (by default as the dominant hand): the right stick moves,
	the left turns */
	int left_handed;
	int swap_sticks;
	/* vr.gamepad_view: the view's heading in steps ("snap"), and pitched
	with the facing ("camera") */
	int gamepad_snap;
	int gamepad_pitch;
	float weapon_offset[3];
	int melee_gesture;
	int holsters;
	int physical_crouch;
	/* vr.scopes and vr.scope_view: HALO_VR_SCOPES_*, 0 off; the smart-link's
	window, how wide (degrees, vr.smartlink_size), where it is (on the visor,
	vr.smartlink_place = "visor"; else at the reticle), and how the weapon in
	hand shows over it (vr.scope_gun: 0 hidden, 1 ghost, 2 visible) */
	int scopes;
	float smartlink_degrees;
	int smartlink_visor;
	int scope_gun;
	int manual_reload;
	/* vr.vehicle_flip: what flips an overturned vehicle, X ("button",
	"both") and a lift of both hands ("lift", "both") */
	int flip_button;
	int flip_lift;
	/* vr.grab_world: the hands grab the world (a weapon on the ground, a
	control) */
	int grab_world;
	/* vr.grenade_throw: the hand throws ("gesture", "both"), and not the
	left trigger too ("gesture"); debug.vr_throw_log */
	int throw_gesture;
	int throw_gesture_only;
	int throw_log;
	/* debug.vr_melee_log: the swings that did not melee too */
	int melee_log;
	int vehicle_first_person;
	/* vr.ghost_steering = "handlebars", vr.warthog_steering = "wheel",
	vr.comfort_vignette */
	int handlebars;
	int wheel;
	int comfort_vignette;
	/* vr.late_latch */
	int late_latch;
	float test_turn;
	float test_jitter;
	/* debug.vr_test_shake: the right controller's shake (degrees, RMS) and
	both hands' (metres) */
	float test_shake;
	float test_shake_place;
	/* debug.vr_aim_log */
	int aim_log;
	/* vr.aim_assist: how strong (0: none) */
	float aim_assist;
	/* vr.weapon_haptics, vr.recoil: how strong the weapons' and the hits'
	feel in the hands is, and the weapon's kick in the hand (0: none, 1) */
	float weapon_haptics;
	float recoil;
	/* debug.vr_test_head: the head held still, turned left and right by
	this many degrees over this many seconds, pitched up this many and
	rolled this many to the right */
	int test_head;
	float test_head_yaw;
	float test_head_period;
	float test_head_pitch;
	float test_head_roll;
	/* debug.vr_test_fov: each eye's field of view (degrees: outward, inward,
	up, down) and its turn outward (degrees), instead of the runtime's */
	int test_fov;
	float test_fov_angles[5];
	/* debug.vr_test_gaze: the eyes' gaze (degrees right and up from the
	head's forward), and the seconds after which it jumps to the opposite
	and back (0: never) */
	int test_gaze;
	float test_gaze_angles[2];
	float test_gaze_period;
	int two_handed;
	int floating_hands;
	int arms;
	float shoulder_offset[3];
	/* debug.vr_test_hands: each pose's left and right hands (metres right,
	up, forward from the recentred head; degrees of yaw left, pitch up, roll
	right), the left grip's pull, the buttons pressed, the right trigger's
	pull, the head's turn (degrees left), the frames it lasts, the frames the
	hands take to move to it from the pose before, the head lowered (metres),
	nodded about the neck (degrees up) and moved in the room (metres forward
	and right), and the right grip's pull; the poses' frames in all */
	int test_hand_poses;
	float test_hands[TEST_HANDS_POSES][TEST_HANDS_NUMBERS];
	unsigned long test_hands_frames;
} settings = { (unsigned long)-1 };

/* debug.vr_test_hands: "lx ly lz lyaw lpitch lroll, rx ry rz ryaw rpitch
rroll[, grip[, buttons[, trigger[, head yaw[, frames[, move frames[, head
down[, head pitch[, head forward[, head right[, right grip]]]]]]]]]]]",
poses separated by ';', each lasting its frames (TEST_HANDS_FRAMES if 0) */
static void test_hands_read(const char *text)
{
	settings.test_hand_poses = 0;
	settings.test_hands_frames = 0;
	while (text && *text && settings.test_hand_poses < TEST_HANDS_POSES)
	{
		float *pose = settings.test_hands[settings.test_hand_poses];
		char numbers[256];
		const char *end = strchr(text, ';');
		size_t length = end ? (size_t)(end - text) : strlen(text);
		char *c;
		int count;

		if (length >= sizeof(numbers))
			length = sizeof(numbers) - 1;
		memcpy(numbers, text, length);
		numbers[length] = 0;
		for (c = numbers; *c; c++)
		{
			if (*c == ',')
				*c = ' ';
		}
		pose[12] = pose[13] = pose[14] = pose[15] = pose[16] = pose[17] = pose[18] = 0.0f;
		pose[19] = pose[20] = pose[21] = pose[22] = pose[23] = pose[24] = 0.0f;
		count = sscanf(numbers, "%f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f",
			&pose[0], &pose[1], &pose[2], &pose[3], &pose[4], &pose[5], &pose[6], &pose[7], &pose[8], &pose[9],
			&pose[10], &pose[11], &pose[12], &pose[13], &pose[14], &pose[15], &pose[16], &pose[17], &pose[18],
			&pose[19], &pose[20], &pose[21], &pose[22], &pose[23], &pose[24]);
		if (count >= 12)
		{
			if (pose[16] < 1.0f)
				pose[16] = (float)TEST_HANDS_FRAMES;
			settings.test_hands_frames += (unsigned long)pose[16];
			settings.test_hand_poses++;
		}
		text = end ? end + 1 : NULL;
	}
}

static void settings_read(void)
{
	if (settings.read_at == config_changes())
		return;
	settings.read_at = config_changes();
	settings.smooth_turn = !strcmp(config_string("vr.turn"), "smooth");
	settings.snap_angle = (float)config_real("vr.snap_turn_angle") * (float)M_PI / 180.0f;
	settings.smooth_speed = (float)config_real("vr.smooth_turn_speed") * (float)M_PI / 180.0f;
	settings.standing = !strcmp(config_string("vr.height"), "standing");
	settings.player_height = (float)config_real("vr.player_height");
	settings.world_scale = (float)config_real("vr.world_scale");
	if (settings.world_scale <= 0.0f)
		settings.world_scale = 1.0f;
	settings.hud_distance = (float)config_real("vr.hud_distance");
	if (settings.hud_distance < 0.3f)
		settings.hud_distance = 0.3f;
	settings.hud_size = (float)config_real("vr.hud_size");
	if (settings.hud_size < 20.0f || settings.hud_size > 120.0f)
		settings.hud_size = 60.0f;
	settings.menu_distance = (float)config_real("vr.menu_distance");
	settings.menu_width = (float)config_real("vr.menu_width");
	settings.gamepad_aim = !strcmp(config_string("vr.aim"), "gamepad");
	settings.controller_aim = !settings.gamepad_aim && strcmp(config_string("vr.aim"), "head") != 0;
	settings.left_handed = !settings.gamepad_aim && !strcmp(config_string("vr.dominant_hand"), "left");
	settings.swap_sticks = !settings.gamepad_aim && (!strcmp(config_string("vr.swap_sticks"), "yes") ||
		(settings.left_handed && strcmp(config_string("vr.swap_sticks"), "no") != 0));
	settings.gamepad_snap = !strcmp(config_string("vr.gamepad_view"), "snap");
	settings.gamepad_pitch = !settings.gamepad_snap && strcmp(config_string("vr.gamepad_view"), "level") != 0;
	settings.melee_gesture = config_boolean("vr.melee_gesture");
	settings.throw_gesture_only = !strcmp(config_string("vr.grenade_throw"), "gesture");
	settings.throw_gesture = settings.throw_gesture_only || !strcmp(config_string("vr.grenade_throw"), "both");
	settings.holsters = config_boolean("vr.holsters");
	settings.physical_crouch = config_boolean("vr.physical_crouch");
	settings.scopes = !config_boolean("vr.scopes") || !strcmp(config_string("vr.scope_view"), "off") ? 0 :
		!strcmp(config_string("vr.scope_view"), "eye") ? HALO_VR_SCOPES_EYE : HALO_VR_SCOPES_SMARTLINK;
	settings.smartlink_degrees = (float)config_real("vr.smartlink_size");
	if (settings.smartlink_degrees < SMARTLINK_NARROWEST || settings.smartlink_degrees > SMARTLINK_WIDEST)
		settings.smartlink_degrees = settings.smartlink_degrees < SMARTLINK_NARROWEST ? SMARTLINK_NARROWEST :
			SMARTLINK_WIDEST;
	settings.smartlink_visor = !strcmp(config_string("vr.smartlink_place"), "visor");
	settings.scope_gun = !strcmp(config_string("vr.scope_gun"), "visible") ? 2 :
		!strcmp(config_string("vr.scope_gun"), "ghost") ? 1 : 0;
	settings.manual_reload = config_boolean("vr.manual_reload");
	settings.flip_lift = strcmp(config_string("vr.vehicle_flip"), "button") != 0;
	settings.flip_button = !settings.flip_lift || !strcmp(config_string("vr.vehicle_flip"), "both");
	settings.grab_world = config_boolean("vr.grab_world");
	settings.throw_log = config_boolean("debug.vr_throw_log");
	settings.melee_log = config_boolean("debug.vr_melee_log");
	settings.vehicle_first_person = strcmp(config_string("vr.vehicle_view"), "third_person") != 0;
	settings.handlebars = strcmp(config_string("vr.ghost_steering"), "look") != 0;
	settings.wheel = strcmp(config_string("vr.warthog_steering"), "look") != 0;
	settings.comfort_vignette = config_boolean("vr.comfort_vignette");
	settings.late_latch = config_boolean("vr.late_latch");
	settings.test_turn = (float)config_real("debug.vr_test_turn") * (float)M_PI / 180.0f;
	settings.test_jitter = (float)config_real("debug.vr_test_jitter") * 0.001f;
	settings.test_shake = settings.test_shake_place = 0.0f;
	sscanf(config_string("debug.vr_test_shake"), "%f %f", &settings.test_shake, &settings.test_shake_place);
	settings.test_shake_place *= 0.001f;
	settings.aim_log = config_boolean("debug.vr_aim_log");
	settings.aim_assist = (float)config_real("vr.aim_assist");
	if (settings.aim_assist < 0.0f || settings.aim_assist > 2.0f)
		settings.aim_assist = settings.aim_assist < 0.0f ? 0.0f : 2.0f;
	settings.weapon_haptics = (float)config_real("vr.weapon_haptics");
	if (settings.weapon_haptics < 0.0f || settings.weapon_haptics > 1.0f)
		settings.weapon_haptics = settings.weapon_haptics < 0.0f ? 0.0f : 1.0f;
	settings.recoil = (float)config_real("vr.recoil");
	if (settings.recoil < 0.0f || settings.recoil > 1.0f)
		settings.recoil = settings.recoil < 0.0f ? 0.0f : 1.0f;
	settings.test_head_yaw = 0.0f;
	settings.test_head_period = 0.0f;
	settings.test_head_pitch = 0.0f;
	settings.test_head_roll = 0.0f;
	settings.test_head = sscanf(config_string("debug.vr_test_head"), "%f %f %f %f", &settings.test_head_yaw,
		&settings.test_head_period, &settings.test_head_pitch, &settings.test_head_roll) >= 1;
	settings.test_fov_angles[4] = 0.0f;
	settings.test_fov = sscanf(config_string("debug.vr_test_fov"), "%f %f %f %f %f", &settings.test_fov_angles[0],
		&settings.test_fov_angles[1], &settings.test_fov_angles[2], &settings.test_fov_angles[3],
		&settings.test_fov_angles[4]) >= 4;
	settings.test_gaze_period = 0.0f;
	settings.test_gaze = sscanf(config_string("debug.vr_test_gaze"), "%f %f %f", &settings.test_gaze_angles[0],
		&settings.test_gaze_angles[1], &settings.test_gaze_period) >= 2;
	settings.two_handed = config_boolean("vr.two_handed");
	settings.floating_hands = strcmp(config_string("vr.hands"), "game") != 0;
	settings.arms = settings.floating_hands && strcmp(config_string("vr.hands"), "floating") != 0;
	settings.shoulder_offset[0] = 0.19f;
	settings.shoulder_offset[1] = -0.20f;
	settings.shoulder_offset[2] = -0.07f;
	sscanf(config_string("vr.shoulders"), "%f , %f , %f", &settings.shoulder_offset[0],
		&settings.shoulder_offset[1], &settings.shoulder_offset[2]);
	test_hands_read(config_string("debug.vr_test_hands"));
	settings.weapon_offset[0] = 0.15f;
	settings.weapon_offset[1] = -0.22f;
	settings.weapon_offset[2] = 0.30f;
	sscanf(config_string("vr.weapon_offset"), "%f , %f , %f", &settings.weapon_offset[0],
		&settings.weapon_offset[1], &settings.weapon_offset[2]);
}

int halo_vr_enabled(void)
{
	return config_boolean("vr.enabled");
}

int halo_vr_running(void)
{
	return vr.initialized;
}

/* ---------- maths: OpenXR's axes (x right, y up, z backwards) */

static void quaternion_rotate(const float q[4], const float v[3], float out[3])
{
	/* v + 2w(q x v) + 2 q x (q x v) */
	float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
	float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
	float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);

	out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
	out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
	out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

/* the yaw (about y) the orientation looks along */
static float quaternion_yaw(const float q[4])
{
	static const float backwards[3] = { 0.0f, 0.0f, -1.0f };
	float forward[3];

	quaternion_rotate(q, backwards, forward);
	return atan2f(-forward[0], -forward[2]);
}

static void quaternion_multiply(const float a[4], const float b[4], float out[4])
{
	float r[4];

	r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	memcpy(out, r, sizeof(r));
}

/* an OpenXR orientation (x, y, z, w) whose axes are these (right, up,
back: orthonormal) */
static void quaternion_from_axes(const float x[3], const float y[3], const float z[3], float q[4])
{
	float trace = x[0] + y[1] + z[2], s;

	if (trace > 0.0f)
	{
		s = sqrtf(trace + 1.0f) * 2.0f;
		q[3] = 0.25f * s;
		q[0] = (y[2] - z[1]) / s;
		q[1] = (z[0] - x[2]) / s;
		q[2] = (x[1] - y[0]) / s;
	}
	else if (x[0] > y[1] && x[0] > z[2])
	{
		s = sqrtf(1.0f + x[0] - y[1] - z[2]) * 2.0f;
		q[3] = (y[2] - z[1]) / s;
		q[0] = 0.25f * s;
		q[1] = (y[0] + x[1]) / s;
		q[2] = (z[0] + x[2]) / s;
	}
	else if (y[1] > z[2])
	{
		s = sqrtf(1.0f + y[1] - x[0] - z[2]) * 2.0f;
		q[3] = (z[0] - x[2]) / s;
		q[0] = (y[0] + x[1]) / s;
		q[1] = 0.25f * s;
		q[2] = (z[1] + y[2]) / s;
	}
	else
	{
		s = sqrtf(1.0f + z[2] - x[0] - y[1]) * 2.0f;
		q[3] = (x[1] - y[0]) / s;
		q[0] = (z[0] + x[2]) / s;
		q[1] = (z[1] + y[2]) / s;
		q[2] = 0.25f * s;
	}
}

/* an OpenXR vector turned by -yaw about y, in game axes */
static void game_axes(const float v[3], float yaw, float out[3])
{
	float c = cosf(-yaw), s = sinf(-yaw);
	float x = v[0] * c + v[2] * s;
	float z = -v[0] * s + v[2] * c;

	out[0] = -z;
	out[1] = -x;
	out[2] = v[1];
}

/* game axes turned by yaw about y, as an OpenXR vector (game_axes'
inverse) */
static void play_axes(const float g[3], float yaw, float out[3])
{
	float c = cosf(-yaw), s = sinf(-yaw);
	float x = -g[1], z = -g[0];

	out[0] = x * c - z * s;
	out[1] = g[2];
	out[2] = x * s + z * c;
}

/* ---------- the session */

static void acquired_clear(void);
static void pose_point(const float position[3], const float orientation[4], const float *forward_given,
	const float *up_given, const float along[3], float point[3]);
static void haptic_play(int hand, const struct haptic_pulse *pulses, int count);

int halo_vr_initialize(void)
{
	if (vr.initialized || vr.failed)
		return vr.initialized;
	if (!halo_vr_enabled())
	{
		vr.failed = 1;
		return 0;
	}
	settings_read();
	memset(&vr.info, 0, sizeof(vr.info));
	vr.info.scale = (float)config_real("vr.resolution_scale");
	if (vr.info.scale < 0.25f || vr.info.scale > 2.0f)
		vr.info.scale = 1.0f;
	vr.info.hud_width = HUD_WIDTH;
	vr.info.hud_height = HUD_HEIGHT;
	/* (no image for the visor's glass: it is drawn into the eyes', vr_visor.c) */
	vr.info.visor_width = vr.info.visor_height = 0;
	vr_visor_panel_sizes(vr.info.panel_width, vr.info.panel_height);
	{
		int size = (int)((settings.scopes == HALO_VR_SCOPES_SMARTLINK ? SCOPE_WINDOW_IMAGE_SIZE : SCOPE_IMAGE_SIZE) *
			vr.info.scale / 16.0f + 0.5f) * 16;

		vr.info.scope_size = size < SCOPE_IMAGE_SMALLEST ? SCOPE_IMAGE_SMALLEST :
			size > SCOPE_IMAGE_LARGEST ? SCOPE_IMAGE_LARGEST : size;
	}
	vr.info.standing = settings.standing;
	vr.info.refresh_rate_wanted = (float)config_real("vr.refresh_rate");
	vr.info.depth = config_boolean("vr.depth");
	/* (decided as the session is made: the eyes' targets are sized for it,
	d3d8_device.c) */
	vr.foveation = !strcmp(config_string("vr.foveation"), "off") ? 0 :
		!strcmp(config_string("vr.foveation"), "fixed") ? 1 : 2;
	vr.foveation_inset = (float)config_real("vr.foveation_inset");
	if (!(vr.foveation_inset >= FOVEA_INSET_SMALLEST && vr.foveation_inset <= FOVEA_INSET_LARGEST))
		vr.foveation_inset = FOVEA_INSET;
	vr.info.eye_gaze = vr.foveation == 2 && !settings.test_gaze;
	if (!host_vr_initialize(&vr.info))
	{
		platform_log("vr: OpenXR is not available; playing flat");
		vr.failed = 1;
		return 0;
	}
	vr.force_render = config_boolean("debug.vr_force_render");
	acquired_clear();
	vr_visor_initialize(&vr.info);
	vr.trigger_touch_sensed = -1;
	vr.grab_nearest = vr.grab_reach = vr.grab_taken = vr.grab_pressed = -1;
	vr.initialized = 1;
	vr.statistics = config_boolean("debug.gpu_stats");
	platform_log("vr: %s, eyes %dx%d, %.0f Hz", vr.info.system_name, vr.info.eye_width, vr.info.eye_height,
		vr.info.refresh_rate);
	if (vr.foveation == 2 && !vr.info.eye_gaze && !settings.test_gaze)
		vr.foveation = 1;
	return 1;
}

void halo_vr_eye_size(int *width, int *height)
{
	*width = vr.info.eye_width;
	*height = vr.info.eye_height;
}

void halo_vr_hud_size(int *width, int *height)
{
	*width = vr.info.hud_width;
	*height = vr.info.hud_height;
}

void halo_vr_panel_size(int panel, int *width, int *height)
{
	*width = panel >= 0 && panel < VR_PANEL_COUNT ? vr.info.panel_width[panel] : 0;
	*height = panel >= 0 && panel < VR_PANEL_COUNT ? vr.info.panel_height[panel] : 0;
}

int halo_vr_scope_size(void)
{
	return vr.info.scope_size;
}

static void acquired_clear(void)
{
	int swapchain;

	for (swapchain = 0; swapchain < VR_SWAPCHAIN_COUNT; swapchain++)
		vr.acquired[swapchain] = -1;
}

unsigned int halo_vr_image(int swapchain)
{
	if (!vr.frame || swapchain < 0 || swapchain >= VR_SWAPCHAIN_COUNT)
		return 0;
	/* (the host acquires the images, as it draws: port/linux/vulkan/
	host_vulkan_vr.c; here they are only said to be) */
	vr.acquired[swapchain] = 0;
	return 1;
}

static void recentre(void)
{
	const struct vr_host_pose *head = &vr.views.head;
	float yaw;

	if (!head->valid)
		return;
	yaw = quaternion_yaw(head->orientation);
	/* the aim keeps pointing where it did; the gamepad's is where the head
	looks now (halo_vr_aim) */
	if (settings.gamepad_aim)
		vr.aim_synced = 0;
	else if (vr.origin_valid)
		vr.body_yaw += yaw - vr.origin_yaw;
	vr.origin_yaw = yaw;
	/* (the body's heading is from the origin's) */
	vr.body_heading_valid = 0;
	vr.origin_position[0] = head->position[0];
	vr.origin_position[1] = settings.standing ? settings.player_height : head->position[1];
	vr.origin_position[2] = head->position[2];
	vr.origin_valid = 1;
	platform_log("vr: recentred (%s)", settings.standing ? "standing" : "seated");
}

static void normalize(float v[3])
{
	float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (length > 1e-6f)
	{
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}
}

/* ---------- the dominant hand */

/* A left-hander's controllers (vr.dominant_hand = "left"): the left one
holds the weapon. What the runtime says of each hand is swapped as it comes,
the poses as each frame locates them and the triggers, the grips and the
trigger's touch as they are read, so that all that follows has the weapon's
hand where it had the right: the aim, the trigger finger, the melee swing,
the holsters', the scope's zoom, and the other hand the foregrip's, the
magazines', the grenades'. What is on the body is mirrored where it is
placed (hand_left: the holsters, the pouches), the haptics go back to the
hand they are for (hand_haptic), and the first-person weapon and hands are
drawn mirrored (port/linux/game/vr_render.c). The buttons stay where they
are, as the game's prompts name them (A, B, X, Y, the bumpers, the d-pad,
View and Menu); the sticks swap as vr.swap_sticks says, the move's and the
turn's, each with its click (the crouch, the zoom) and the turn's pushed
down (the magazine's eject). debug.vr_test_hands mirrors its poses
(test_hands_apply). */
static void hands_assign_views(void)
{
	struct vr_host_pose pose;

	if (!vr.views_unassigned)
		return;
	vr.views_unassigned = 0;
	if (!settings.left_handed)
		return;
	pose = vr.views.aim[0];
	vr.views.aim[0] = vr.views.aim[1];
	vr.views.aim[1] = pose;
	pose = vr.views.grip[0];
	vr.views.grip[0] = vr.views.grip[1];
	vr.views.grip[1] = pose;
}

static void hands_assign_input(void)
{
	struct vr_host_input *input = &vr.input;
	unsigned int clicks = VR_BUTTON_LEFT_STICK | VR_BUTTON_RIGHT_STICK;
	float value, stick[2];
	int flag;

	if (settings.left_handed)
	{
		value = input->left_trigger;
		input->left_trigger = input->right_trigger;
		input->right_trigger = value;
		value = input->left_grip;
		input->left_grip = input->right_grip;
		input->right_grip = value;
		flag = input->left_trigger_touch;
		input->left_trigger_touch = input->right_trigger_touch;
		input->right_trigger_touch = flag;
		flag = input->active[0];
		input->active[0] = input->active[1];
		input->active[1] = flag;
	}
	if (settings.swap_sticks)
	{
		memcpy(stick, input->left_stick, sizeof(stick));
		memcpy(input->left_stick, input->right_stick, sizeof(stick));
		memcpy(input->right_stick, stick, sizeof(stick));
		if ((input->buttons & clicks) == VR_BUTTON_LEFT_STICK || (input->buttons & clicks) == VR_BUTTON_RIGHT_STICK)
			input->buttons ^= clicks;
	}
}

/* the controller of a hand as this file names them (0 the left, 1 the
right: the weapon's) */
static int hand_controller(int hand)
{
	return settings.left_handed ? !hand : hand;
}

static void hand_haptic(int hand, float amplitude, float seconds)
{
	host_vr_haptic(hand_controller(hand), amplitude, seconds);
}

/* a place on the body's left (metres; a right-hander's, as holster_places
and reload_pouches have them) where it is for the dominant hand */
static float hand_left(float left)
{
	return settings.left_handed ? -left : left;
}

/* a hand's grip is the climb's: holding a hold, or at one (halo_vr_climb) */
#define CLIMB_BUSY(hand) (vr.climb_grip[hand] || vr.climb_near[hand])

/* both hands on a long gun, once a frame: the left hand takes the foregrip
(its grip pressed near the line ahead of the right hand) and lets it go, and
the line between the hands is smoothed */
static void two_handed_update(float seconds)
{
	static const float backwards[3] = { 0.0f, 0.0f, -1.0f };
	const struct vr_host_pose *right = &vr.views.grip[1], *left = &vr.views.grip[0];
	int able = settings.two_handed && settings.controller_aim && vr.long_gun && vr.aimed &&
		right->valid && left->valid && vr.views.aim[1].valid && !vr.reload.held && !vr.reload_grip_taken &&
		!vr.flip_grip[0] && !CLIMB_BUSY(0);
	float line[3], pointing[3], along = 0.0f, across = 0.0f, length;
	int axis;

	if (able)
	{
		quaternion_rotate(vr.views.aim[1].orientation, backwards, pointing);
		for (axis = 0; axis < 3; axis++)
			line[axis] = left->position[axis] - right->position[axis];
		for (axis = 0; axis < 3; axis++)
			along += line[axis] * pointing[axis];
		for (axis = 0; axis < 3; axis++)
		{
			float off = line[axis] - pointing[axis] * along;

			across += off * off;
		}
		across = sqrtf(across);
		if (!vr.two_handed && vr.input.left_grip > FOREGRIP_PRESSED && along > FOREGRIP_NEAREST &&
			along < FOREGRIP_FARTHEST && across < FOREGRIP_RADIUS)
		{
			vr.two_handed = 1;
			if (vr.two_handed_weight <= 0.0f)
				memcpy(vr.two_handed_line, pointing, sizeof(vr.two_handed_line));
			hand_haptic(0, 0.25f, 0.03f);
		}
		else if (vr.two_handed && vr.input.left_grip < FOREGRIP_RELEASED)
		{
			vr.two_handed = 0;
		}
	}
	else
	{
		vr.two_handed = 0;
	}
	if (vr.two_handed)
	{
		/* (the hands too near each other point nowhere: the line stays) */
		length = sqrtf(line[0] * line[0] + line[1] * line[1] + line[2] * line[2]);
		if (length > FOREGRIP_NEAREST * 0.5f)
		{
			float t = seconds > 0.0f ? 1.0f - expf(-seconds / TWO_HANDED_SMOOTHING) : 1.0f;

			for (axis = 0; axis < 3; axis++)
				vr.two_handed_line[axis] += (line[axis] / length - vr.two_handed_line[axis]) * t;
			normalize(vr.two_handed_line);
		}
		vr.two_handed_weight += seconds / TWO_HANDED_BLEND;
		if (vr.two_handed_weight > 1.0f)
			vr.two_handed_weight = 1.0f;
	}
	else
	{
		vr.two_handed_weight -= seconds / TWO_HANDED_BLEND;
		if (vr.two_handed_weight < 0.0f || !able)
			vr.two_handed_weight = 0.0f;
	}
}

/* where the weapon in the right hand points, and its up (OpenXR axes): the
right controller's pointing pose, or with both hands on a long gun, the line
between them, the right hand's roll kept */
static void weapon_pointing(float forward[3], float up[3])
{
	static const float backwards[3] = { 0.0f, 0.0f, -1.0f };
	static const float upwards[3] = { 0.0f, 1.0f, 0.0f };
	const float *orientation = vr.views.aim[1].orientation;
	int axis;

	quaternion_rotate(orientation, backwards, forward);
	quaternion_rotate(orientation, upwards, up);
	if (vr.two_handed_weight > 0.0f)
	{
		float w = vr.two_handed_weight, along = 0.0f;

		/* (smoothstep: no jolt as the hand takes the foregrip) */
		w = w * w * (3.0f - 2.0f * w);
		for (axis = 0; axis < 3; axis++)
			forward[axis] += (vr.two_handed_line[axis] - forward[axis]) * w;
		normalize(forward);
		for (axis = 0; axis < 3; axis++)
			along += up[axis] * forward[axis];
		for (axis = 0; axis < 3; axis++)
			up[axis] -= forward[axis] * along;
		normalize(up);
	}
}

/* ---------- the grenade thrown with the left hand */

enum
{
	THROW_IDLE = 0,
	THROW_HELD,
	THROW_SWING,
	THROW_RELEASED,
};

static void throw_enter(int state)
{
	vr.throw_state = state;
	vr.throw_seconds = 0.0f;
}

/* the hand's velocity at a sample, over THROW_VELOCITY_SECONDS back from
it (or as long as the samples go back, if a frame or more); 0 if there is
none */
static int throw_velocity_at(unsigned int sample, float velocity[3])
{
	unsigned int newest = sample & (THROW_SAMPLES - 1), back;
	int axis;

	if (!vr.throw_places_valid[newest])
		return 0;
	for (back = 1; back < THROW_SAMPLES; back++)
	{
		unsigned int older = (sample - back) & (THROW_SAMPLES - 1);
		double span;

		if (!vr.throw_places_valid[older])
			break;
		span = vr.throw_times[newest] - vr.throw_times[older];
		if (span >= THROW_VELOCITY_SECONDS || back == THROW_SAMPLES - 1 ||
			!vr.throw_places_valid[(sample - back - 1) & (THROW_SAMPLES - 1)])
		{
			if (span <= 0.0)
				return 0;
			for (axis = 0; axis < 3; axis++)
				velocity[axis] = (float)((vr.throw_places[newest][axis] - vr.throw_places[older][axis]) / span);
			return 1;
		}
	}
	return 0;
}

static float length3(const float v[3])
{
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/* the throw as the hand lets go: along its fastest velocity of the last
THROW_RELEASE_LOOKBACK seconds (the hand slows as it opens), lifted, as hard
as that speed throws; where the hand is now */
static void throw_release(const char *why)
{
	struct halo_vr_throw *out = &vr.throw_out;
	unsigned int newest = vr.throw_sample & (THROW_SAMPLES - 1), back;
	float fastest[3] = { 0.0f, 0.0f, 0.0f }, speed = 0.0f, f[3], offset[3];
	float level, pitch, lift = THROW_LIFT_DEGREES * (float)M_PI / 180.0f;
	int axis;

	for (back = 0; back < THROW_SAMPLES; back++)
	{
		unsigned int sample = (vr.throw_sample - back) & (THROW_SAMPLES - 1);
		float *v = vr.throw_velocities[sample];

		if (!vr.throw_places_valid[sample] ||
			vr.throw_times[newest] - vr.throw_times[sample] > THROW_RELEASE_LOOKBACK)
		{
			break;
		}
		if (length3(v) > speed)
		{
			speed = length3(v);
			memcpy(fastest, v, sizeof(fastest));
		}
	}
	/* (no speed: along where the head looks, as the game throws, softly) */
	if (speed <= 0.0f)
	{
		static const float backwards[3] = { 0.0f, 0.0f, -1.0f };

		quaternion_rotate(vr.views.head.orientation, backwards, fastest);
	}
	game_axes(fastest, vr.origin_yaw, f);
	level = sqrtf(f[0] * f[0] + f[1] * f[1]);
	pitch = atan2f(f[2], level) + lift;
	if (pitch > (float)M_PI * 0.5f)
		pitch = (float)M_PI * 0.5f;
	out->yaw = vr.body_yaw + atan2f(f[1], f[0]);
	out->pitch = pitch;
	out->direction[0] = cosf(pitch) * (level > 0.0f ? f[0] / level : 1.0f);
	out->direction[1] = cosf(pitch) * (level > 0.0f ? f[1] / level : 0.0f);
	out->direction[2] = sinf(pitch);
	out->speed = speed;
	out->power = speed / THROW_FULL_SPEED;
	if (out->power > 1.0f)
		out->power = 1.0f;
	if (out->power < THROW_LEAST_POWER)
		out->power = THROW_LEAST_POWER;
	for (axis = 0; axis < 3; axis++)
		offset[axis] = vr.throw_places[newest][axis] - vr.origin_position[axis];
	game_axes(offset, vr.origin_yaw, out->position);
	out->body_yaw = vr.body_yaw;
	out->world_units_per_metre = settings.world_scale / METRES_PER_WORLD_UNIT;
	if (settings.throw_log)
	{
		platform_log("vr: throw: let go (%s) %.2f s into the swing at %.2f m/s: power %.2f, "
			"%.0f degrees from the head's heading, pitched %.0f up", why, vr.throw_seconds, speed, out->power,
			remainderf(out->yaw - vr.body_yaw - quaternion_yaw(vr.views.head.orientation) + vr.origin_yaw,
				2.0f * (float)M_PI) * 180.0f / (float)M_PI, pitch * 180.0f / (float)M_PI);
	}
	hand_haptic(0, 0.6f, 0.05f);
	throw_enter(THROW_RELEASED);
}

/* the left hand throwing a grenade, once a frame (vr.grenade_throw =
"gesture" or "both"; THROW_* above): after two_handed_update, which takes the left
grip first on a long gun's foregrip */
static void throw_update(float seconds)
{
	static const float backwards[3] = { 0.0f, 0.0f, -1.0f };
	const struct vr_host_pose *hand = &vr.views.grip[0];
	unsigned int sample;
	float velocity[3] = { 0.0f, 0.0f, 0.0f }, forward[3], ahead = 0.0f, speed_forward = 0.0f, length;
	int pressed = vr.input.left_grip > THROW_GRIP_PRESSED;
	int released = vr.input.left_grip < THROW_GRIP_RELEASED;
	int pressing = pressed && !vr.throw_grip_pressed;
	int able, axis;

	if (vr.throw_press_frames > 0)
		vr.throw_press_frames--;
	vr.throw_clock += seconds;
	vr.throw_seconds += seconds;
	if (pressed)
		vr.throw_grip_pressed = 1;
	else if (released)
		vr.throw_grip_pressed = 0;

	/* the hand's place and velocity, kept */
	sample = ++vr.throw_sample;
	vr.throw_places_valid[sample & (THROW_SAMPLES - 1)] = hand->valid && seconds > 0.0f;
	memcpy(vr.throw_places[sample & (THROW_SAMPLES - 1)], hand->position, sizeof(vr.throw_places[0]));
	vr.throw_times[sample & (THROW_SAMPLES - 1)] = vr.throw_clock;
	if (!throw_velocity_at(sample, velocity))
		velocity[0] = velocity[1] = velocity[2] = 0.0f;
	memcpy(vr.throw_velocities[sample & (THROW_SAMPLES - 1)], velocity, sizeof(velocity));

	/* forward: where the head faces, level; the hand's way ahead of the
	eyes and its speed along it */
	quaternion_rotate(vr.views.head.orientation, backwards, forward);
	forward[1] = 0.0f;
	length = sqrtf(forward[0] * forward[0] + forward[2] * forward[2]);
	if (length > 1e-3f)
	{
		forward[0] /= length;
		forward[2] /= length;
		for (axis = 0; axis < 3; axis++)
		{
			ahead += (hand->position[axis] - vr.views.head.position[axis]) * forward[axis];
			speed_forward += velocity[axis] * forward[axis];
		}
	}

	able = settings.throw_gesture && settings.controller_aim && vr.aimed && hand->valid &&
		vr.views.head.valid && !vr.two_handed && vr.input.right_grip <= 0.8f && !vr.reload.held &&
		!vr.reload_grip_taken && !vr.flip_grip[0] && !CLIMB_BUSY(0);
	switch (vr.throw_state)
	{
	case THROW_IDLE:
		/* (the grip pulled now, not already held: off a foregrip, or
		while the game could not throw) */
		if (able && pressing && vr.throw_ready)
		{
			throw_enter(THROW_HELD);
			vr.throw_nearest = ahead;
			hand_haptic(0, 0.15f, 0.02f);
			if (settings.throw_log)
				platform_log("vr: throw: held, %.2f m ahead of the eyes", ahead);
		}
		else if (pressing && settings.throw_log && settings.throw_gesture)
		{
			platform_log("vr: throw: the grip taken, holding nothing (%s)", vr.flip_grip[0] ? "lifting a vehicle" :
				vr.two_handed ? "on the foregrip" :
				vr.reload.held ? "a magazine taken" : vr.reload_grip_taken ? "the grip at a pouch" :
				vr.input.right_grip > 0.8f ? "both grips: recentring" : !settings.controller_aim ?
				"vr.aim is not the controller" : !vr.aimed ? "the aim is the game's" : !hand->valid ?
				"the hand is lost" : !vr.throw_ready ? "the game cannot throw" : "the head is lost");
		}
		break;
	case THROW_HELD:
		if (ahead < vr.throw_nearest)
			vr.throw_nearest = ahead;
		if (!able || !vr.throw_ready)
		{
			if (settings.throw_log)
				platform_log("vr: throw: dropped (%s)", vr.two_handed ? "on the foregrip" :
					vr.input.right_grip > 0.8f ? "both grips: recentring" : !vr.throw_ready ?
					"the game cannot throw" : "the hand or the aim is lost");
			throw_enter(THROW_IDLE);
		}
		else if (released)
		{
			if (settings.throw_log)
				platform_log("vr: throw: let go after %.2f s without a swing (%.2f m/s forward)",
					vr.throw_seconds, speed_forward);
			throw_enter(THROW_IDLE);
		}
		else if (vr.throw_seconds >= THROW_HOLD_SECONDS && speed_forward > THROW_SWING_SPEED &&
			vr.throw_nearest <= THROW_WINDUP_REACH)
		{
			if (settings.throw_log)
				platform_log("vr: throw: swing at %.2f m/s forward, %.2f m ahead of the eyes (drawn back to "
					"%.2f), held %.2f s", speed_forward, ahead, vr.throw_nearest, vr.throw_seconds);
			throw_enter(THROW_SWING);
			vr.throw_press_frames = THROW_PRESS_FRAMES;
		}
		else if (settings.throw_log && speed_forward > THROW_SWING_SPEED && !vr.throw_fast)
		{
			platform_log("vr: throw: no swing at %.2f m/s forward: held %.2f s (of %.2f), drawn back to %.2f m "
				"ahead of the eyes (of %.2f)", speed_forward, vr.throw_seconds, THROW_HOLD_SECONDS,
				vr.throw_nearest, THROW_WINDUP_REACH);
		}
		break;
	case THROW_SWING:
		/* the game's throw has begun: the grenade is let go of, or thrown
		anyway */
		if (released)
			throw_release("the grip");
		else if (vr.throw_seconds >= THROW_SWING_SECONDS)
			throw_release("never let go");
		else if (!hand->valid)
			throw_release("the hand lost");
		break;
	case THROW_RELEASED:
		if (vr.throw_seconds >= THROW_RELEASED_SECONDS)
		{
			if (settings.throw_log)
				platform_log("vr: throw: the game threw nothing");
			throw_enter(THROW_IDLE);
		}
		break;
	}
	/* (the game's facing during a swing: along the hand's velocity) */
	if (vr.throw_state == THROW_SWING)
	{
		float f[3];

		game_axes(velocity, vr.origin_yaw, f);
		if (length3(f) > 0.5f)
		{
			vr.throw_out.yaw = vr.body_yaw + atan2f(f[1], f[0]);
			vr.throw_out.pitch = atan2f(f[2], sqrtf(f[0] * f[0] + f[1] * f[1])) +
				THROW_LIFT_DEGREES * (float)M_PI / 180.0f;
		}
	}
	vr.throw_fast = speed_forward > THROW_SWING_SPEED;
}

int halo_vr_throw(struct halo_vr_throw *throw_state)
{
	int phase = vr.throw_state == THROW_SWING ? HALO_VR_THROW_SWING :
		vr.throw_state == THROW_RELEASED ? HALO_VR_THROW_RELEASED : HALO_VR_THROW_NONE;

	if (!vr.initialized)
		phase = HALO_VR_THROW_NONE;
	*throw_state = vr.throw_out;
	throw_state->phase = phase;
	throw_state->held = vr.initialized && vr.throw_state != THROW_IDLE;
	throw_state->world_units_per_metre = settings.world_scale / METRES_PER_WORLD_UNIT;
	throw_state->hands = settings.floating_hands && settings.controller_aim;
	throw_state->log = settings.throw_log;
	return phase;
}

void halo_vr_throw_ready(int ready)
{
	vr.throw_ready = ready;
}

void halo_vr_throw_done(void)
{
	if (vr.throw_state == THROW_SWING || vr.throw_state == THROW_RELEASED)
	{
		if (settings.throw_log && vr.throw_state == THROW_SWING)
			platform_log("vr: throw: the game's throw ended before the hand let go");
		throw_enter(THROW_IDLE);
	}
}

/* the body's heading, once a frame (BODY_TURN_* above): the one heading
of all that is on the body, the holsters, the body drawn under the head
(first_person_legs.c) and the arms' shoulders (vr_hands.c) */
static void body_update(float seconds)
{
	static const float backwards[3] = { 0.0f, 0.0f, -1.0f };
	float v[3], f[3], difference;

	if (!vr.origin_valid || !vr.views.head.valid)
	{
		vr.body_heading_valid = 0;
		return;
	}
	quaternion_rotate(vr.views.head.orientation, backwards, v);
	game_axes(v, vr.origin_yaw, f);
	/* (straight up or down the head has no heading: the body keeps its own) */
	if (f[0] * f[0] + f[1] * f[1] <= 1e-4f)
		return;
	if (!vr.body_heading_valid)
	{
		vr.body_heading = atan2f(f[1], f[0]);
		vr.body_heading_valid = 1;
	}
	difference = remainderf(atan2f(f[1], f[0]) - vr.body_heading, 2.0f * (float)M_PI);
	if (difference > BODY_TURN_FREE)
	{
		vr.body_heading += difference - BODY_TURN_FREE;
		difference = BODY_TURN_FREE;
	}
	else if (difference < -BODY_TURN_FREE)
	{
		vr.body_heading += difference + BODY_TURN_FREE;
		difference = -BODY_TURN_FREE;
	}
	if (seconds > 0.0f)
		vr.body_heading += difference * (1.0f - expf(-seconds / BODY_TURN_SECONDS));
	vr.body_heading = remainderf(vr.body_heading, 2.0f * (float)M_PI);
}

/* where the body hangs from the head (head: its place, forward and up, in
game axes from the recentred head): the top of the neck, which stays put as
the head nods and turns, and the eyes as they would be from it with the
head level and turned to the body's heading, which the holsters and the
arms' shoulders are placed from */
static void body_place(const float head[3], const float forward[3], const float up[3], float neck[3], float eyes[3])
{
	int axis;

	for (axis = 0; axis < 3; axis++)
		neck[axis] = head[axis] - forward[axis] * NECK_BACK - up[axis] * NECK_DOWN;
	eyes[0] = neck[0] + cosf(vr.body_heading) * NECK_BACK;
	eyes[1] = neck[1] + sinf(vr.body_heading) * NECK_BACK;
	eyes[2] = neck[2] + NECK_DOWN;
}

/* the eyes as body_place has them this frame, from the head as it is now
(game axes from the recentred head): what is on the body is placed from
them */
static void body_eyes(float eyes[3])
{
	static const float backwards[3] = { 0.0f, 0.0f, -1.0f }, upwards[3] = { 0.0f, 1.0f, 0.0f };
	float v[3], head[3], forward[3], up[3], neck[3];
	int axis;

	for (axis = 0; axis < 3; axis++)
		v[axis] = vr.views.head.position[axis] - vr.origin_position[axis];
	game_axes(v, vr.origin_yaw, head);
	quaternion_rotate(vr.views.head.orientation, backwards, v);
	game_axes(v, vr.origin_yaw, forward);
	quaternion_rotate(vr.views.head.orientation, upwards, v);
	game_axes(v, vr.origin_yaw, up);
	body_place(head, forward, up, neck, eyes);
}

/* the right hand aiming its weapon, not reaching for the shoulder's holster:
the left hand on its foregrip, the trigger pulled at all, or the index
finger on it where the trigger senses a finger (a reach is made with the
finger off it) */
static int holster_near_aiming(void)
{
	return vr.two_handed || vr.input.right_trigger > 0.05f ||
		(vr.trigger_touch_sensed == 1 && vr.input.right_trigger_touch > 0);
}

/* the holsters, once a frame (vr.holsters; HOLSTER_* above): where they
are on the body, the right hand reaching into one (a tick of its
controller), and its grip there drawing the stowed weapon */
static void holsters_update(float seconds)
{
	struct halo_vr_holsters *h = &vr.holsters;
	const struct vr_host_pose *hand = &vr.views.grip[1];
	float v[3], eyes[3], place[3], c, s;
	/* (not a grip the lift of a vehicle has) */
	int pressed = vr.input.right_grip > HOLSTER_GRIP_PRESSED && !vr.flip_grip[1] && !CLIMB_BUSY(1);
	int pressing = pressed && !vr.holster_grip_pressed;
	int reach = HALO_VR_HOLSTER_NONE, holster, axis;

	if (vr.holster_press_frames > 0)
		vr.holster_press_frames--;
	if (pressed)
		vr.holster_grip_pressed = 1;
	else if (vr.input.right_grip < HOLSTER_GRIP_RELEASED)
		vr.holster_grip_pressed = 0;
	if (!vr.holster_grip_pressed)
		vr.holster_gripped = 0;
	h->enabled = settings.holsters && settings.controller_aim && vr.body_heading_valid;
	if (!h->enabled)
	{
		h->reach = HALO_VR_HOLSTER_NONE;
		for (holster = 0; holster < HALO_VR_HOLSTERS; holster++)
			h->distance[holster] = 1000.0f;
		return;
	}
	/* the holsters about the head (as it would be level and turned with
	the body: they stay put as it looks down at them), along the body */
	h->body_yaw = vr.body_heading;
	body_eyes(eyes);
	c = cosf(h->body_yaw);
	s = sinf(h->body_yaw);
	for (holster = HALO_VR_HOLSTER_BACK; holster < HALO_VR_HOLSTERS; holster++)
	{
		const float *p = holster_places[holster];
		float left = hand_left(p[1]);

		h->place[holster][0] = eyes[0] + p[0] * c - left * s;
		h->place[holster][1] = eyes[1] + p[0] * s + left * c;
		h->place[holster][2] = eyes[2] + p[2];
	}
	/* the right hand in one */
	for (holster = 0; holster < HALO_VR_HOLSTERS; holster++)
		h->distance[holster] = 1000.0f;
	if (hand->valid && vr.aimed)
	{
		for (axis = 0; axis < 3; axis++)
			v[axis] = hand->position[axis] - vr.origin_position[axis];
		game_axes(v, vr.origin_yaw, place);
		for (holster = HALO_VR_HOLSTER_BACK; holster < HALO_VR_HOLSTERS; holster++)
		{
			for (axis = 0; axis < 3; axis++)
				v[axis] = place[axis] - h->place[holster][axis];
			h->distance[holster] = length3(v) - holster_places[holster][3];
			if (h->distance[holster] < 0.0f)
			{
				h->distance[holster] = 0.0f;
				reach = holster;
			}
		}
	}
	/* (a tick as the hand comes to the stowed weapon; before, ticks
	quickening as it nears the shoulder's, which it cannot see) */
	if (reach != h->reach && reach != HALO_VR_HOLSTER_NONE && reach == vr.holster_stowed)
		hand_haptic(1, 0.2f, 0.02f);
	if (reach == HALO_VR_HOLSTER_NONE && vr.holster_stowed == HALO_VR_HOLSTER_BACK &&
		h->distance[HALO_VR_HOLSTER_BACK] < HOLSTER_NEAR_DISTANCE && !holster_near_aiming())
	{
		float near = 1.0f - h->distance[HALO_VR_HOLSTER_BACK] / HOLSTER_NEAR_DISTANCE;

		vr.holster_near_seconds += seconds;
		if (vr.holster_near_seconds >= HOLSTER_NEAR_SLOWEST + (HOLSTER_NEAR_FASTEST - HOLSTER_NEAR_SLOWEST) * near)
		{
			vr.holster_near_seconds = 0.0f;
			hand_haptic(1, holster_near_amplitude[0] + (holster_near_amplitude[1] - holster_near_amplitude[0]) * near,
				HOLSTER_NEAR_TICK_SECONDS);
		}
	}
	else
	{
		/* (the first tick as soon as the hand comes near) */
		vr.holster_near_seconds = HOLSTER_NEAR_SLOWEST;
	}
	h->reach = reach;
	if (reach == HALO_VR_HOLSTER_NONE && !vr.melee_swinging)
		vr.holster_drawn = 0;
	/* (a grip squeezed as a swing passes through: no draw, nor the
	holster's) */
	if (pressing && reach != HALO_VR_HOLSTER_NONE && (vr.hand_speed >= HOLSTER_STILL_SPEED ||
		(vr.melee_swinging && vr.melee_fired)))
	{
		platform_log("vr: holster: the grip in the %s holster at %.1f m/s: a swing passing, not a draw",
			reach == HALO_VR_HOLSTER_BACK ? "shoulder's" : "hip's", vr.hand_speed);
	}
	else if (pressing && reach != HALO_VR_HOLSTER_NONE)
	{
		vr.holster_gripped = 1;
		if (reach == vr.holster_stowed)
		{
			vr.holster_press_frames = HOLSTER_PRESS_FRAMES;
			vr.holster_ticks = GetTickCount();
			vr.holster_drawn = 1;
			hand_haptic(1, 0.5f, 0.05f);
		}
		platform_log("vr: holster: the grip in the %s holster: %s", reach == HALO_VR_HOLSTER_BACK ? "shoulder's" : "hip's",
			reach == vr.holster_stowed ? "drawing its weapon" : vr.holster_stowed == HALO_VR_HOLSTER_NONE ?
			"no weapon to draw" : "the other weapon is in the other one");
	}
}

/* how far a point is from the line between two others (metres) */
static float segment_distance(const float point[3], const float a[3], const float b[3])
{
	float ab[3], ap[3], along = 0.0f, length;
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		ab[axis] = b[axis] - a[axis];
		ap[axis] = point[axis] - a[axis];
	}
	length = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
	if (length > 0.0f)
	{
		along = (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / length;
		along = along < 0.0f ? 0.0f : along > 1.0f ? 1.0f : along;
	}
	for (axis = 0; axis < 3; axis++)
		ap[axis] -= ab[axis] * along;
	return length3(ap);
}

/* where a hand's fingers press (game axes from the recentred head), if the
hand may press */
static int press_fingers(int hand, float place[3])
{
	static const float along[3] = { PRESS_FINGERS, 0.0f, 0.0f };
	const struct vr_host_pose *grip = &vr.views.grip[hand], *aim = &vr.views.aim[hand];
	float point[3], v[3];
	int axis;

	if (!grip->valid || !aim->valid)
		return 0;
	/* (the left hand holding the foregrip, a grenade, a magazine) */
	if (!hand && (vr.two_handed || vr.throw_state != THROW_IDLE || vr.reload.held))
		return 0;
	pose_point(grip->position, aim->orientation, NULL, NULL, along, point);
	for (axis = 0; axis < 3; axis++)
		v[axis] = point[axis] - vr.origin_position[axis];
	game_axes(v, vr.origin_yaw, place);
	return 1;
}

/* how far the fingers are out of a control (metres; 0 or less: pressing
it), its face's middle and way given */
static float press_distance(const float fingers[3], const struct halo_vr_grab_thing *thing)
{
	float v[3], depth, across;
	int axis;

	for (axis = 0; axis < 3; axis++)
		v[axis] = fingers[axis] - thing->ends[0][axis];
	if (length3(thing->front) < 0.5f)
		return length3(v) - thing->radius;
	depth = v[0] * thing->front[0] + v[1] * thing->front[1] + v[2] * thing->front[2];
	for (axis = 0; axis < 3; axis++)
		v[axis] -= thing->front[axis] * depth;
	across = length3(v) - thing->radius;
	/* (before the face: as far as it is beyond PRESS_BEHIND; into it: as
	far as it is past PRESS_DEPTH) */
	depth = depth > PRESS_DEPTH ? depth - PRESS_DEPTH : depth < -PRESS_BEHIND ? -PRESS_BEHIND - depth : 0.0f;
	return across > depth ? across : depth;
}

/* the controls pressed, once a frame (vr.grab_world; PRESS_* above): either
hand's fingers pushed into one press it (the game presses its action:
vr_grab.c) */
static void press_update(int able)
{
	static const char *const hands[2] = { "left", "right" };
	int hand, index;

	for (hand = 0; hand < 2; hand++)
	{
		float fingers[3], nearest = 1000.0f;
		const struct halo_vr_grab_thing *control = NULL;

		if (!able || !press_fingers(hand, fingers))
		{
			vr.press_in[hand] = 0;
			continue;
		}
		for (index = 0; index < vr.grab_told.count; index++)
		{
			const struct halo_vr_grab_thing *thing = &vr.grab_told.things[index];
			float distance;

			if (thing->kind != HALO_VR_GRAB_CONTROL)
				continue;
			distance = press_distance(fingers, thing);
			if (distance < nearest)
			{
				nearest = distance;
				control = thing;
			}
		}
		if (vr.press_in[hand] && nearest > PRESS_REARM)
			vr.press_in[hand] = 0;
		if (!vr.press_in[hand] && control && nearest <= 0.0f)
		{
			vr.press_in[hand] = 1;
			vr.grab_pressed = control->object;
			vr.press_ticks = GetTickCount();
			haptic_play(hand, HAPTIC_FEEL(press_feel));
			platform_log("vr: grab: the %s hand presses the control %lx (the fingers at %.2f %.2f %.2f, its face %.2f "
				"%.2f %.2f)", hands[hand], (unsigned long)control->object, fingers[0], fingers[1], fingers[2],
				control->ends[0][0], control->ends[0][1], control->ends[0][2]);
		}
	}
}

/* the grab, once a frame (vr.grab_world; GRAB_* above), after
holsters_update, whose holsters keep the grip squeezed in them: the weapon
on offer that the right hand reaches (a tick of its controller as it gets
there), and its grip there taking it (the game presses its pick-up:
vr_grab.c) */
static void grab_update(void)
{
	const struct vr_host_pose *hand = &vr.views.grip[1];
	/* (not a grip the lift of a vehicle has) */
	int pressed = vr.input.right_grip > GRAB_GRIP_PRESSED && !vr.flip_grip[1] && !CLIMB_BUSY(1);
	int pressing = pressed && !vr.grab_grip_pressed;
	int able = settings.grab_world && settings.controller_aim && vr.aimed && vr.crouch_able_frame && !vr.menus &&
		vr.origin_valid && vr.presents - vr.grab_told_at <= GRAB_TOLD_FRAMES;
	long reach = -1;
	int index, axis;

	if (pressed)
		vr.grab_grip_pressed = 1;
	else if (vr.input.right_grip < GRAB_GRIP_RELEASED)
		vr.grab_grip_pressed = 0;
	if (!vr.grab_grip_pressed)
		vr.grab_gripped = 0;
	vr.grab_nearest = -1;
	vr.grab_distance = 1000.0f;
	if (able && hand->valid)
	{
		float v[3], place[3];

		for (axis = 0; axis < 3; axis++)
			v[axis] = hand->position[axis] - vr.origin_position[axis];
		game_axes(v, vr.origin_yaw, place);
		for (index = 0; index < vr.grab_told.count; index++)
		{
			const struct halo_vr_grab_thing *thing = &vr.grab_told.things[index];
			float distance;

			if (thing->kind != HALO_VR_GRAB_WEAPON)
				continue;
			distance = segment_distance(place, thing->ends[0], thing->ends[1]) - thing->radius - GRAB_REACH;
			if (distance < 0.0f)
				distance = 0.0f;
			if (distance < vr.grab_distance)
			{
				vr.grab_nearest = thing->object;
				vr.grab_distance = distance;
			}
		}
		if (vr.grab_nearest != -1 && vr.grab_distance <= 0.0f)
			reach = vr.grab_nearest;
	}
	/* (a tick as the hand gets to a weapon) */
	if (reach != -1 && reach != vr.grab_reach)
		hand_haptic(1, 0.2f, 0.02f);
	vr.grab_reach = reach;
	press_update(able);
	if (!pressing || reach == -1)
		return;
	if (vr.holsters.reach != HALO_VR_HOLSTER_NONE)
	{
		platform_log("vr: grab: the grip at a weapon, in a holster too: the holster's");
	}
	else if (vr.hand_speed >= GRAB_STILL_SPEED || (vr.melee_swinging && vr.melee_fired))
	{
		platform_log("vr: grab: the grip at a weapon at %.1f m/s: a swing passing, not a grab", vr.hand_speed);
	}
	else
	{
		vr.grab_gripped = 1;
		vr.grab_taken = reach;
		vr.grab_ticks = GetTickCount();
		hand_haptic(1, 0.5f, 0.05f);
		platform_log("vr: grab: the grip takes the weapon %lx", (unsigned long)reach);
	}
}

/* player_control.c, as the first player's input is read, each frame: the
things to grab told, the right hand's reach given, and the weapon its grip
took and the control a hand pressed (once each) */
int halo_vr_grab(struct halo_vr_grab *grab)
{
	grab->enabled = 0;
	grab->nearest = -1;
	grab->distance = 1000.0f;
	grab->taken = -1;
	grab->pressed = -1;
	if (!vr.initialized)
		return 0;
	settings_read();
	grab->world_units_per_metre = settings.world_scale / METRES_PER_WORLD_UNIT;
	if (grab->count > HALO_VR_GRAB_THINGS)
		grab->count = HALO_VR_GRAB_THINGS;
	vr.grab_told.count = grab->count;
	memcpy(vr.grab_told.things, grab->things, sizeof(*grab->things) * (size_t)grab->count);
	vr.grab_told_at = vr.presents;
	grab->enabled = settings.grab_world && settings.controller_aim && vr.aimed && vr.crouch_able_frame && !vr.menus;
	if (!grab->enabled)
	{
		vr.grab_taken = vr.grab_pressed = -1;
		return 1;
	}
	grab->nearest = vr.grab_nearest;
	grab->distance = vr.grab_distance;
	grab->taken = vr.grab_taken;
	grab->pressed = vr.grab_pressed;
	vr.grab_taken = vr.grab_pressed = -1;
	return 1;
}

void halo_vr_test_face(float yaw)
{
	static const float backwards[3] = { 0.0f, 0.0f, -1.0f };
	float v[3], f[3];

	if (!vr.initialized || !vr.views.head.valid)
		return;
	quaternion_rotate(vr.views.head.orientation, backwards, v);
	game_axes(v, vr.origin_yaw, f);
	vr.body_yaw = yaw - atan2f(f[1], f[0]);
}

/* the physical crouch, once a frame (vr.physical_crouch; CROUCH_* above):
the head low enough below the recentred eyes crouches, and back up near
them stands; only while the game could crouch the player (on foot, the VR
mode aiming, not in the menus) */
static void crouch_update(void)
{
	float drop = settings.standing ? CROUCH_STANDING_DROP : CROUCH_SEATED_DROP;
	float below = vr.origin_position[1] - vr.views.head.position[1];
	int able = settings.physical_crouch && vr.crouch_able_frame && !vr.menus && vr.origin_valid &&
		vr.views.head.valid;
	int crouching;

	if (!vr.origin_valid || !vr.views.head.valid)
		vr.head_low = 0;
	else if (!vr.head_low && below > drop)
		vr.head_low = 1;
	else if (vr.head_low && below < drop - CROUCH_HYSTERESIS)
		vr.head_low = 0;
	crouching = able && vr.head_low;
	if (crouching != vr.crouching)
	{
		platform_log("vr: crouch: %s (the head %.2f m below the recentred eyes)%s", crouching ? "on" : "off", below,
			crouching || !vr.head_low ? "" : !settings.physical_crouch ? ", vr.physical_crouch is off" :
			vr.menus ? ", the menus" : !vr.crouch_able_frame ? ", not on foot" : "");
		vr.crouching = crouching;
	}
}

void halo_vr_crouch_able(int able)
{
	vr.crouch_able = able;
}

int halo_vr_crouch(void)
{
	if (!vr.initialized)
		return 0;
	/* (the head low counts whether or not the game's crouch is pressed for
	it: just out of a vehicle the player is still crouched from before it,
	the head down, a frame before the game says the player is on foot) */
	return (settings.physical_crouch && vr.head_low ? HALO_VR_CROUCH_PHYSICAL : 0) |
		(vr.crouch_button ? HALO_VR_CROUCH_BUTTON : 0);
}

void halo_vr_holster_stowed(int holster)
{
	vr.holster_stowed = holster;
}

int halo_vr_holsters(struct halo_vr_holsters *holsters)
{
	*holsters = vr.holsters;
	holsters->enabled = vr.initialized && vr.holsters.enabled;
	return holsters->enabled;
}

/* ---------- the scopes */

int halo_vr_scopes(void)
{
	settings_read();
	return vr.initialized && settings.controller_aim && vr.info.scope_size > 0 ? settings.scopes : 0;
}

void halo_vr_smartlink_view(float *degrees, float *distance)
{
	settings_read();
	*degrees = settings.smartlink_degrees;
	*distance = settings.hud_distance;
}

int halo_vr_smartlink_hud(float *hud_degrees)
{
	settings_read();
	*hud_degrees = settings.hud_size;
	return settings.smartlink_visor;
}

void halo_vr_scope(const struct halo_vr_scope *scope)
{
	vr.scope_set = scope != NULL;
	if (scope)
		vr.scope = *scope;
}

float halo_vr_scope_zoom(float *height)
{
	*height = vr.scope.zoom_height;
	return vr.scope_set ? vr.scope.zoom : 0.0f;
}

int halo_vr_scope_smartlink(float reticle[2])
{
	reticle[0] = vr.scope.reticle[0];
	reticle[1] = vr.scope.reticle[1];
	return vr.scope_set && vr.scope.eye < 0;
}

/* the window from the recentred head's game axes to the play space (as the
scope's quad is placed, halo_vr_present), then into each eye's space as
the eyes were drawn: the window is drawn into their images, where each
sees it */
int halo_vr_scope_window(struct halo_vr_scope_window *window)
{
	float position[3], back[3], up[3], right[3], g[3], along;
	int axis, eye;

	if (!vr.scope_set || vr.scope.eye >= 0 || vr.scope.window[0] <= 0.0f || !vr.views.eye[0].valid ||
		!vr.views.eye[1].valid)
	{
		return 0;
	}
	play_axes(vr.scope.position, vr.origin_yaw, position);
	for (axis = 0; axis < 3; axis++)
	{
		position[axis] += vr.origin_position[axis];
		g[axis] = -vr.scope.forward[axis];
	}
	play_axes(g, vr.origin_yaw, back);
	play_axes(vr.scope.up, vr.origin_yaw, up);
	normalize(back);
	along = up[0] * back[0] + up[1] * back[1] + up[2] * back[2];
	for (axis = 0; axis < 3; axis++)
		up[axis] -= back[axis] * along;
	normalize(up);
	right[0] = up[1] * back[2] - up[2] * back[1];
	right[1] = up[2] * back[0] - up[0] * back[2];
	right[2] = up[0] * back[1] - up[1] * back[0];
	for (eye = 0; eye < 2; eye++)
	{
		const struct vr_host_pose *pose = &vr.views.eye[eye];
		float inverse[4], from_eye[3];

		inverse[0] = -pose->orientation[0];
		inverse[1] = -pose->orientation[1];
		inverse[2] = -pose->orientation[2];
		inverse[3] = pose->orientation[3];
		for (axis = 0; axis < 3; axis++)
			from_eye[axis] = position[axis] - pose->position[axis];
		quaternion_rotate(inverse, from_eye, window->centre[eye]);
		quaternion_rotate(inverse, right, window->right[eye]);
		quaternion_rotate(inverse, up, window->up[eye]);
		memcpy(window->fov[eye], vr.views.fov[eye], sizeof(window->fov[eye]));
	}
	window->half[0] = vr.scope.size * 0.5f;
	window->half[1] = vr.scope.size * 0.5f * vr.scope.height;
	window->window[0] = vr.scope.window[0];
	window->window[1] = vr.scope.window[1] / vr.scope.height;
	window->near_z = vr.depth_near;
	window->far_z = vr.depth_far;
	return 1;
}

/* a feed cockpit's display this frame (port/linux/game/vr_cabin.c), none
until it is told again */
static int cockpit_set;
static struct halo_vr_cockpit_display cockpit;

void halo_vr_cockpit_display(const struct halo_vr_cockpit_display *display)
{
	cockpit_set = display != NULL;
	if (display)
		cockpit = *display;
}

/* the display from the recentred head's game axes to the play space, then
into each eye's space as the eyes were drawn (as the smart-link's window) */
int halo_vr_cockpit_window(struct halo_vr_cockpit_window *window, struct halo_vr_cockpit_display *display)
{
	float position[3], forward[3], up[3], right[3], along;
	int axis, eye;

	if (!vr.initialized || !cockpit_set || !vr.views.eye[0].valid || !vr.views.eye[1].valid)
		return 0;
	*display = cockpit;
	play_axes(cockpit.position, vr.origin_yaw, position);
	for (axis = 0; axis < 3; axis++)
		position[axis] += vr.origin_position[axis];
	play_axes(cockpit.forward, vr.origin_yaw, forward);
	play_axes(cockpit.up, vr.origin_yaw, up);
	normalize(up);
	along = up[0] * forward[0] + up[1] * forward[1] + up[2] * forward[2];
	for (axis = 0; axis < 3; axis++)
		forward[axis] -= up[axis] * along;
	normalize(forward);
	/* (right: forward x up, OpenXR's axes being right-handed) */
	right[0] = forward[1] * up[2] - forward[2] * up[1];
	right[1] = forward[2] * up[0] - forward[0] * up[2];
	right[2] = forward[0] * up[1] - forward[1] * up[0];
	for (eye = 0; eye < 2; eye++)
	{
		const struct vr_host_pose *pose = &vr.views.eye[eye];
		float inverse[4], from_eye[3];

		inverse[0] = -pose->orientation[0];
		inverse[1] = -pose->orientation[1];
		inverse[2] = -pose->orientation[2];
		inverse[3] = pose->orientation[3];
		for (axis = 0; axis < 3; axis++)
			from_eye[axis] = position[axis] - pose->position[axis];
		quaternion_rotate(inverse, from_eye, window->centre[eye]);
		quaternion_rotate(inverse, up, window->up[eye]);
		quaternion_rotate(inverse, forward, window->forward[eye]);
		quaternion_rotate(inverse, right, window->right[eye]);
		memcpy(window->fov[eye], vr.views.fov[eye], sizeof(window->fov[eye]));
	}
	window->near_z = vr.depth_near;
	window->far_z = vr.depth_far;
	return 1;
}

/* climbing a vehicle by its holds (vr.vehicle_climb: port/linux/game/
vr_climb.c), once a frame, told the holds near (lines: game axes from the
recentred head, metres, and their reach): a grip taken with the hand within
a hold's reach holds it (a tick in the hand; a light one as a hand comes to
a hold), until let go; while a hand holds, the eye moves against its
motion (both: their mean), as a climber's body moves under the hands that
pull it: moved. CLIMB_GRIP_* the grip's pull taken and let go. */
#define CLIMB_GRIP_PRESSED 0.6f
#define CLIMB_GRIP_RELEASED 0.35f
#define CLIMB_NEAR 0.05f
static struct
{
	int pressed[2];
	int hold[2];
	float last[2][3];
	int near[2];
} climb;

static float climb_distance(const float p[3], const float ends[2][3], float radius)
{
	float d[3], e[3], t, len2 = 0.0f, dist2 = 0.0f;
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		e[axis] = ends[1][axis] - ends[0][axis];
		len2 += e[axis] * e[axis];
	}
	t = 0.0f;
	if (len2 > 1e-9f)
		t = ((p[0] - ends[0][0]) * e[0] + (p[1] - ends[0][1]) * e[1] + (p[2] - ends[0][2]) * e[2]) / len2;
	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	for (axis = 0; axis < 3; axis++)
	{
		d[axis] = p[axis] - (ends[0][axis] + e[axis] * t);
		dist2 += d[axis] * d[axis];
	}
	return sqrtf(dist2) - radius;
}

static int seat_held(void);

int halo_vr_climb(struct halo_vr_climb *c)
{
	static const char *const hands[2] = { "left", "right" };
	int hand, holding = 0, axis;

	c->moved[0] = c->moved[1] = c->moved[2] = 0.0f;
	c->seat_held = seat_held();
	c->frame = vr.presents;
	for (hand = 0; hand < 2; hand++)
	{
		const struct vr_host_pose *grip = &vr.views.grip[hand];
		float value = hand ? vr.input.right_grip : vr.input.left_grip;
		float place[3], v[3], best = 1e9f;
		int index, nearest = -1;

		c->held[hand] = -1;
		c->nearest[hand] = -1;
		c->distance[hand] = 1e9f;
		if (vr.initialized && grip->valid && vr.origin_valid)
		{
			for (axis = 0; axis < 3; axis++)
				v[axis] = grip->position[axis] - vr.origin_position[axis];
			game_axes(v, vr.origin_yaw, place);
			memcpy(c->hand[hand], place, sizeof(place));
		}
		if (!vr.initialized || !grip->valid || !vr.origin_valid || c->count <= 0 || vr.menus)
		{
			vr.climb_grip[hand] = vr.climb_near[hand] = 0;
			climb.near[hand] = 0;
			continue;
		}
		for (index = 0; index < c->count && index < HALO_VR_CLIMB_HOLDS; index++)
		{
			float d = climb_distance(place, (const float (*)[3])c->ends[index], c->radius[index]);

			if (d < best)
			{
				best = d;
				nearest = index;
			}
		}
		c->nearest[hand] = nearest;
		c->distance[hand] = best;
		/* (a light tick as the hand comes to a hold) */
		if (best <= 0.0f && !climb.near[hand])
			hand_haptic(hand, 0.2f, 0.02f);
		climb.near[hand] = best <= 0.0f;
		vr.climb_near[hand] = best <= CLIMB_NEAR;
		if (value > CLIMB_GRIP_PRESSED && (!climb.pressed[hand] || (c->grab && !vr.climb_grip[hand])))
		{
			climb.pressed[hand] = 1;
			if (best <= 0.0f)
			{
				vr.climb_grip[hand] = 1;
				climb.hold[hand] = nearest;
				memcpy(climb.last[hand], place, sizeof(place));
				hand_haptic(hand, 0.55f, 0.05f);
				platform_log("vr: climb: the %s hand takes hold %d", hands[hand_controller(hand)], nearest);
			}
		}
		else if (value < CLIMB_GRIP_RELEASED)
		{
			climb.pressed[hand] = 0;
			if (vr.climb_grip[hand])
				platform_log("vr: climb: the %s hand lets go", hands[hand_controller(hand)]);
			vr.climb_grip[hand] = 0;
		}
		if (vr.climb_grip[hand])
		{
			c->held[hand] = climb.hold[hand];
			for (axis = 0; axis < 3; axis++)
			{
				c->moved[axis] -= place[axis] - climb.last[hand][axis];
				climb.last[hand][axis] = place[axis];
			}
			holding++;
		}
	}
	if (holding == 2)
	{
		for (axis = 0; axis < 3; axis++)
			c->moved[axis] *= 0.5f;
	}
	return holding;
}

void halo_vr_scope_readout(float *range, int *rounds_loaded, int *rounds_carried)
{
	*range = vr.scope.range;
	*rounds_loaded = vr.scope.rounds_loaded;
	*rounds_carried = vr.scope.rounds_carried;
}

void halo_vr_scope_animation(float *opened_seconds, float *closing, float *switched_seconds)
{
	*opened_seconds = vr.scope.opened_seconds;
	*closing = vr.scope.closing;
	*switched_seconds = vr.scope.switched_seconds;
}

void halo_vr_scope_shape(float *corner, float *dim, float *feather)
{
	*corner = vr.scope_set ? vr.scope.corner : -1.0f;
	*dim = vr.scope_set ? vr.scope.dim : 0.0f;
	*feather = vr.scope_set ? vr.scope.feather : 0.0f;
}

int halo_vr_scope_gun(void)
{
	settings_read();
	return settings.scope_gun;
}

/* a point along a pose's axes from its position (OpenXR axes; along:
metres forward, left, up), the axes those of the orientation or, given
forward and up, those */
static void pose_point(const float position[3], const float orientation[4], const float *forward_given,
	const float *up_given, const float along[3], float point[3])
{
	static const float backwards[3] = { 0.0f, 0.0f, -1.0f }, upwards[3] = { 0.0f, 1.0f, 0.0f };
	float forward[3], up[3], left[3];
	int axis;

	if (forward_given)
	{
		memcpy(forward, forward_given, sizeof(forward));
		memcpy(up, up_given, sizeof(up));
	}
	else
	{
		quaternion_rotate(orientation, backwards, forward);
		quaternion_rotate(orientation, upwards, up);
	}
	/* (left: up x forward, OpenXR's axes being right-handed) */
	left[0] = up[1] * forward[2] - up[2] * forward[1];
	left[1] = up[2] * forward[0] - up[0] * forward[2];
	left[2] = up[0] * forward[1] - up[1] * forward[0];
	for (axis = 0; axis < 3; axis++)
		point[axis] = position[axis] + forward[axis] * along[0] + left[axis] * along[1] + up[axis] * along[2];
}

/* the left hand's distance from a pouch's reach (reload_pouches; 0 within
it, large when the hand is not tracked), and the pouch's middle (game axes
from the recentred head) */
static float reload_pouch_distance(int pouch, float yaw, const float eyes[3], float middle[3])
{
	const struct vr_host_pose *hand = &vr.views.grip[0];
	const float *p = reload_pouches[pouch];
	float v[3], place[3], c = cosf(yaw), s = sinf(yaw), left = hand_left(p[1]), distance;
	int axis;

	middle[0] = eyes[0] + p[0] * c - left * s;
	middle[1] = eyes[1] + p[0] * s + left * c;
	middle[2] = eyes[2] + p[2];
	if (!hand->valid || !vr.views.aim[0].valid)
		return 1000.0f;
	for (axis = 0; axis < 3; axis++)
		v[axis] = hand->position[axis] - vr.origin_position[axis];
	game_axes(v, vr.origin_yaw, place);
	for (axis = 0; axis < 3; axis++)
		v[axis] = place[axis] - middle[axis];
	distance = length3(v) - p[3];
	return distance < 0.0f ? 0.0f : distance;
}

/* the patterns' pulses that are due, once a frame */
static void haptics_update(void)
{
	unsigned long now = GetTickCount();
	int hand;

	for (hand = 0; hand < 2; hand++)
	{
		while (vr.haptic_sent[hand] < vr.haptic_count[hand] &&
			now - vr.haptic_ticks[hand] >= (unsigned long)(vr.haptic_pulses[hand][vr.haptic_sent[hand]].start * 1000.0f))
		{
			const struct haptic_pulse *pulse = &vr.haptic_pulses[hand][vr.haptic_sent[hand]++];

			hand_haptic(hand, pulse->amplitude, pulse->seconds);
		}
	}
}

/* a hand's haptic pattern, its first pulse now and the others as they
come due (haptics_update); one played over another takes its place. Until
it ends the game's rumble leaves that hand alone (halo_vr_rumble): a pulse
sent after one replaces it, and the game's rumble goes on a moment after a
weapon stops firing, as a magazine is emptied and ejected */
static void haptic_play(int hand, const struct haptic_pulse *pulses, int count)
{
	if (count > HAPTIC_PULSES)
		count = HAPTIC_PULSES;
	memcpy(vr.haptic_pulses[hand], pulses, sizeof(*pulses) * (size_t)count);
	vr.haptic_count[hand] = count;
	vr.haptic_sent[hand] = 0;
	vr.haptic_ticks[hand] = GetTickCount();
	vr.haptic_end[hand] = vr.haptic_ticks[hand] +
		(unsigned long)((pulses[count - 1].start + pulses[count - 1].seconds) * 1000.0f);
	vr.haptic_kept_end[hand] = vr.haptic_end[hand];
	haptics_update();
}

/* a weapon's or a hit's pattern (haptic_play's), its pulses this strong
(times their own), unless the reload's or the lift's plays in that hand */
static void haptic_feel(int hand, const struct haptic_pulse *pulses, int count, float strength)
{
	unsigned long now = GetTickCount();
	int index;

	if ((long)(now - vr.haptic_kept_end[hand]) < 0 || strength <= 0.0f)
		return;
	if (count > HAPTIC_PULSES)
		count = HAPTIC_PULSES;
	for (index = 0; index < count; index++)
	{
		vr.haptic_pulses[hand][index] = pulses[index];
		vr.haptic_pulses[hand][index].amplitude = fminf(pulses[index].amplitude * strength, 1.0f);
	}
	vr.haptic_count[hand] = count;
	vr.haptic_sent[hand] = 0;
	vr.haptic_ticks[hand] = now;
	vr.haptic_end[hand] = now + (unsigned long)((pulses[count - 1].start + pulses[count - 1].seconds) * 1000.0f);
	haptics_update();
}

/* the reload by hand, once a frame (vr.manual_reload; RELOAD_* above):
after holsters_update, before two_handed_update and throw_update, which
leave the left grip to it when it is pressed at a pouch. The right stick
down ejects the magazine (not the shotgun's: it takes shells while it has
room); the left hand takes another from its pouch whenever the weapon has
room for it (a tick of its controller as it gets there, and a firmer one as
it takes it; the magazine in the weapon ejects itself if the stick has not)
and pushes it home where it goes (a knock in both hands): the game's reload
(vr_reload.c presses it). */
static void reload_update(void)
{
	static const char *const names[] = { "", "magazine", "shell", "tubes" };
	static const char *const articles[] = { "", "a ", "a ", "" };
	struct halo_vr_reload *r = &vr.reload;
	const struct halo_vr_reload_weapon *w = &vr.reload_weapon;
	const struct vr_host_pose *hand = &vr.views.grip[0];
	/* (the weapon's grip as vr_render_right_hand has it) */
	const struct vr_host_pose *right = settings.floating_hands && vr.views.grip[1].valid ? &vr.views.grip[1] :
		&vr.views.aim[1];
	float v[3], eyes[3], place[3], stick = vr.input.right_stick[1], hip_distance;
	/* (not a grip the lift of a vehicle has) */
	int pressed = vr.input.left_grip > RELOAD_GRIP_PRESSED && !vr.flip_grip[0] && !CLIMB_BUSY(0);
	int pressing = pressed && !vr.reload_grip_pressed;
	int pouch, in_pouch, near_pouch, axis;
	unsigned long now = GetTickCount();

	if (pressed)
	{
		vr.reload_grip_pressed = 1;
	}
	else if (vr.input.left_grip < RELOAD_GRIP_RELEASED)
	{
		vr.reload_grip_pressed = 0;
		vr.reload_grip_taken = 0;
		vr.reload_grip_waiting = 0;
	}
	if (vr.reload_full_tick && (long)(now - vr.reload_full_tick) >= 0)
	{
		hand_haptic(0, RELOAD_FULL_AMPLITUDE, RELOAD_FULL_SECONDS);
		vr.reload_full_tick = 0;
	}
	r->enabled = settings.manual_reload && settings.controller_aim && vr.aimed && vr.body_heading_valid;
	r->hands = settings.floating_hands && settings.controller_aim;
	memcpy(r->held_offset, reload_held, sizeof(r->held_offset));
	/* (another weapon, or none to reload by hand: what the hand did with the
	last one is over) */
	if (!r->enabled || w->kind == HALO_VR_RELOAD_NONE || w->weapon != vr.reload_weapon_index)
	{
		if (r->ejected || r->held)
			platform_log("vr: reload: the %s %s, for another weapon", names[r->kind], r->held ? "dropped" : "put back");
		r->ejected = 0;
		r->held = 0;
	}
	vr.reload_weapon_index = w->weapon;
	r->kind = r->enabled ? w->kind : HALO_VR_RELOAD_NONE;
	r->wanted = 0;
	r->pouch_distance = 1000.0f;
	r->well_distance = 1000.0f;
	if (!r->enabled)
	{
		vr.reload_in_pouch = 0;
		return;
	}
	/* the game reloading (X, or a reload the hand began): the magazine is
	in, and one held is not wanted (but the next shell, while the last
	loads) */
	if (w->reloading && (r->kind == HALO_VR_RELOAD_MAGAZINE || r->kind == HALO_VR_RELOAD_TUBE) &&
		(r->ejected || r->held))
	{
		platform_log("vr: reload: the game reloads: the %s in", names[r->kind]);
		r->ejected = 0;
		r->held = 0;
	}
	if (r->held && r->kind == HALO_VR_RELOAD_SHELL && !w->can_reload)
	{
		platform_log("vr: reload: no room for the shell: dropped");
		r->held = 0;
	}

	/* the right stick down ejects the magazine */
	if (stick < -RELOAD_STICK_PRESSED)
	{
		if (!vr.reload_stick_down && r->kind != HALO_VR_RELOAD_NONE)
		{
			if (r->kind != HALO_VR_RELOAD_SHELL && !r->ejected && w->can_reload)
			{
				r->ejected = 1;
				haptic_play(1, HAPTIC_FEEL(reload_eject_feel));
			}
			platform_log("vr: reload: eject: %s", r->kind == HALO_VR_RELOAD_SHELL ? "the shotgun takes shells, not a magazine" :
				r->ejected && w->can_reload ? (r->kind == HALO_VR_RELOAD_TUBE ? "the tubes out" : "the magazine out") :
				r->ejected ? "already out" :
				w->reloading ? "the game is reloading" : "nothing to reload (full, or no rounds left)");
		}
		vr.reload_stick_down = 1;
	}
	else if (stick > -RELOAD_STICK_RELEASED)
	{
		vr.reload_stick_down = 0;
	}

	/* the pouch, about the body (as the holsters are), and the left hand
	in it, or by it or the hip (the magazines' place, whatever the weapon: a
	plasma weapon's, the rocket launcher's), within RELOAD_POUCH_MARGIN; the
	one held from where it goes in. One is to be taken there whenever the
	weapon has room for it */
	pouch = r->kind == HALO_VR_RELOAD_TUBE;
	r->body_yaw = vr.body_heading;
	body_eyes(eyes);
	r->pouch_distance = reload_pouch_distance(pouch, r->body_yaw, eyes, r->pouch);
	hip_distance = pouch ? reload_pouch_distance(0, r->body_yaw, eyes, place) : r->pouch_distance;
	near_pouch = hip_distance <= RELOAD_POUCH_MARGIN ||
		(r->kind != HALO_VR_RELOAD_NONE && r->pouch_distance <= RELOAD_POUCH_MARGIN);
	r->wanted = r->kind != HALO_VR_RELOAD_NONE && !r->held && w->can_reload;
	if (r->held && w->well_known && right->valid && hand->valid && vr.views.aim[0].valid)
	{
		float forward[3], up[3], held[3], well[3];

		weapon_pointing(forward, up);
		pose_point(right->position, NULL, forward, up, w->well, well);
		pose_point(hand->position, vr.views.aim[0].orientation, NULL, NULL, reload_held, held);
		for (axis = 0; axis < 3; axis++)
			v[axis] = held[axis] - well[axis];
		r->well_distance = length3(v);
	}
	in_pouch = r->kind != HALO_VR_RELOAD_NONE && r->pouch_distance <= 0.0f;
	if (in_pouch && !vr.reload_in_pouch && r->wanted)
		hand_haptic(0, 0.2f, 0.02f);
	vr.reload_in_pouch = in_pouch;

	/* (taken as the grip closes in the pouch, or as the hand that closed by
	it comes in) */
	if (in_pouch && r->wanted && (pressing || (vr.reload_grip_waiting && vr.reload_grip_pressed)))
	{
		r->held = 1;
		r->wanted = 0;
		vr.reload_grip_taken = 1;
		vr.reload_grip_waiting = 0;
		hand_haptic(0, 0.35f, 0.03f);
		platform_log("vr: reload: %s%s taken from the %s%s", articles[r->kind], names[r->kind],
			pouch ? (settings.left_handed ? "right shoulder" : "left shoulder") :
			settings.left_handed ? "right hip" : "left hip", pressing ? "" : " (the grip closed by it)");
		/* (the magazine in the weapon out as the new one is taken, as the
		right stick would have: the weapon is drawn without it, and does not
		fire until the new one is in) */
		if (r->kind != HALO_VR_RELOAD_SHELL && !r->ejected)
		{
			r->ejected = 1;
			haptic_play(1, HAPTIC_FEEL(reload_eject_feel));
			platform_log("vr: reload: eject: the %s out, as the new %s taken", names[r->kind],
				r->kind == HALO_VR_RELOAD_TUBE ? "ones are" : "one is");
		}
		if (w->well_known && right->valid)
		{
			float forward[3], up[3], well[3];

			/* (where it goes, as debug.vr_test_hands places the hands) */
			weapon_pointing(forward, up);
			pose_point(right->position, NULL, forward, up, w->well, well);
			for (axis = 0; axis < 3; axis++)
				v[axis] = well[axis] - vr.origin_position[axis];
			game_axes(v, vr.origin_yaw, place);
			platform_log("vr: reload: it goes in %.2f m right, %.2f up, %.2f forward of the recentred head",
				-place[1], place[2], place[0]);
		}
	}
	else if (pressing && near_pouch)
	{
		/* (the grip there is the reload's all the same: never a grenade.
		Short of the pouch, it takes one as it gets there; in it, or with
		nothing to take, a double tick says so: full) */
		vr.reload_grip_taken = 1;
		vr.reload_grip_waiting = r->wanted && !in_pouch;
		if (!vr.reload_grip_waiting)
		{
			hand_haptic(0, RELOAD_FULL_AMPLITUDE, RELOAD_FULL_SECONDS);
			vr.reload_full_tick = now + RELOAD_FULL_GAP_MS;
			if (!vr.reload_full_tick)
				vr.reload_full_tick = 1;
		}
		platform_log("vr: reload: the grip by the pouch (%.2f m from its reach), nothing taken: %s",
			r->kind != HALO_VR_RELOAD_NONE ? r->pouch_distance : hip_distance, vr.reload_grip_waiting ?
			"one is taken as the hand gets there" : r->kind == HALO_VR_RELOAD_NONE ? "the weapon has no magazine" :
			w->reloading ? "the game is reloading" : "nothing to reload (full, or no rounds left)");
	}
	else if (r->held && !vr.reload_grip_pressed)
	{
		r->held = 0;
		platform_log("vr: reload: the %s let go of, %.2f m from where it goes in", names[r->kind], r->well_distance);
	}
	else if (r->held && r->well_distance < RELOAD_WELL_REACH)
	{
		r->held = 0;
		r->ejected = 0;
		r->inserted++;
		if (r->kind == HALO_VR_RELOAD_SHELL)
		{
			haptic_play(0, HAPTIC_FEEL(reload_shell_feel));
			haptic_play(1, HAPTIC_FEEL(reload_shell_weapon_feel));
		}
		else
		{
			haptic_play(0, HAPTIC_FEEL(reload_seat_feel));
			haptic_play(1, HAPTIC_FEEL(reload_seat_weapon_feel));
		}
		platform_log("vr: reload: the %s pushed home (%.3f m off)", names[r->kind], r->well_distance);
	}
}

void halo_vr_reload_weapon(const struct halo_vr_reload_weapon *weapon)
{
	vr.reload_weapon = *weapon;
}

int halo_vr_reload(struct halo_vr_reload *reload)
{
	*reload = vr.reload;
	if (!vr.initialized)
		reload->enabled = 0;
	if (!reload->enabled)
	{
		reload->kind = HALO_VR_RELOAD_NONE;
		reload->ejected = reload->held = reload->wanted = 0;
	}
	return reload->enabled;
}

/* the right hand's velocity, and from the head, at the newest sample over
seconds back (or as long as the samples go back, if a frame or more); 0 if
there is none */
static int melee_velocity(float seconds, float hand[3], float from_head[3])
{
	unsigned int newest = vr.melee_sample & (MELEE_SAMPLES - 1), back;
	int axis;

	if (!vr.melee_valid[newest])
		return 0;
	for (back = 1; back < MELEE_SAMPLES; back++)
	{
		unsigned int older = (vr.melee_sample - back) & (MELEE_SAMPLES - 1);
		double span;

		if (!vr.melee_valid[older])
			break;
		span = vr.melee_times[newest] - vr.melee_times[older];
		if (span >= seconds || back == MELEE_SAMPLES - 1 ||
			!vr.melee_valid[(vr.melee_sample - back - 1) & (MELEE_SAMPLES - 1)])
		{
			if (span <= 0.0)
				return 0;
			for (axis = 0; axis < 3; axis++)
			{
				hand[axis] = (float)((vr.melee_hand[newest][axis] - vr.melee_hand[older][axis]) / span);
				from_head[axis] = (float)((vr.melee_from_head[newest][axis] - vr.melee_from_head[older][axis]) / span);
			}
			return 1;
		}
	}
	return 0;
}

/* the melee by the right hand's swing, once a frame (vr.melee_gesture;
MELEE_* above): after holsters_update and reload_update, which it keeps out
of the way of */
static void melee_update(float seconds)
{
	static const float backwards[3] = { 0.0f, 0.0f, -1.0f };
	const struct vr_host_pose *hand = &vr.views.grip[1], *head = &vr.views.head;
	unsigned long now = GetTickCount();
	unsigned int sample;
	float velocity[3], from_head[3], forward[3], v[3], speed, moving, along, level;
	float inverse[4] = { -head->orientation[0], -head->orientation[1], -head->orientation[2], head->orientation[3] };
	int grip = vr.input.right_grip > MELEE_GRIP_PRESSED, axis;
	const char *guard = NULL;

	/* the hand's place, and from the head in the head's own axes, kept */
	vr.melee_clock += seconds;
	sample = ++vr.melee_sample & (MELEE_SAMPLES - 1);
	vr.melee_valid[sample] = hand->valid && head->valid && seconds > 0.0f;
	for (axis = 0; axis < 3; axis++)
		v[axis] = hand->position[axis] - head->position[axis];
	memcpy(vr.melee_hand[sample], hand->position, sizeof(vr.melee_hand[0]));
	quaternion_rotate(inverse, v, vr.melee_from_head[sample]);
	vr.melee_times[sample] = vr.melee_clock;
	vr.hand_speed = melee_velocity(MELEE_VELOCITY_SECONDS, velocity, from_head) ? length3(velocity) : 0.0f;

	/* (only on foot, the controllers aiming, in play) */
	if (!settings.melee_gesture || settings.gamepad_aim || !vr.crouch_able_frame || vr.menus ||
		!melee_velocity(0.0f, velocity, from_head))
	{
		vr.melee_swinging = 0;
		return;
	}
	moving = length3(velocity);
	melee_velocity(MELEE_VELOCITY_SECONDS, velocity, from_head);
	speed = length3(velocity);
	/* its way backward, along where the head faces, level */
	quaternion_rotate(head->orientation, backwards, forward);
	level = sqrtf(forward[0] * forward[0] + forward[2] * forward[2]);
	along = level > 0.0f && length3(velocity) > 0.0f ?
		-(velocity[0] * forward[0] + velocity[2] * forward[2]) / level / length3(velocity) : 0.0f;

	/* what keeps a swing from meleeing */
	if (vr.throw_state == THROW_SWING || vr.throw_state == THROW_RELEASED)
		guard = "the left hand throwing";
	if (vr.reload.held)
		guard = "the left hand holding a magazine";
	if (guard)
		vr.melee_guard_ticks = now;
	else if (now - vr.melee_guard_ticks < MELEE_GUARD_MS)
		guard = "just after a throw or a reload";
	else if (vr.holster_gripped)
		guard = "the grip taken at a holster";
	else if (vr.flip_grip[0] || vr.flip_grip[1])
		guard = "the hands lifting a vehicle";
	else if (vr.climb_grip[0] || vr.climb_grip[1])
		guard = "the hands climbing a vehicle";
	else if (vr.flip_ticks && now - vr.flip_ticks < FLIP_NO_MELEE_MS)
		guard = "just after a lift";
	else if (now - vr.holster_ticks < HOLSTER_NO_MELEE_MS)
		guard = "just after drawing";
	else if (vr.grab_gripped)
		guard = "the grip taken at a weapon";
	else if (vr.grab_ticks && now - vr.grab_ticks < GRAB_NO_MELEE_MS)
		guard = "just after a grab";
	else if (vr.press_ticks && now - vr.press_ticks < PRESS_NO_MELEE_MS)
		guard = "just after a press";

	/* the swing: from faster than MELEE_REARM_SPEED until slower again */
	if (speed < MELEE_REARM_SPEED)
	{
		if (vr.melee_swinging && settings.melee_log && !vr.melee_fired && vr.melee_fastest > MELEE_GRIP_SPEED)
		{
			platform_log("vr: melee: a swing of %.2f m/s over %.2f m%s, no melee (%s)", vr.melee_fastest,
				vr.melee_travel, vr.melee_grip ? ", the grip held" : "", vr.melee_guard);
		}
		vr.melee_swinging = 0;
		return;
	}
	if (!vr.melee_swinging)
	{
		vr.melee_swinging = 1;
		vr.melee_fired = 0;
		vr.melee_travel = 0.0f;
		vr.melee_seconds = 0.0f;
		vr.melee_frames = 0;
		vr.melee_fastest = 0.0f;
		vr.melee_grip = 0;
		vr.melee_grip_before = grip;
		vr.melee_guard = "slower than a swing, the grip not held";
		/* (the swing that brings a weapon just drawn out of its holster) */
		vr.melee_from_holster = vr.holster_drawn && vr.holsters.reach != HALO_VR_HOLSTER_NONE;
		vr.holster_drawn = 0;
	}
	vr.melee_travel += speed * seconds;
	vr.melee_seconds += seconds;
	vr.melee_frames++;
	if (speed > vr.melee_fastest)
		vr.melee_fastest = speed;
	vr.melee_grip |= grip;
	if (!guard && vr.melee_from_holster)
		guard = "bringing the drawn weapon out";
	if (!guard && speed > MELEE_FASTEST)
		guard = "too fast: the tracking jumped";
	if (!guard && length3(from_head) < MELEE_CARRIED * speed)
		guard = "carried with the head: a turn, a step";
	/* (a reach for a holster goes back to it, the grip open: with it held
	since before the swing, the hand draws nothing there, and swings) */
	if (!guard && along > MELEE_BACKWARD && !vr.melee_grip_before)
		guard = "going back toward the body";
	/* (why not, for debug.vr_melee_log, as it goes) */
	if (vr.melee_fired)
		return;
	if (!guard && speed < (grip ? MELEE_GRIP_SPEED : MELEE_SPEED))
		guard = vr.melee_grip ? "slower than a gripped swing" : "slower than a swing, the grip not held";
	if (!guard && (vr.melee_travel < MELEE_TRAVEL || vr.melee_seconds < MELEE_SUSTAIN_SECONDS ||
		vr.melee_frames < MELEE_SUSTAIN_FRAMES))
		guard = "too short";
	if (!guard && moving < MELEE_REARM_SPEED)
		guard = "not moving on: the tracking jumped";
	if (!guard && now - vr.melee_ticks < MELEE_INTERVAL_MS)
		guard = "too soon after the last";
	if (guard)
	{
		/* (the slowness of a swing that was guarded says less than its
		guard) */
		if (strncmp(guard, "slower", 6) != 0 || !strncmp(vr.melee_guard, "slower", 6))
			vr.melee_guard = guard;
		return;
	}
	vr.melee_fired = 1;
	vr.melee_ticks = now;
	vr.melee_release = now + MELEE_PRESS_MS;
	platform_log("vr: melee: a swing of %.2f m/s over %.2f m%s", speed, vr.melee_travel,
		grip ? ", the grip held" : "");
}

/* units.c: the first player's melee landed (on anything: its sound plays) */
void halo_vr_melee_hit(void)
{
	if (!vr.initialized)
		return;
	hand_haptic(1, MELEE_HIT_AMPLITUDE, MELEE_HIT_SECONDS);
	if (settings.melee_log)
		platform_log("vr: melee: it landed");
}

/* the weapon's feel is the VR mode's: the controllers aim (not a gamepad) */
int halo_vr_weapon_feel(void)
{
	return vr.initialized && settings.controller_aim && (vr.aiming || vr.aimed);
}

float halo_vr_recoil(void)
{
	return halo_vr_weapon_feel() ? settings.recoil : 0.0f;
}

/* vr_feel.c: the first player's weapon in hand fired (weapon_feels) */
void halo_vr_weapon_fire(int feel)
{
	if (!halo_vr_weapon_feel() || feel < 0 || feel >= HALO_VR_FEELS)
		return;
	haptic_feel(1, weapon_feels[feel].pulses, weapon_feels[feel].count, settings.weapon_haptics);
	if (vr.two_handed)
		haptic_feel(0, weapon_feels[feel].pulses, weapon_feels[feel].count, settings.weapon_haptics * WEAPON_FOREGRIP);
}

/* ... and charging (the plasma pistol), once a frame: a hum in its hand
between its pulses, rising with the charge */
void halo_vr_weapon_charge(float charge)
{
	unsigned long now = GetTickCount();
	float amplitude = WEAPON_CHARGE_HUM * charge * settings.weapon_haptics;

	if (charge <= 0.0f || !halo_vr_weapon_feel() || amplitude <= 0.0f || (long)(now - vr.haptic_end[1]) < 0)
		return;
	hand_haptic(1, amplitude, 0.05f);
}

/* vr_feel.c: the first player hit, from that direction (game axes from the
recentred head), this hard (0 to 1) */
void halo_vr_hit(const float from[3], float strength)
{
	static const float backwards[3] = { 0.0f, 0.0f, -1.0f };
	float v[3], forward[3], left[3], across, side;

	if (!halo_vr_weapon_feel() || !vr.views.head.valid)
		return;
	/* (how far to its side, of the head's left across the level) */
	quaternion_rotate(vr.views.head.orientation, backwards, v);
	game_axes(v, vr.origin_yaw, forward);
	across = sqrtf(forward[0] * forward[0] + forward[1] * forward[1]);
	if (across <= 1e-4f)
		return;
	left[0] = -forward[1] / across;
	left[1] = forward[0] / across;
	left[2] = 0.0f;
	side = -(from[0] * left[0] + from[1] * left[1] + from[2] * left[2]);
	side = side < -1.0f ? -1.0f : side > 1.0f ? 1.0f : side;
	vr.hit_side = side;
	vr.hit_ticks = GetTickCount();
	strength = (HIT_LEAST + (1.0f - HIT_LEAST) * strength) * settings.weapon_haptics;
	haptic_feel(1, HAPTIC_FEEL(hit_feel), strength * (0.5f + 0.5f * side));
	haptic_feel(0, HAPTIC_FEEL(hit_feel), strength * (0.5f - 0.5f * side));
}

/* ... and the shields broken, in both hands, the more on the side of the
hit that broke them */
void halo_vr_shields_broken(void)
{
	float side = GetTickCount() - vr.hit_ticks < HIT_RECENT_MS ? vr.hit_side : 0.0f;

	if (!halo_vr_weapon_feel())
		return;
	haptic_feel(1, HAPTIC_FEEL(shields_broken_feel), settings.weapon_haptics * (0.75f + 0.25f * side));
	haptic_feel(0, HAPTIC_FEEL(shields_broken_feel), settings.weapon_haptics * (0.75f - 0.25f * side));
}

/* the right index finger on the trigger or off it, once a frame
(TRIGGER_TOUCH_* above) */
static void trigger_touch_update(float seconds)
{
	int sensed = vr.input.right_trigger_touch >= 0;
	float on = !sensed || vr.input.right_trigger_touch || vr.input.right_trigger > TRIGGER_TOUCH_PULL ? 1.0f : 0.0f;
	float step = seconds / TRIGGER_TOUCH_SECONDS;

	/* (the input's state is the runtime's once the controllers are
	active: said once, and again if it changes) */
	if ((vr.input.active[1] || sensed) && sensed != vr.trigger_touch_sensed)
	{
		platform_log("vr: the %s trigger %s", settings.left_handed ? "left" : "right", sensed ? "senses a finger on "
			"it: the index finger lies off it until then" : "senses no finger on it: the index finger rests on it");
		vr.trigger_touch_sensed = sensed;
	}
	if (vr.trigger_touch < on)
		vr.trigger_touch = vr.trigger_touch + step < on ? vr.trigger_touch + step : on;
	else
		vr.trigger_touch = vr.trigger_touch - step > on ? vr.trigger_touch - step : on;
}

/* the lift of an overturned vehicle (vr.vehicle_flip; FLIP_* above), once
a frame, before what else the grips do (the holsters, the reload, the
foregrip, the throw, the recentring), which leave a grip that is the lift's
alone */
static void flip_update(float seconds)
{
	static const char *const hands[2] = { "left", "right" };
	int able = settings.flip_lift && !settings.gamepad_aim && vr.aimed && vr.crouch_able_frame && !vr.menus &&
		vr.flip_offered && vr.origin_valid && vr.views.head.valid && vr.body_heading_valid;
	float eyes[3], toward[2], heights[2] = { 0.0f, 0.0f }, out = 0.0f, length;
	unsigned int sample;
	int hand, low_out;

	if (vr.flip_press_frames > 0)
		vr.flip_press_frames--;
	vr.flip_clock += seconds;
	if (vr.flip_offered != vr.flip_offer_logged)
	{
		platform_log("vr: flip: %s", !vr.flip_offered ? "no flip on offer" : able ?
			"a flip on offer: both hands low beside it, gripping, then lifted" : settings.flip_lift &&
			!settings.gamepad_aim ? "a flip on offer, not to lift now" : "a flip on offer, to X");
		vr.flip_offer_logged = vr.flip_offered;
	}

	/* toward the vehicle, level, from the eyes (or where the body faces) */
	body_eyes(eyes);
	toward[0] = cosf(vr.body_heading);
	toward[1] = sinf(vr.body_heading);
	if (vr.flip_vehicle_known)
	{
		float to[2] = { vr.flip_vehicle[0] - eyes[0], vr.flip_vehicle[1] - eyes[1] };

		length = sqrtf(to[0] * to[0] + to[1] * to[1]);
		if (length > 0.05f)
		{
			toward[0] = to[0] / length;
			toward[1] = to[1] / length;
		}
	}

	/* both hands: low, and out toward the vehicle (their middle) */
	for (hand = 0; hand < 2; hand++)
	{
		const struct vr_host_pose *grip = &vr.views.grip[hand];
		float v[3], place[3];
		int axis;

		if (!grip->valid)
			continue;
		for (axis = 0; axis < 3; axis++)
			v[axis] = grip->position[axis] - vr.origin_position[axis];
		game_axes(v, vr.origin_yaw, place);
		heights[hand] = place[2];
		out += ((place[0] - eyes[0]) * toward[0] + (place[1] - eyes[1]) * toward[1]) * 0.5f;
	}
	low_out = vr.views.grip[0].valid && vr.views.grip[1].valid && heights[0] < -FLIP_LOW &&
		heights[1] < -FLIP_LOW && out > FLIP_OUT;

	/* each grip: taken with both hands so, the lift's until let go (one
	hand low at the hip, the other up, takes a magazine, draws, as ever) */
	for (hand = 0; hand < 2; hand++)
	{
		float value = hand ? vr.input.right_grip : vr.input.left_grip;
		int pressing = value > FLIP_GRIP_PRESSED && !vr.flip_grip_pressed[hand];

		if (value > FLIP_GRIP_PRESSED)
		{
			vr.flip_grip_pressed[hand] = 1;
		}
		else if (value < FLIP_GRIP_RELEASED)
		{
			vr.flip_grip_pressed[hand] = 0;
			vr.flip_grip[hand] = 0;
		}
		if (pressing && able && low_out)
		{
			vr.flip_grip[hand] = 1;
			haptic_play(hand, HAPTIC_FEEL(flip_hand_feel));
			platform_log("vr: flip: the %s hand takes hold, %.2f m below the eyes, the hands %.2f m out toward it",
				hands[hand_controller(hand)], -heights[hand], out);
		}
		else if (pressing && able)
		{
			platform_log("vr: flip: the %s grip, the hands %.2f and %.2f m below the eyes and %.2f m out toward it: "
				"not both low and out (%.2f, %.2f)", hands[hand_controller(hand)], -heights[0], -heights[1], out,
				FLIP_LOW, FLIP_OUT);
		}
	}

	/* the hands' heights, kept */
	sample = ++vr.flip_sample & (FLIP_SAMPLES - 1);
	vr.flip_times[sample] = vr.flip_clock;
	vr.flip_heights[0][sample] = heights[0];
	vr.flip_heights[1][sample] = heights[1];
	vr.flip_heights_valid[sample] = vr.views.grip[0].valid && vr.views.grip[1].valid && seconds > 0.0f;

	switch (vr.flip_state)
	{
	case FLIP_IDLE:
		if (able && vr.flip_grip[0] && vr.flip_grip[1])
		{
			vr.flip_state = FLIP_HOLDING;
			vr.flip_held_clock = vr.flip_clock;
			haptic_play(0, HAPTIC_FEEL(flip_hold_feel));
			haptic_play(1, HAPTIC_FEEL(flip_hold_feel));
			platform_log("vr: flip: both hands hold it");
		}
		break;
	case FLIP_HOLDING:
		if (!vr.flip_grip[0] || !vr.flip_grip[1] || !able)
		{
			platform_log("vr: flip: %s, no lift", !able ? "the flip no longer on offer" : "a hand let go");
			vr.flip_state = FLIP_IDLE;
		}
		else
		{
			/* (each hand's rise from its lowest within FLIP_LIFT_SECONDS,
			since it took hold) */
			float rise[2] = { 0.0f, 0.0f };
			unsigned int back;

			for (back = 0; back < FLIP_SAMPLES; back++)
			{
				unsigned int k = (vr.flip_sample - back) & (FLIP_SAMPLES - 1);

				if (vr.flip_times[k] < vr.flip_clock - FLIP_LIFT_SECONDS || vr.flip_times[k] < vr.flip_held_clock)
					break;
				if (!vr.flip_heights_valid[k])
					continue;
				for (hand = 0; hand < 2; hand++)
				{
					if (heights[hand] - vr.flip_heights[hand][k] > rise[hand])
						rise[hand] = heights[hand] - vr.flip_heights[hand][k];
				}
			}
			if (vr.flip_heights_valid[sample] && rise[0] >= FLIP_LIFT_HEIGHT && rise[1] >= FLIP_LIFT_HEIGHT)
			{
				vr.flip_state = FLIP_LIFTED;
				vr.flip_press_frames = FLIP_PRESS_FRAMES;
				vr.flip_ticks = GetTickCount();
				haptic_play(0, HAPTIC_FEEL(flip_lift_feel));
				haptic_play(1, HAPTIC_FEEL(flip_lift_feel));
				platform_log("vr: flip: lifted, %.2f and %.2f m within %.2f s, held %.2f s: the game's action",
					rise[0], rise[1], FLIP_LIFT_SECONDS, vr.flip_clock - vr.flip_held_clock);
			}
		}
		break;
	case FLIP_LIFTED:
		if (!vr.flip_grip[0] && !vr.flip_grip[1])
			vr.flip_state = FLIP_IDLE;
		break;
	}
}

/* player_control.c, as the first player's input is read, each frame: the
game's offer of a flip told, what flips it given, and the lift's press */
int halo_vr_flip(struct halo_vr_flip *flip)
{
	flip->flips = halo_vr_flips();
	flip->pressed = 0;
	if (!vr.initialized)
		return 0;
	vr.flip_offered = flip->offered;
	vr.flip_vehicle_known = flip->vehicle_known;
	memcpy(vr.flip_vehicle, flip->vehicle, sizeof(vr.flip_vehicle));
	flip->pressed = vr.flip_press_frames > 0;
	return 1;
}

/* hud.c and the above: what flips an overturned vehicle now (with
vr.aim = "gamepad", X: the gamepad plays as on the flat screen) */
int halo_vr_flips(void)
{
	int lift;

	if (!vr.initialized)
		return HALO_VR_FLIP_BUTTON;
	settings_read();
	lift = settings.flip_lift && !settings.gamepad_aim;
	return (lift ? HALO_VR_FLIP_LIFT : 0) | (settings.flip_button || !lift ? HALO_VR_FLIP_BUTTON : 0);
}

/* in a seat steered by handlebars, placed this frame or the last */
static int handlebars_active(void)
{
	return settings.controller_aim && vr.bars_placed_valid && vr.presents - vr.bars_placed <= HANDLEBAR_PLACED_FRAMES;
}

/* and steered by its controls (the bars, a wheel), the hands on them
(drawn on them when held), and both hands holding them */
static int seat_steers(void)
{
	return handlebars_active() && (vr.seat.kind == HALO_VR_SEAT_BARS || vr.seat.kind == HALO_VR_SEAT_WHEEL);
}

static int seat_hands(void)
{
	return handlebars_active() && vr.seat.kind != HALO_VR_SEAT_RIDE && vr.seat.handles_valid;
}

static int seat_held(void)
{
	return seat_hands() && vr.bars_held[0] && vr.bars_held[1];
}

/* steered by the sticks: a seat steered by its controls, without both hands
holding them (STICK_STEER_*) */
static int seat_stick_steers(void)
{
	return seat_steers() && !(vr.bars_held[0] && vr.bars_held[1]);
}

/* driving a vehicle steered toward the aim, told this frame or the last */
static int seat_drives(void)
{
	return settings.controller_aim && vr.drive_told_valid && vr.presents - vr.drive_told <= HANDLEBAR_PLACED_FRAMES &&
		!handlebars_active();
}

/* a stick's sideways push past STICK_STEER_DEADZONE, from it (-1 to 1) */
static float stick_steer_push(float x)
{
	if (fabsf(x) <= STICK_STEER_DEADZONE)
		return 0.0f;
	x = (x - (x > 0.0f ? STICK_STEER_DEADZONE : -STICK_STEER_DEADZONE)) / (1.0f - STICK_STEER_DEADZONE);
	return x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
}

/* a hand's place (grip), and its pointing's pitch (radians), in the game's
axes from the recentred head (metres) */
static int bars_hand(int hand, float place[3], float *pitch)
{
	static const float backwards[3] = { 0.0f, 0.0f, -1.0f };
	float v[3], f[3];
	int axis;

	if (!vr.views.grip[hand].valid || !vr.views.aim[hand].valid)
		return 0;
	for (axis = 0; axis < 3; axis++)
		v[axis] = vr.views.grip[hand].position[axis] - vr.origin_position[axis];
	game_axes(v, vr.origin_yaw, place);
	quaternion_rotate(vr.views.aim[hand].orientation, backwards, v);
	game_axes(v, vr.origin_yaw, f);
	*pitch = asinf(f[2] < -1.0f ? -1.0f : f[2] > 1.0f ? 1.0f : f[2]);
	return 1;
}

/* the line between the hands (the left's to the right's), across the
wheel's axis: in its plane, of length 1 */
static void wheel_line(float place[2][3], float line[3])
{
	const float *n = vr.seat.wheel_axis;
	float d[3], along, length;
	int axis;

	for (axis = 0; axis < 3; axis++)
		d[axis] = place[1][axis] - place[0][axis];
	along = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
	for (axis = 0; axis < 3; axis++)
		line[axis] = d[axis] - along * n[axis];
	length = length3(line);
	for (axis = 0; axis < 3; axis++)
		line[axis] = length > 0.0f ? line[axis] / length : 0.0f;
}

/* the facing's turn from the Ghost's heading (radians, unsigned) that keeps
it turning this fast (radians a second): the error at which the game's turn
toward the facing is as fast */
static float turn_facing(float rate)
{
	float tick, error;

	if (rate > HANDLEBAR_TURN_FASTEST)
		rate = HANDLEBAR_TURN_FASTEST;
	if (rate <= 0.0f)
		return 0.0f;
	tick = rate / HANDLEBAR_TICKS_PER_SECOND;
	error = tick * tick / (2.0f * HANDLEBAR_GHOST_TURN_ACCELERATION);
	if (error < 2.0f * tick)
		error = 2.0f * tick;
	return error;
}

/* the facing's turn from the vehicle's heading (radians) that keeps it
turning as the bars' turn asks (HANDLEBAR_TURN_*) */
static float bars_facing(float turn)
{
	float error = turn_facing(fabsf(turn) * HANDLEBAR_TURN_RATE);

	return turn < 0.0f ? -error : error;
}

/* the facing's turn from the vehicle's heading (radians: to the left) the
sticks steer by, with the hands off the controls (STICK_STEER_*): the
stronger sideways push of the two (to the right, positive) */
static float stick_facing(int kind, float left_x, float right_x, float lock_left, float lock_right, float steer_sign)
{
	float left = stick_steer_push(left_x), right = stick_steer_push(right_x);
	float push = fabsf(left) > fabsf(right) ? left : right;

	if (push == 0.0f)
		return 0.0f;
	if (kind == HALO_VR_SEAT_BARS)
	{
		float error = turn_facing(fabsf(push) * HANDLEBAR_TURN_FASTEST);

		return push > 0.0f ? -error : error;
	}
	if (kind == HALO_VR_SEAT_WHEEL)
		return (push > 0.0f ? -push * lock_right : -push * lock_left) * steer_sign;
	return 0.0f;
}

/* without both hands on the controls, once a frame: the turn the sticks
steer by eased toward (STICK_STEER_*), else out (ease: this frame's of
HANDLEBAR_RELEASE_SECONDS) */
static void stick_steer_update(float seconds, float ease)
{
	float steer = seat_stick_steers() ? stick_facing(vr.seat.kind, vr.input.left_stick[0], vr.input.right_stick[0],
		vr.seat.lock[0], vr.seat.lock[1], vr.seat.steer_sign) : 0.0f;

	if (steer != 0.0f)
		vr.bars_turn += (steer - vr.bars_turn) * (1.0f - expf(-seconds / STICK_STEER_SECONDS));
	else
		vr.bars_turn -= vr.bars_turn * ease;
}

/* the handlebars, once a frame (HANDLEBAR_* above): each hand takes its
handle or lets go, and with both held the bars' turn and pitch steer */
static void handlebars_update(float seconds)
{
	float place[2][3], pitch[2], v[3];
	float grips[2] = { vr.input.left_grip, vr.input.right_grip };
	int hand, both, was_both = vr.bars_held[0] && vr.bars_held[1];
	float ease = 1.0f - expf(-seconds / HANDLEBAR_RELEASE_SECONDS);

	if (!seat_hands())
	{
		/* (no handles to hold: the sticks steer alone) */
		vr.bars_held[0] = vr.bars_held[1] = 0;
		vr.bars_pitch = 0.0f;
		if (seat_stick_steers())
			stick_steer_update(seconds, ease);
		else
			vr.bars_turn = 0.0f;
		return;
	}
	for (hand = 0; hand < 2; hand++)
	{
		int valid = bars_hand(hand, place[hand], &pitch[hand]);
		/* (each hand's handle is the one on its side, the weapon's hand the
		left one's: vr.dominant_hand) */
		int side = hand_controller(hand);
		int axis;

		if (vr.bars_held[hand] && (grips[hand] < HANDLEBAR_GRIP_RELEASED || !valid))
		{
			vr.bars_held[hand] = 0;
			platform_log("vr: %s: the %s hand lets go", vr.seat.kind == HALO_VR_SEAT_WHEEL ? "wheel" : vr.seat.kind ==
				HALO_VR_SEAT_TURRET ? "turret" : "handlebars", side ? "right" : "left");
		}
		else if (!vr.bars_held[hand] && valid && grips[hand] > HANDLEBAR_GRIP_PRESSED && !vr.climb_grip[hand])
		{
			for (axis = 0; axis < 3; axis++)
				v[axis] = place[hand][axis] - vr.seat.handles[side][axis];
			if (length3(v) < HANDLEBAR_REACH)
			{
				vr.bars_held[hand] = 1;
				hand_haptic(hand, 0.3f, 0.02f);
				platform_log("vr: %s: the %s hand takes its handle (%.2f m from it, at %.2f %.2f %.2f m: forward, left, "
					"up of the recentred head)", vr.seat.kind == HALO_VR_SEAT_WHEEL ? "wheel" : vr.seat.kind ==
					HALO_VR_SEAT_TURRET ? "turret" : "handlebars", side ? "right" : "left", length3(v),
					vr.seat.handles[side][0], vr.seat.handles[side][1], vr.seat.handles[side][2]);
			}
		}
	}
	both = vr.bars_held[0] && vr.bars_held[1];
	if (both && !was_both)
	{
		vr.bars_pitch_zero = (pitch[0] + pitch[1]) * 0.5f;
		wheel_line(place, vr.bars_wheel_zero);
	}
	if (both && vr.seat.kind == HALO_VR_SEAT_WHEEL)
	{
		/* (the wheel's turn about its axis from where it was taken hold of,
		counterclockwise as the driver sees it to the left) */
		float line[3], c[3], turn, lock;

		wheel_line(place, line);
		c[0] = vr.bars_wheel_zero[1] * line[2] - vr.bars_wheel_zero[2] * line[1];
		c[1] = vr.bars_wheel_zero[2] * line[0] - vr.bars_wheel_zero[0] * line[2];
		c[2] = vr.bars_wheel_zero[0] * line[1] - vr.bars_wheel_zero[1] * line[0];
		turn = atan2f(c[0] * vr.seat.wheel_axis[0] + c[1] * vr.seat.wheel_axis[1] + c[2] * vr.seat.wheel_axis[2],
			line[0] * vr.bars_wheel_zero[0] + line[1] * vr.bars_wheel_zero[1] + line[2] * vr.bars_wheel_zero[2]);
		turn = fabsf(turn) <= HANDLEBAR_DEADZONE ? 0.0f : turn - (turn > 0.0f ? HANDLEBAR_DEADZONE : -HANDLEBAR_DEADZONE);
		turn /= WHEEL_RATIO;
		lock = turn > 0.0f ? vr.seat.lock[0] : vr.seat.lock[1];
		if (fabsf(turn) > lock)
			turn = turn > 0.0f ? lock : -lock;
		vr.bars_turn = turn * vr.seat.steer_sign;
		vr.bars_pitch = 0.0f;
	}
	else if (both && vr.seat.kind == HALO_VR_SEAT_BARS)
	{
		/* (the bars' turn: the line from the left hand to the right, straight
		across when it points right, turned left as the right hand comes
		forward) */
		const float *left = place[hand_controller(0)], *right = place[hand_controller(1)];
		float turn = atan2f(right[0] - left[0], -(right[1] - left[1]));
		float tilt = (pitch[0] + pitch[1]) * 0.5f - vr.bars_pitch_zero;

		turn = fabsf(turn) <= HANDLEBAR_DEADZONE ? 0.0f : turn - (turn > 0.0f ? HANDLEBAR_DEADZONE : -HANDLEBAR_DEADZONE);
		vr.bars_turn = bars_facing(turn);
		vr.bars_pitch = tilt < -HANDLEBAR_PITCH_LIMIT ? -HANDLEBAR_PITCH_LIMIT :
			tilt > HANDLEBAR_PITCH_LIMIT ? HANDLEBAR_PITCH_LIMIT : tilt;
	}
	else if (!both)
	{
		/* (let go: the sticks steer; centred, the course held, the turn
		easing out) */
		stick_steer_update(seconds, ease);
		vr.bars_pitch -= vr.bars_pitch * ease;
	}
}

void halo_vr_seat_place(const struct halo_vr_seat *seat)
{
	if (!vr.initialized)
		return;
	if (seat->kind != vr.seat.kind || !handlebars_active())
		vr.bars_held[0] = vr.bars_held[1] = 0;
	vr.seat = *seat;
	vr.bars_placed = vr.presents;
	vr.bars_placed_valid = 1;
	/* (the view, the aim and the hands turn with the vehicle) */
	vr.body_yaw = seat->heading;
}

void halo_vr_seat_drive(void)
{
	if (!vr.initialized)
		return;
	vr.drive_told = vr.presents;
	vr.drive_told_valid = 1;
}

int halo_vr_seat_enabled(int kind)
{
	return vr.initialized && settings.controller_aim &&
		(kind == HALO_VR_SEAT_BARS ? settings.handlebars : kind == HALO_VR_SEAT_WHEEL ? settings.wheel : 1);
}

/* the comfort vignette, once a frame (VIGNETTE_* above) */
static void vignette_update(float seconds)
{
	float asked = settings.comfort_vignette && vr.presents - vr.vignette_asked_at <= 1 ? vr.vignette_asked : 0.0f;
	float constant = asked > vr.vignette ? VIGNETTE_IN_SECONDS : VIGNETTE_OUT_SECONDS;

	if (seconds > 0.0f)
		vr.vignette += (asked - vr.vignette) * (1.0f - expf(-seconds / constant));
	if (vr.vignette < 0.005f && asked <= 0.0f)
		vr.vignette = 0.0f;
}

/* the play space turned (port/linux/game/vr_seat.c: getting into a feed
cockpit, so as to face its display), as the stick turns it */
void halo_vr_body_turn(float radians)
{
	if (vr.initialized)
		vr.body_yaw += radians;
}

void halo_vr_comfort_vignette(float amount)
{
	vr.vignette_asked = amount < 0.0f ? 0.0f : amount > 1.0f ? 1.0f : amount;
	vr.vignette_asked_at = vr.presents;
}

float halo_vr_vignette(float fov[2][4])
{
	if (!vr.initialized || vr.vignette <= 0.0f)
		return 0.0f;
	memcpy(fov, vr.views.fov, sizeof(vr.views.fov));
	return vr.vignette;
}

/* ---------- foveation (vr.foveation; xgpu_gl.c draws it) */

float halo_vr_foveation_inset(void)
{
	return vr.initialized && vr.foveation ? vr.foveation_inset : 0.0f;
}

const char *halo_vr_foveation_name(void)
{
	return !vr.initialized || !vr.foveation ? "off" : vr.foveation == 1 ? "fixed (the middle of each eye's view)" :
		settings.test_gaze ? "the eyes' gaze (debug.vr_test_gaze)" : "the eyes' gaze (XR_EXT_eye_gaze_interaction)";
}

/* the gaze followed, once a frame: where the runtime (or the test) says the
eyes look, smoothed; the middle without it */
static void fovea_follow(void)
{
	float seconds = vr.views.display_elapsed > 0.0f ? vr.views.display_elapsed : vr.views.display_period;
	float target[2], constant, moved;
	int valid = 0;

	if (settings.test_gaze)
	{
		float sign = settings.test_gaze_period > 0.0f &&
			((long)(vr.test_head_seconds / settings.test_gaze_period) & 1) ? -1.0f : 1.0f;

		target[0] = tanf(sign * settings.test_gaze_angles[0] * (float)M_PI / 180.0f);
		target[1] = tanf(sign * settings.test_gaze_angles[1] * (float)M_PI / 180.0f);
		valid = 1;
	}
	else if (vr.views.gaze.valid)
	{
		static const float forward[3] = { 0.0f, 0.0f, -1.0f };
		float d[3];

		quaternion_rotate(vr.views.gaze.orientation, forward, d);
		if (d[2] < -0.1f)
		{
			target[0] = d[0] / -d[2];
			target[1] = d[1] / -d[2];
			valid = 1;
		}
	}
	if (valid && !vr.fovea_seen)
	{
		vr.fovea_gaze[0] = target[0];
		vr.fovea_gaze[1] = target[1];
		vr.fovea_seen = 1;
	}
	if (valid)
	{
		vr.fovea_lost = 0.0f;
		moved = atanf(sqrtf((target[0] - vr.fovea_gaze[0]) * (target[0] - vr.fovea_gaze[0]) +
			(target[1] - vr.fovea_gaze[1]) * (target[1] - vr.fovea_gaze[1]))) * 180.0f / (float)M_PI;
		constant = moved >= FOVEA_SACCADE_DEGREES ? FOVEA_FOLLOW_SECONDS : FOVEA_SETTLE_SECONDS;
	}
	else
	{
		vr.fovea_lost += seconds;
		if (vr.fovea_lost < FOVEA_LOST_SECONDS)
			return;
		target[0] = target[1] = 0.0f;
		constant = FOVEA_RETURN_SECONDS;
	}
	if (seconds > 0.0f)
	{
		float t = 1.0f - expf(-seconds / constant);

		vr.fovea_gaze[0] += (target[0] - vr.fovea_gaze[0]) * t;
		vr.fovea_gaze[1] += (target[1] - vr.fovea_gaze[1]) * t;
	}
}

int halo_vr_foveation(float centres[2][2])
{
	float inverse[4], head[4], direction[3], d[3];
	int eye;

	if (!vr.initialized || !vr.foveation || !vr.frame)
		return 0;
	if (vr.foveation == 2 && vr.fovea_frame != vr.presents)
	{
		vr.fovea_frame = vr.presents;
		fovea_follow();
	}
	/* the gaze from the head, then in each eye (as its image was drawn:
	its own turn and field of view) */
	direction[0] = vr.foveation == 2 ? vr.fovea_gaze[0] : 0.0f;
	direction[1] = vr.foveation == 2 ? vr.fovea_gaze[1] : 0.0f;
	direction[2] = -1.0f;
	for (eye = 0; eye < 2; eye++)
	{
		const float *fov = vr.views.fov[eye];
		float u, v;

		if (vr.foveation == 2 && vr.views.head.valid && vr.views.eye[eye].valid)
		{
			/* (the eye's turn from the head's: their inverse) */
			inverse[0] = -vr.views.eye[eye].orientation[0];
			inverse[1] = -vr.views.eye[eye].orientation[1];
			inverse[2] = -vr.views.eye[eye].orientation[2];
			inverse[3] = vr.views.eye[eye].orientation[3];
			quaternion_multiply(inverse, vr.views.head.orientation, head);
			quaternion_rotate(head, direction, d);
		}
		else
		{
			memcpy(d, direction, sizeof(d));
		}
		if (d[2] > -0.1f || fov[1] <= fov[0] || fov[2] <= fov[3])
		{
			centres[eye][0] = centres[eye][1] = 0.5f;
			continue;
		}
		u = (d[0] / -d[2] - fov[0]) / (fov[1] - fov[0]);
		v = (fov[2] - d[1] / -d[2]) / (fov[2] - fov[3]);
		centres[eye][0] = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
		centres[eye][1] = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
	}
	return vr.foveation;
}

/* the controllers' turning and recentring, once a frame */
static void input_update(void)
{
	unsigned long now = GetTickCount();
	float seconds = vr.last_ticks ? (float)(now - vr.last_ticks) * 0.001f : 0.0f;
	/* (vr.aim = "gamepad": the gamepad's right stick turns, as on the flat
	screen, and the view after it, halo_vr_aim; the controllers' does not) */
	float x = settings.gamepad_aim ? 0.0f : vr.input.right_stick[0];

	vr.last_ticks = now;
	if (seconds > 0.1f)
		seconds = 0.1f;
	body_update(vr.views.display_elapsed > 0.0f ? vr.views.display_elapsed : seconds);
	flip_update(seconds);
	holsters_update(vr.views.display_elapsed > 0.0f ? vr.views.display_elapsed : seconds);
	grab_update();
	reload_update();
	haptics_update();
	/* a swing of the right controller melees (vr.melee_gesture) */
	melee_update(vr.views.display_elapsed > 0.0f ? vr.views.display_elapsed : seconds);
	trigger_touch_update(vr.views.display_elapsed > 0.0f ? vr.views.display_elapsed : seconds);
	two_handed_update(seconds);
	handlebars_update(vr.views.display_elapsed > 0.0f ? vr.views.display_elapsed : seconds);
	vignette_update(vr.views.display_elapsed > 0.0f ? vr.views.display_elapsed : seconds);
	crouch_update();
	/* (the poses' times: from one frame's display to the next's) */
	throw_update(vr.views.display_elapsed > 0.0f ? vr.views.display_elapsed : seconds);
	/* debug.vr_test_turn: turning without hands, for automated tests (the
	gamepad's aim turns the body itself) */
	if (!settings.gamepad_aim)
		vr.body_yaw += settings.test_turn * seconds;
	/* a gamepad's Back held recentres (the controllers may be off) */
	if (vr.gamepad_back)
	{
		vr.gamepad_recentre_time += seconds;
		if (vr.gamepad_recentre_time >= GAMEPAD_RECENTRE_SECONDS &&
			vr.gamepad_recentre_time - seconds < GAMEPAD_RECENTRE_SECONDS)
		{
			recentre();
		}
	}
	else
	{
		vr.gamepad_recentre_time = 0.0f;
	}
	/* (on a vehicle's handlebars the view turns with the vehicle: the stick
	turns nothing, and both grips held are the bars, not a recentring) */
	if (handlebars_active())
		x = 0.0f;
	/* (driving a vehicle steered toward the aim: the left stick's sideways
	push turns smoothly too, STICK_DRIVE_FASTEST) */
	if (seat_drives())
		vr.body_yaw -= stick_steer_push(vr.input.left_stick[0]) * STICK_DRIVE_FASTEST * seconds;
	if (settings.smooth_turn)
	{
		if (fabsf(x) > 0.2f)
			vr.body_yaw -= (x - (x > 0.0f ? 0.2f : -0.2f)) / 0.8f * settings.smooth_speed * seconds;
	}
	else if (!vr.snap_latched && fabsf(x) > 0.7f)
	{
		vr.body_yaw -= x > 0.0f ? settings.snap_angle : -settings.snap_angle;
		vr.snap_latched = 1;
	}
	else if (vr.snap_latched && fabsf(x) < 0.3f)
	{
		vr.snap_latched = 0;
	}
	/* (not while the left hand holds a long gun's foregrip, nor while
	either grip holds a vehicle to lift it, nor on a Ghost's handlebars) */
	if (vr.input.left_grip > 0.8f && vr.input.right_grip > 0.8f && !vr.two_handed && !vr.flip_grip[0] &&
		!vr.flip_grip[1] && !CLIMB_BUSY(0) && !CLIMB_BUSY(1) && !seat_steers() && !seat_held())
	{
		vr.recentre_time += seconds;
		if (vr.recentre_time >= RECENTRE_SECONDS && vr.recentre_time - seconds < RECENTRE_SECONDS)
			recentre();
	}
	else
	{
		vr.recentre_time = 0.0f;
	}
}

/* debug.gpu_stats: each frame's time from one wait's end to the next's (as
the game sees it), the part of it spent waiting for the runtime (the rest is
the game's), and the frames that took more than one display period */
static void timing_update(void)
{
	Uint64 now = SDL_GetTicksNS();

	if (vr.frame_start)
	{
		double frame = (double)(now - vr.frame_start) * 1e-9;

		vr.frame_seconds += frame;
		vr.wait_seconds += (double)(now - vr.wait_start) * 1e-9;
		if (frame > vr.worst_seconds)
			vr.worst_seconds = frame;
		if (vr.views.display_period > 0.0f && frame > vr.views.display_period * 1.5f)
			vr.late_frames++;
		if (++vr.timed_frames == 300)
		{
			platform_log("vr: %.2f ms a frame (period %.2f ms), %.2f ms of it the game's, worst %.2f ms, %lu of %lu late",
				vr.frame_seconds * 1000.0 / vr.timed_frames, vr.views.display_period * 1000.0f,
				(vr.frame_seconds - vr.wait_seconds) * 1000.0 / vr.timed_frames, vr.worst_seconds * 1000.0,
				vr.late_frames, vr.timed_frames);
			vr.timed_frames = vr.late_frames = 0;
			vr.frame_seconds = vr.wait_seconds = vr.worst_seconds = 0.0;
		}
	}
	vr.frame_start = now;
}

/* debug.vr_test_jitter: the head moved and turned a little, differently
each frame, as a worn headset's is (by up to this many millimetres, and as
many hundredths of a degree about each axis): for automated tests of what
flickers when the view moves by so little, with the headset still */
static float jitter_random(void)
{
	/* (xorshift: from -1 to 1) */
	static unsigned int state = 0x9e3779b9u;

	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return (float)(state >> 8) / (float)(1u << 23) - 1.0f;
}

static void jitter_new(void)
{
	float half_angles[3];
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		vr.jitter_offset[axis] = settings.test_jitter * jitter_random();
		/* (0.01 degree a millimetre, halved for the quaternion) */
		half_angles[axis] = settings.test_jitter * 1000.0f * 0.01f * (float)M_PI / 180.0f * 0.5f *
			jitter_random();
	}
	vr.jitter_turn[0] = half_angles[0];
	vr.jitter_turn[1] = half_angles[1];
	vr.jitter_turn[2] = half_angles[2];
	vr.jitter_turn[3] = 1.0f;
	{
		float length = sqrtf(vr.jitter_turn[0] * vr.jitter_turn[0] + vr.jitter_turn[1] * vr.jitter_turn[1] +
			vr.jitter_turn[2] * vr.jitter_turn[2] + 1.0f);

		for (axis = 0; axis < 4; axis++)
			vr.jitter_turn[axis] /= length;
	}
}

/* debug.vr_test_head: the head where it was first located, level (or
pitched as it says), its yaw a sine of the frames' display times (a
simulated headset's head wobbles) */
static void test_head_apply(void)
{
	struct vr_host_pose *eyes = vr.views.eye;
	float half_ipd[3], v[3], turn[4], yaw = 0.0f;
	int eye, axis;

	if (!settings.test_head || !eyes[0].valid || !eyes[1].valid)
		return;
	for (axis = 0; axis < 3; axis++)
		v[axis] = (eyes[1].position[axis] - eyes[0].position[axis]) * 0.5f;
	if (!vr.test_head_placed)
	{
		for (axis = 0; axis < 3; axis++)
			vr.test_head_centre[axis] = (eyes[0].position[axis] + eyes[1].position[axis]) * 0.5f;
		vr.test_head_placed = 1;
	}
	if (settings.test_head_period > 0.0f)
		yaw = settings.test_head_yaw * (float)M_PI / 180.0f *
			(float)sin(2.0 * M_PI * vr.test_head_seconds / settings.test_head_period);
	turn[0] = turn[2] = 0.0f;
	turn[1] = sinf(yaw * 0.5f);
	turn[3] = cosf(yaw * 0.5f);
	if (settings.test_head_pitch != 0.0f)
	{
		/* (about the head's own right, after its turn) */
		float half = settings.test_head_pitch * (float)M_PI / 180.0f * 0.5f;
		float pitch[4] = { sinf(half), 0.0f, 0.0f, cosf(half) };

		quaternion_multiply(turn, pitch, turn);
	}
	if (settings.test_head_roll != 0.0f)
	{
		/* (about the head's own forward, after its pitch: the right ear
		down) */
		float half = -settings.test_head_roll * (float)M_PI / 180.0f * 0.5f;
		float roll[4] = { 0.0f, 0.0f, sinf(half), cosf(half) };

		quaternion_multiply(turn, roll, turn);
	}
	v[0] = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	v[1] = v[2] = 0.0f;
	quaternion_rotate(turn, v, half_ipd);
	for (eye = 0; eye < 2; eye++)
	{
		for (axis = 0; axis < 3; axis++)
			eyes[eye].position[axis] = vr.test_head_centre[axis] + (eye ? half_ipd[axis] : -half_ipd[axis]);
		memcpy(eyes[eye].orientation, turn, sizeof(turn));
	}
	memcpy(vr.views.head.position, vr.test_head_centre, sizeof(vr.test_head_centre));
	memcpy(vr.views.head.orientation, turn, sizeof(turn));
}

/* debug.vr_test_fov: each eye's field of view as the setting says, and
each eye turned outward by its angle (as a headset with canted displays
has them), for automated tests of what the edges of a wide view show */
static void test_fov_apply(void)
{
	float angles[5];
	int eye, side;

	if (!settings.test_fov)
		return;
	/* (short of 90 degrees, where the tangents are) */
	for (side = 0; side < 5; side++)
		angles[side] = settings.test_fov_angles[side] < -85.0f ? -85.0f : settings.test_fov_angles[side] > 85.0f ? 85.0f :
			settings.test_fov_angles[side];
	for (eye = 0; eye < 2; eye++)
	{
		float outward = tanf(angles[0] * (float)M_PI / 180.0f), inward = tanf(angles[1] * (float)M_PI / 180.0f);
		float turn[4], cant = (eye ? -angles[4] : angles[4]) * (float)M_PI / 180.0f;

		vr.views.fov[eye][0] = eye ? -inward : -outward;
		vr.views.fov[eye][1] = eye ? outward : inward;
		vr.views.fov[eye][2] = tanf(angles[2] * (float)M_PI / 180.0f);
		vr.views.fov[eye][3] = -tanf(angles[3] * (float)M_PI / 180.0f);
		if (cant != 0.0f)
		{
			/* (about the eye's own up: the left eye turned left) */
			turn[0] = turn[2] = 0.0f;
			turn[1] = sinf(cant * 0.5f);
			turn[3] = cosf(cant * 0.5f);
			quaternion_multiply(vr.views.eye[eye].orientation, turn, vr.views.eye[eye].orientation);
		}
	}
}

/* the located views with the frame's jitter: the eyes turned about the
head's centre, and all of it moved */
static void jitter_apply(void)
{
	struct vr_host_pose *poses[3] = { &vr.views.eye[0], &vr.views.eye[1], &vr.views.head };
	float centre[3], v[3], turned[3];
	int pose, axis;

	if (settings.test_jitter <= 0.0f)
		return;
	for (axis = 0; axis < 3; axis++)
		centre[axis] = (vr.views.eye[0].position[axis] + vr.views.eye[1].position[axis]) * 0.5f;
	for (pose = 0; pose < 3; pose++)
	{
		for (axis = 0; axis < 3; axis++)
			v[axis] = poses[pose]->position[axis] - centre[axis];
		quaternion_rotate(vr.jitter_turn, v, turned);
		for (axis = 0; axis < 3; axis++)
			poses[pose]->position[axis] = centre[axis] + turned[axis] + vr.jitter_offset[axis];
		quaternion_multiply(vr.jitter_turn, poses[pose]->orientation, poses[pose]->orientation);
	}
}

/* debug.vr_test_hands: the controllers held where the setting says, from
the recentred head, each of its poses in turn (for automated tests of the
hands: a simulated headset has none), and with input, its grips, trigger,
stick and buttons, as the runtime gives them (before hands_assign). A
left-hander's (vr.dominant_hand), mirrored: its right hand is the weapon's,
the left controller, and its left the right controller, each where the other
would be across the body (right and left, yaw and roll the other way, the
head's turn and its move across too), and its grips, trigger, stick and
stick clicks are those of the controllers and sticks that do what it names */
static void test_hands_apply(int input)
{
	static int last_index = -1;
	const float *pose, *before;
	float moved[12];
	float mirror = settings.left_handed ? -1.0f : 1.0f;
	float *weapon_trigger = settings.left_handed ? &vr.input.left_trigger : &vr.input.right_trigger;
	float *other_trigger = settings.left_handed ? &vr.input.right_trigger : &vr.input.left_trigger;
	float *weapon_grip = settings.left_handed ? &vr.input.left_grip : &vr.input.right_grip;
	float *other_grip = settings.left_handed ? &vr.input.right_grip : &vr.input.left_grip;
	int *weapon_touch = settings.left_handed ? &vr.input.left_trigger_touch : &vr.input.right_trigger_touch;
	float *move_stick = settings.swap_sticks ? vr.input.right_stick : vr.input.left_stick;
	float *turn_stick = settings.swap_sticks ? vr.input.left_stick : vr.input.right_stick;
	unsigned long frame;
	int hand, index;

	if (!settings.test_hand_poses || !vr.origin_valid || !settings.test_hands_frames)
		return;
	/* (the pose this frame is in, and how far into it) */
	frame = vr.presents % settings.test_hands_frames;
	for (index = 0; index < settings.test_hand_poses - 1 && frame >= (unsigned long)settings.test_hands[index][16];
		index++)
	{
		frame -= (unsigned long)settings.test_hands[index][16];
	}
	if (index != last_index)
	{
		platform_log("vr: test hands: pose %d from frame %lu", index, vr.presents);
		last_index = index;
	}
	pose = settings.test_hands[index];
	/* a pose with move frames: the hands from the pose before to it over
	them, a step a frame (a swing, at the speed the steps make) */
	before = settings.test_hands[index ? index - 1 : settings.test_hand_poses - 1];
	memcpy(moved, pose, sizeof(moved));
	if (pose[17] >= 1.0f && (float)frame < pose[17])
	{
		float t = (float)(frame + 1) / pose[17];
		int part;

		for (part = 0; part < 12; part++)
			moved[part] = before[part] + (pose[part] - before[part]) * t;
	}
	for (hand = 0; hand < 2; hand++)
	{
		const float *p = moved + hand * 6;
		float radians = (float)M_PI / 180.0f;
		float local[3] = { p[0] * mirror, p[1], -p[2] };
		float yaw[4] = { 0.0f, sinf((vr.origin_yaw + p[3] * mirror * radians) * 0.5f), 0.0f,
			cosf((vr.origin_yaw + p[3] * mirror * radians) * 0.5f) };
		float pitch[4] = { sinf(p[4] * radians * 0.5f), 0.0f, 0.0f, cosf(p[4] * radians * 0.5f) };
		float roll[4] = { 0.0f, 0.0f, -sinf(p[5] * mirror * radians * 0.5f), cosf(p[5] * mirror * radians * 0.5f) };
		float heading[4] = { 0.0f, sinf(vr.origin_yaw * 0.5f), 0.0f, cosf(vr.origin_yaw * 0.5f) };
		struct vr_host_pose held;
		float offset[3];
		int axis;

		quaternion_rotate(heading, local, offset);
		for (axis = 0; axis < 3; axis++)
			held.position[axis] = vr.origin_position[axis] + offset[axis];
		quaternion_multiply(yaw, pitch, held.orientation);
		quaternion_multiply(held.orientation, roll, held.orientation);
		/* (a hand 10 metres or more out to the side is not tracked) */
		held.valid = fabsf(p[0]) < 10.0f;
		vr.views.aim[hand_controller(hand)] = held;
		vr.views.grip[hand_controller(hand)] = held;
	}
	/* (both written, the controllers' poses: hands_assign_views) */
	vr.views_unassigned = 1;
	if (input)
	{
		*other_grip = pose[12];
		/* (the left stick and the right grip held through the pose: driving,
		a vehicle's handlebars) */
		if (pose[22] != 0.0f || pose[23] != 0.0f)
		{
			move_stick[0] = pose[22];
			move_stick[1] = pose[23];
		}
		if (pose[24] > *weapon_grip)
			*weapon_grip = pose[24];
		/* (a pull less than 0: the finger off the trigger, which senses touch) */
		*weapon_trigger = pose[14] > 0.0f ? pose[14] : 0.0f;
		*weapon_touch = pose[14] >= 0.0f;
		vr.input.active[0] = vr.input.active[1] = 1;
	}
	if (pose[15] != 0.0f)
	{
		/* the head turned about its middle, by the pose's yaw */
		struct vr_host_pose *poses[3] = { &vr.views.eye[0], &vr.views.eye[1], &vr.views.head };
		float half = pose[15] * mirror * (float)M_PI / 180.0f * 0.5f;
		float turn[4] = { 0.0f, sinf(half), 0.0f, cosf(half) };
		float centre[3], v[3], turned[3];
		int which, axis;

		for (axis = 0; axis < 3; axis++)
			centre[axis] = (vr.views.eye[0].position[axis] + vr.views.eye[1].position[axis]) * 0.5f;
		for (which = 0; which < 3; which++)
		{
			for (axis = 0; axis < 3; axis++)
				v[axis] = poses[which]->position[axis] - centre[axis];
			quaternion_rotate(turn, v, turned);
			for (axis = 0; axis < 3; axis++)
				poses[which]->position[axis] = centre[axis] + turned[axis];
			quaternion_multiply(turn, poses[which]->orientation, poses[which]->orientation);
		}
	}
	{
		/* the head nodded about the neck, moved in the room and lowered, as
		a crouch (with the hands, over the move frames) */
		struct vr_host_pose *poses[3] = { &vr.views.eye[0], &vr.views.eye[1], &vr.views.head };
		float t = pose[17] >= 1.0f && (float)frame < pose[17] ? (float)(frame + 1) / pose[17] : 1.0f;
		float down = before[18] + (pose[18] - before[18]) * t;
		float pitch = (before[19] + (pose[19] - before[19]) * t) * (float)M_PI / 180.0f;
		float local[3] = { (before[21] + (pose[21] - before[21]) * t) * mirror, 0.0f,
			-(before[20] + (pose[20] - before[20]) * t) };
		float heading[4] = { 0.0f, sinf(vr.origin_yaw * 0.5f), 0.0f, cosf(vr.origin_yaw * 0.5f) };
		float neck[3], right[3], nod[4], moved_by[3], v[3], turned[3];
		static const float x_axis[3] = { 1.0f, 0.0f, 0.0f };
		int which, axis;

		/* (about the head's own right, through the neck behind and below
		the eyes as the head is turned) */
		v[0] = 0.0f;
		v[1] = -TEST_HEAD_NECK_DOWN;
		v[2] = TEST_HEAD_NECK_BACK;
		quaternion_rotate(vr.views.head.orientation, v, neck);
		quaternion_rotate(vr.views.head.orientation, x_axis, right);
		for (axis = 0; axis < 3; axis++)
			neck[axis] += vr.views.head.position[axis];
		for (axis = 0; axis < 3; axis++)
			nod[axis] = right[axis] * sinf(pitch * 0.5f);
		nod[3] = cosf(pitch * 0.5f);
		quaternion_rotate(heading, local, moved_by);
		for (which = 0; which < 3; which++)
		{
			for (axis = 0; axis < 3; axis++)
				v[axis] = poses[which]->position[axis] - neck[axis];
			quaternion_rotate(nod, v, turned);
			for (axis = 0; axis < 3; axis++)
				poses[which]->position[axis] = neck[axis] + turned[axis] + moved_by[axis];
			quaternion_multiply(nod, poses[which]->orientation, poses[which]->orientation);
			poses[which]->position[1] -= down;
		}
	}
	if (input && frame < TEST_HANDS_PRESS_FRAMES)
	{
		unsigned int buttons = (unsigned int)pose[13];
		unsigned int clicks = VR_BUTTON_LEFT_STICK | VR_BUTTON_RIGHT_STICK;

		/* (the move's stick's click and the turn's, on their sticks) */
		if (settings.swap_sticks && ((buttons & clicks) == VR_BUTTON_LEFT_STICK ||
			(buttons & clicks) == VR_BUTTON_RIGHT_STICK))
		{
			buttons ^= clicks;
		}
		vr.input.buttons |= buttons & 0xffff;
		if (buttons & TEST_HANDS_RIGHT_TRIGGER)
		{
			*weapon_trigger = 1.0f;
			*weapon_touch = 1;
		}
		if (buttons & TEST_HANDS_LEFT_TRIGGER)
			*other_trigger = 1.0f;
		if (buttons & TEST_HANDS_RIGHT_GRIP)
			*weapon_grip = 1.0f;
		if (buttons & TEST_HANDS_RIGHT_STICK_DOWN)
			turn_stick[1] = -1.0f;
	}
}

int halo_vr_test_hands(void)
{
	return settings.test_hand_poses > 0;
}

int halo_vr_left_handed(void)
{
	return vr.initialized && settings.left_handed;
}

/* debug.vr_test_target: the right controller, held by debug.vr_test_hands,
pointed along the direction the game gives (at a target it placed), its
roll level; after test_hands_apply */
static void test_point_apply(void)
{
	float forward[3], up[3] = { 0.0f, 1.0f, 0.0f }, back[3], right[3], along = 0.0f;
	int axis;

	if (!vr.test_point_valid || !settings.test_hand_poses || !vr.views.aim[1].valid)
		return;
	play_axes(vr.test_point, vr.origin_yaw, forward);
	normalize(forward);
	for (axis = 0; axis < 3; axis++)
		along += up[axis] * forward[axis];
	for (axis = 0; axis < 3; axis++)
	{
		up[axis] -= forward[axis] * along;
		back[axis] = -forward[axis];
	}
	normalize(up);
	right[0] = up[1] * back[2] - up[2] * back[1];
	right[1] = up[2] * back[0] - up[0] * back[2];
	right[2] = up[0] * back[1] - up[1] * back[0];
	quaternion_from_axes(right, up, back, vr.views.aim[1].orientation);
	memcpy(vr.views.grip[1].orientation, vr.views.aim[1].orientation, sizeof(vr.views.grip[1].orientation));
}

void halo_vr_test_point(const float direction[3])
{
	vr.test_point_valid = direction != NULL;
	if (direction)
		memcpy(vr.test_point, direction, sizeof(vr.test_point));
}

/* debug.vr_test_shake: a random number of unit variance, of its own (the
head's jitter keeps its sequence) */
static float shake_random(void)
{
	static unsigned int state = 0x2545f491u;

	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return ((float)(state >> 8) / (float)(1u << 23) - 1.0f) * 1.7320508f;
}

/* debug.vr_test_shake, as a frame begins: this frame's shake of the hands,
a slow wander and a tremor, half of it each (by its RMS) */
static void shake_new(float seconds)
{
	float keep = expf(-2.0f * (float)M_PI * SHAKE_WANDER_HZ * seconds), add = sqrtf(1.0f - keep * keep);
	float half = 0.70710678f;
	int axis, hand;

	if (settings.test_shake <= 0.0f && settings.test_shake_place <= 0.0f)
		return;
	if (vr.shake_time == 0.0)
	{
		vr.shake_phase[0] = shake_random();
		vr.shake_phase[1] = shake_random();
	}
	vr.shake_time += seconds;
	for (axis = 0; axis < 2; axis++)
	{
		float tremor = sinf(2.0f * (float)M_PI * SHAKE_TREMOR_HZ * (float)vr.shake_time + vr.shake_phase[axis]);

		vr.shake_wander[axis] = vr.shake_wander[axis] * keep + add * settings.test_shake * half * shake_random();
		vr.shake_turn[axis] = vr.shake_wander[axis] + settings.test_shake * half * 1.41421356f * tremor;
	}
	for (hand = 0; hand < 2; hand++)
	{
		for (axis = 0; axis < 3; axis++)
		{
			vr.shake_wander_place[hand][axis] = vr.shake_wander_place[hand][axis] * keep +
				add * settings.test_shake_place * shake_random();
			vr.shake_offset[hand][axis] = vr.shake_wander_place[hand][axis];
		}
	}
}

/* debug.vr_test_shake: the frame's shake on the controllers (the right one
turned about the play space's up and its own right, both moved); after
test_point_apply */
static void shake_apply(void)
{
	static const float upward[3] = { 0.0f, 1.0f, 0.0f }, rightward[3] = { 1.0f, 0.0f, 0.0f };
	struct vr_host_pose *poses[2][2] = { { &vr.views.aim[0], &vr.views.grip[0] }, { &vr.views.aim[1], &vr.views.grip[1] } };
	int hand, pose, axis;

	if (settings.test_shake <= 0.0f && settings.test_shake_place <= 0.0f)
		return;
	for (hand = 0; hand < 2; hand++)
	{
		for (pose = 0; pose < 2; pose++)
		{
			struct vr_host_pose *p = poses[hand][pose];

			if (!p->valid)
				continue;
			for (axis = 0; axis < 3; axis++)
				p->position[axis] += vr.shake_offset[hand][axis];
			if (hand == 1)
			{
				float right[3], yaw[4], pitch[4];
				float half_yaw = vr.shake_turn[0] * (float)M_PI / 180.0f * 0.5f;
				float half_pitch = vr.shake_turn[1] * (float)M_PI / 180.0f * 0.5f;

				quaternion_rotate(p->orientation, rightward, right);
				for (axis = 0; axis < 3; axis++)
				{
					yaw[axis] = upward[axis] * sinf(half_yaw);
					pitch[axis] = right[axis] * sinf(half_pitch);
				}
				yaw[3] = cosf(half_yaw);
				pitch[3] = cosf(half_pitch);
				quaternion_multiply(pitch, p->orientation, p->orientation);
				quaternion_multiply(yaw, p->orientation, p->orientation);
			}
		}
	}
}

/* The aim assist (vr.aim_assist), after the aim is steadied: as the game
aims, a target near the aim that its magnetism would be drawn to (as a
gamepad's aim is: halo_vr_aim_assist, vr_aim.c) makes the aim slow to leave
it (sticky): the hand's turn, less as much of it as follows the target's
(its middle's, as it moves, seen from the hand), as it takes the aim away
from the target (not as it comes to it, nor along its body), slowed by
ASSIST_FRICTION, the more the nearer the target is to the aim (its magnetism
level); and zoomed, the aim is drawn gently toward it. "The target" is the
point of the part of its body in sight from the hand (from its pelvis to its
head: of a Shade's gunner, the head over the gun) nearest the aim: a head
aimed at stays a head, and the aim is not drawn off it to a middle hidden
behind the turret it sits in (vr_aim.c finds the part in sight: none of it,
a Banshee's pilot in its cockpit, no target). (Not the point the game's
autoaim aims at on it: that point is on the line from the game's eye, and
seen from the hand it moves as the aim does, so that the aim chased it off
the target.) The aim may drift off the hand's line so, never more than
ASSIST_MOST degrees, and away from a target it comes back to it by half the
hand's own turn, which hides it: the aim never moves against the hand, nor
without it but for the zoomed pull (with the hand still, a target lost, as
a reload, a melee or a kill loses it, leaves the aim where it was). The reticle, the smart-link's view and the shot all
follow the assisted aim; the first-person weapon stays in the hand. */
static void aim_assist_step(const struct aim_steady *before, struct aim_steady *state, float seconds)
{
	float offset[3], turn[3], length = 0.0f, along = 0.0f, most = ASSIST_MOST * (float)M_PI / 180.0f;
	float strength = settings.aim_assist * vr.assist_level * (1.0f + ASSIST_TWO_HANDED * vr.two_handed);
	int axis;

	/* (the offset on the plane square to the aim, as the angle between
	them: taken as their difference, each frame's normalising shrank it, and
	a still hand's aim crept back toward its line) */
	{
		float cosine = before->assisted[0] * before->out[0] + before->assisted[1] * before->out[1] +
			before->assisted[2] * before->out[2];

		for (axis = 0; axis < 3; axis++)
		{
			offset[axis] = cosine > 0.5f ? before->assisted[axis] / cosine - before->out[axis] : 0.0f;
			turn[axis] = state->out[axis] - before->out[axis];
		}
	}
	state->target = strength > 0.0f ? vr.assist_target : -1;
	memcpy(state->target_middle, vr.assist_middle, sizeof(state->target_middle));
	if (state->target != -1)
	{
		float friction = ASSIST_FRICTION * strength, toward[3], hand = 0.0f, with = 0.0f, near = 0.0f, away = 0.0f;

		if (friction > ASSIST_FRICTION_MOST)
			friction = ASSIST_FRICTION_MOST;
		/* (the target's turn counts only along the hand's, and no more than
		it: the hand following a moving target is not slowed, and a still
		hand's aim stays still) */
		for (axis = 0; axis < 3; axis++)
		{
			hand += turn[axis] * turn[axis];
			if (before->target == state->target)
				with += (state->target_middle[axis] - before->target_middle[axis]) * turn[axis];
		}
		hand = sqrtf(hand);
		with = hand > 0.0f ? with / hand : 0.0f;
		with = with < 0.0f ? 0.0f : with > hand ? hand : with;
		/* (only as it takes the aim away from the target, the nearest point
		of the part of its body in sight: coming to it is not slowed, leaving
		it is, nor moving along it, from its feet to its head) */
		{
			float span[3], from[3], length_squared = 0.0f, t = 0.0f;

			for (axis = 0; axis < 3; axis++)
			{
				span[axis] = vr.assist_high[axis] - vr.assist_low[axis];
				from[axis] = state->out[axis] + offset[axis] - vr.assist_low[axis];
				length_squared += span[axis] * span[axis];
				t += from[axis] * span[axis];
			}
			t = length_squared > 1e-10f ? t / length_squared : 0.0f;
			t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
			for (axis = 0; axis < 3; axis++)
				toward[axis] = vr.assist_low[axis] + span[axis] * t;
			normalize(toward);
		}
		for (axis = 0; axis < 3; axis++)
		{
			toward[axis] -= state->out[axis] + offset[axis];
			near += toward[axis] * toward[axis];
		}
		near = sqrtf(near);
		for (axis = 0; axis < 3 && near > 1e-5f && hand > 0.0f; axis++)
			away -= turn[axis] * (1.0f - with / hand) * toward[axis] / near;
		for (axis = 0; axis < 3 && away > 0.0f; axis++)
			offset[axis] += toward[axis] / near * away * friction;
		if (state->magnification > 1.01f && near > 1e-5f)
		{
			float step = ASSIST_PULL * (float)M_PI / 180.0f * strength * seconds;

			step = step < near ? step / near : 1.0f;
			for (axis = 0; axis < 3; axis++)
				offset[axis] += toward[axis] * step;
		}
	}
	else
	{
		float back, hand = 0.0f;

		for (axis = 0; axis < 3; axis++)
		{
			length += offset[axis] * offset[axis];
			hand += turn[axis] * turn[axis];
		}
		length = sqrtf(length);
		/* (only as the hand turns: a still hand's aim stays still, whatever
		the weapon's animations or the target do) */
		back = 0.5f * sqrtf(hand);
		for (axis = 0; axis < 3; axis++)
			offset[axis] *= length > back ? (length - back) / length : 0.0f;
	}
	/* (across the aim, and no farther than ASSIST_MOST) */
	for (axis = 0; axis < 3; axis++)
		along += offset[axis] * state->out[axis];
	length = 0.0f;
	for (axis = 0; axis < 3; axis++)
	{
		offset[axis] -= state->out[axis] * along;
		length += offset[axis] * offset[axis];
	}
	length = sqrtf(length);
	for (axis = 0; axis < 3; axis++)
	{
		if (length > most)
			offset[axis] *= most / length;
		state->assisted[axis] = state->out[axis] + offset[axis];
	}
	normalize(state->assisted);
}

void halo_vr_aim_assist(long target, float level, const float middle[3], const float low[3], const float high[3])
{
	vr.assist_target = middle && low && high && level > 0.0f ? target : -1;
	vr.assist_level = level > 1.0f ? 1.0f : level;
	if (vr.assist_target != -1)
	{
		play_axes(middle, vr.origin_yaw, vr.assist_middle);
		normalize(vr.assist_middle);
		play_axes(low, vr.origin_yaw, vr.assist_low);
		normalize(vr.assist_low);
		play_axes(high, vr.origin_yaw, vr.assist_high);
		normalize(vr.assist_high);
	}
}

/* The aim's direction (OpenXR axes), steadied while the smart-link
magnifies it (halo_vr_aim_zoom), else as it is. Zoomed, the view on the
visor magnifies the hand's shake as much as the world: steadied by a
one-euro filter, whose cutoff is low while the hand is about still (a
tremor's back and forth, a slow wander) and rises as it turns, so that a
deliberate swing passes through promptly: STEADY_HZ over the magnification
less one, less still with the left hand on the foregrip, plus STEADY_GAIN for
each degree a second of the hand's turn. The reticle, the view on the visor
and the game's aim all follow it (halo_vr_aim, halo_vr_view): the shot goes
where the steadied reticle is. The first-person weapon stays in the hand.
Each frame steps it once from the last frame's state, however often its
poses are steadied (halo_vr_aim's as the game aims, then halo_vr_view's,
located again for the draws: the later replaces the earlier). */
static void aim_steady(float forward[3])
{
	float seconds = vr.views.display_elapsed > 0.0f ? vr.views.display_elapsed : vr.views.display_period;
	struct aim_steady *state = &vr.steady;
	const struct aim_steady *before = &vr.steady_before;
	float t, speed = 0.0f, cutoff;
	int axis;

	if (vr.steady_frame != vr.presents)
	{
		/* (a frame or more not aimed: from the hand as it is) */
		vr.steady_before = vr.steady;
		vr.steady_before.valid = vr.steady.valid && vr.steady_frame + 1 == vr.presents;
		vr.steady_frame = vr.presents;
	}
	/* (a long frame, a hitch as an animation or a sound is first loaded,
	is taken as 0.1 s: not a reset, which put the aim back on the hand's
	line at once, the steadying's lag and the assist's offset gone, so that
	the reticle jumped by up to ASSIST_MOST with the hand still) */
	if (seconds > 0.1f)
		seconds = 0.1f;
	if (!before->valid || seconds <= 0.0f)
	{
		state->valid = 1;
		memcpy(state->raw, forward, sizeof(state->raw));
		memset(state->velocity, 0, sizeof(state->velocity));
		memcpy(state->out, forward, sizeof(state->out));
		memcpy(state->assisted, forward, sizeof(state->assisted));
		state->target = -1;
		state->magnification = vr.steady_zoom;
		return;
	}
	t = 1.0f - expf(-2.0f * (float)M_PI * STEADY_VELOCITY_HZ * seconds);
	for (axis = 0; axis < 3; axis++)
	{
		float velocity = (forward[axis] - before->raw[axis]) / seconds;

		state->velocity[axis] = before->velocity[axis] + (velocity - before->velocity[axis]) * t;
		speed += state->velocity[axis] * state->velocity[axis];
	}
	memcpy(state->raw, forward, sizeof(state->raw));
	state->magnification = before->magnification +
		(vr.steady_zoom - before->magnification) * (1.0f - expf(-seconds / STEADY_ZOOM_SECONDS));
	if (state->magnification < 1.01f && vr.steady_zoom <= 1.0f)
	{
		memcpy(state->out, forward, sizeof(state->out));
	}
	else
	{
		speed = sqrtf(speed) * 180.0f / (float)M_PI;
		cutoff = STEADY_HZ / (state->magnification > 1.01f ? state->magnification - 1.0f : 0.01f);
		if (vr.two_handed_weight > 0.5f)
			cutoff /= STEADY_TWO_HANDED;
		cutoff += STEADY_GAIN * speed;
		t = 1.0f - expf(-2.0f * (float)M_PI * cutoff * seconds);
		for (axis = 0; axis < 3; axis++)
			state->out[axis] = before->out[axis] + (forward[axis] - before->out[axis]) * t;
		normalize(state->out);
	}
	aim_assist_step(before, state, seconds);
	memcpy(forward, state->assisted, sizeof(state->assisted));
}

void halo_vr_aim_zoom(float magnification)
{
	vr.steady_zoom = magnification > 1.0f ? magnification : 1.0f;
}

/* the views and controllers as the runtime located them (before the tests
move them), and when */
static void latch_located(void)
{
	struct timespec now;
	unsigned long long nanoseconds;

	clock_gettime(CLOCK_MONOTONIC, &now);
	nanoseconds = (unsigned long long)now.tv_sec * 1000000000ull + (unsigned long long)now.tv_nsec;
	vr.latch.located_low = (unsigned int)nanoseconds;
	vr.latch.located_high = (unsigned int)(nanoseconds >> 32);
	memcpy(vr.latch.located_eye, vr.views.eye, sizeof(vr.latch.located_eye));
	memcpy(vr.latch.located_grip, vr.views.grip, sizeof(vr.latch.located_grip));
}

/* waits for the runtime's next frame and begins it; whether it did now */
static int frame_begin(void)
{
	if (!vr.initialized || vr.frame || vr.attempted)
		return 0;
	/* (once a present: until the session runs, each try waits a little) */
	vr.attempted = 1;
	if (vr.pending)
	{
		acquired_clear();
		vr.pending = 0;
	}
	settings_read();
	vr.wait_start = SDL_GetTicksNS();
	if (!host_vr_frame_wait(&vr.views))
		return 0;
	vr.frame = 1;
	if (vr.statistics)
		timing_update();
	if (vr.force_render)
		vr.views.should_render = 1;
	jitter_new();
	shake_new(vr.views.display_elapsed > 0.0f ? vr.views.display_elapsed : vr.views.display_period);
	vr.test_head_seconds += vr.views.display_elapsed > 0.0f ? vr.views.display_elapsed : vr.views.display_period;
	if (host_vr_locate(&vr.views))
	{
		latch_located();
		vr.views_unassigned = 1;
		test_head_apply();
		test_fov_apply();
		jitter_apply();
		if (!vr.origin_valid || (vr.views.focused && !vr.was_focused))
			recentre();
	}
	if (vr.views.head.valid)
		vr.was_focused = vr.views.focused;
	host_vr_input(&vr.input);
	test_hands_apply(1);
	hands_assign_views();
	hands_assign_input();
	test_point_apply();
	shake_apply();
	input_update();
	return 1;
}

/* The frame is begun (the runtime's wait) before the game reads its clock
(main.c), and the time from the last frame's display to this one's is the
frame's: the world moves on by as much as the headset shows it moving.
Read after a draw that ended wherever the game's work did, the clock
stepped the frames unevenly, 9 to 19 ms at 72 Hz, which the headset's even
frames showed as judder (the Pelicans' flights in). */
int halo_vr_frame_clock(float *seconds)
{
	if (!frame_begin() || vr.views.display_elapsed <= 0.0f)
		return 0;
	*seconds = vr.views.display_elapsed;
	return 1;
}

int halo_vr_frame_active(void)
{
	frame_begin();
	return vr.frame && vr.views.should_render;
}

int halo_vr_frame_waited(void)
{
	return vr.frame;
}

/* The frame's layers, which the host's render thread ends the frame with as
it plays the frame (xgpu_stream.h's VR_SUBMIT), while the game goes on: it
begins and ends the runtime's frames, the game waits for them. */
void halo_vr_present(int eyes_drawn, int hud_drawn, int scope_drawn)
{
	struct vr_host_layers *pending = &vr.pending_layers;

	vr.attempted = 0;
	vr.presents++;
	/* (the aim follows the head again only if the next update says so; the
	gamepad goes by this frame's) */
	vr.aimed = vr.aiming;
	vr.aiming = 0;
	vr.crouch_able_frame = vr.crouch_able;
	vr.crouch_able = 0;
	if (!vr.frame)
		return;
	/* (the host ends the frame with these layers, leaving out those whose
	image it did not acquire or draw: every image taken as acquired, the
	eyes' depth as written: port/linux/vulkan/host_vulkan_vr.c) */
	{
		int swapchain;

		for (swapchain = 0; swapchain < VR_SWAPCHAIN_COUNT; swapchain++)
			vr.acquired[swapchain] = 0;
		vr.depth_written = 1;
	}
	memset(pending, 0, sizeof(*pending));
	if (vr.views.should_render)
	{
		pending->projection = eyes_drawn && vr.acquired[VR_SWAPCHAIN_EYES] >= 0;
		memcpy(pending->eye, vr.views.eye, sizeof(pending->eye));
		memcpy(pending->fov, vr.views.fov, sizeof(pending->fov));
		pending->hud = hud_drawn && vr.acquired[VR_SWAPCHAIN_HUD] >= 0;
		pending->depth = pending->projection && vr.depth_written && vr.acquired[VR_SWAPCHAIN_DEPTH] >= 0 &&
			vr.depth_far > vr.depth_near;
		pending->near_z = vr.depth_near;
		pending->far_z = vr.depth_far;
		if (pending->projection && scope_drawn && vr.scope_set && vr.acquired[VR_SWAPCHAIN_SCOPE] >= 0)
		{
			/* the scope's lens, from the recentred head's game axes to the
			play space: its quad faces back along the scope (its +z), up as
			the scope's up */
			struct vr_host_pose *pose = &pending->scope_pose;
			float back[3], up[3], right[3], g[3];
			int axis;

			play_axes(vr.scope.position, vr.origin_yaw, pose->position);
			for (axis = 0; axis < 3; axis++)
			{
				pose->position[axis] += vr.origin_position[axis];
				g[axis] = -vr.scope.forward[axis];
			}
			play_axes(g, vr.origin_yaw, back);
			play_axes(vr.scope.up, vr.origin_yaw, up);
			normalize(back);
			/* (up made square to back) */
			{
				float along = up[0] * back[0] + up[1] * back[1] + up[2] * back[2];

				for (axis = 0; axis < 3; axis++)
					up[axis] -= back[axis] * along;
			}
			normalize(up);
			right[0] = up[1] * back[2] - up[2] * back[1];
			right[1] = up[2] * back[0] - up[0] * back[2];
			right[2] = up[0] * back[1] - up[1] * back[0];
			quaternion_from_axes(right, up, back, pose->orientation);
			pose->valid = 1;
			pending->scope = 1;
			pending->scope_eye = vr.scope.eye;
			pending->scope_size = vr.scope.size;
		}
		if (pending->projection && !vr.menus && vr_visor_enabled())
		{
			/* the HUD on the helmet's visor (vr_visor.c) */
			int panel;

			for (panel = 0; panel < VR_PANEL_COUNT; panel++)
				pending->panels[panel].shown = vr.acquired[VR_SWAPCHAIN_PANEL + panel] >= 0;
			vr_visor_layers(pending);
		}
		else if (pending->projection && !vr.menus)
		{
			/* the HUD before the eyes, where the HUD's camera looks */
			float width = 2.0f * settings.hud_distance * tanf(settings.hud_size * 0.5f * (float)M_PI / 180.0f);

			pending->hud_head_locked = 1;
			pending->hud_pose.orientation[3] = 1.0f;
			pending->hud_pose.position[2] = -settings.hud_distance;
			pending->hud_size[0] = width;
			pending->hud_size[1] = width * (float)HUD_HEIGHT / (float)HUD_WIDTH;
		}
		else
		{
			/* the menus, in front of where the head was at the recentring */
			float s = sinf(vr.origin_yaw * 0.5f), c = cosf(vr.origin_yaw * 0.5f);

			pending->hud_pose.orientation[1] = s;
			pending->hud_pose.orientation[3] = c;
			pending->hud_pose.position[0] = vr.origin_position[0] - sinf(vr.origin_yaw) * settings.menu_distance;
			pending->hud_pose.position[1] = settings.standing ? settings.player_height : vr.origin_position[1];
			pending->hud_pose.position[2] = vr.origin_position[2] - cosf(vr.origin_yaw) * settings.menu_distance;
			pending->hud_size[0] = settings.menu_width;
			pending->hud_size[1] = settings.menu_width * (float)HUD_HEIGHT / (float)HUD_WIDTH;
		}
	}
	vr.pending = 1;
	vr.frame = 0;
	vr.depth_written = 0;
	vr.scope_set = 0;
}

int halo_vr_submitted(struct vr_host_layers *layers, struct vr_host_pose *head)
{
	if (!vr.pending)
		return 0;
	*layers = vr.pending_layers;
	*head = vr.views.head;
	return 1;
}

void halo_vr_depth_range(float near_metres, float far_metres)
{
	vr.depth_near = near_metres;
	vr.depth_far = far_metres;
}

/* The late latch (vr.late_latch): the host's render thread locates the
head and the controllers again just before it draws the frame, and moves
the eyes' views (and the first-person weapon, with its controller) by as
much as they have moved since they were located here (xgpu_stream.h). The
game's work is the frame's as located; the picture is the newest. Not the
test head's (it is not the runtime's), nor the test hands' or pointing.
Off, the poses go to the host still, for the time from their locating to
the frame's submission (debug.gpu_stats). */
int halo_vr_latch(struct vr_host_latch *latch)
{
	if (!vr.frame || !vr.views.should_render || !vr.views.eye[0].valid || !vr.views.eye[1].valid)
		return 0;
	*latch = vr.latch;
	latch->head = settings.late_latch && !settings.test_head;
	/* (the weapon's controller: the runtime's left for a left-hander,
	hands_assign) */
	latch->hand = !settings.late_latch || settings.test_hand_poses || vr.test_point_valid ? -1 :
		settings.left_handed ? 0 : 1;
	memcpy(latch->eye, vr.views.eye, sizeof(latch->eye));
	return 1;
}

int halo_vr_late_latching(void)
{
	settings_read();
	return settings.late_latch && !settings.test_head;
}

int halo_vr_depth_wanted(void)
{
	return vr.initialized && vr.info.depth && vr.frame;
}

void halo_vr_depth_written(void)
{
	vr.depth_written = 1;
}

/* ---------- the views */

/* The head as located when the frame began (frame_begin, for the frame's
display), before halo_vr_view locates it again for the draws: the sounds'
listener is placed before the frame is drawn (sound_manager.c,
refresh_listener; vr_render.c, vr_render_listener), and this frame's head is
a frame newer than the last one drawn. */
int halo_vr_head(float forward[3], float up[3], float position[3])
{
	static const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	static const float above[3] = { 0.0f, 1.0f, 0.0f };
	float v[3], offset[3];
	int axis;

	if (!vr.frame || !vr.views.eye[0].valid || !vr.views.eye[1].valid)
		return 0;
	quaternion_rotate(vr.views.eye[0].orientation, ahead, v);
	game_axes(v, vr.origin_yaw, forward);
	quaternion_rotate(vr.views.eye[0].orientation, above, v);
	game_axes(v, vr.origin_yaw, up);
	for (axis = 0; axis < 3; axis++)
		offset[axis] = (vr.views.eye[0].position[axis] + vr.views.eye[1].position[axis]) * 0.5f -
			vr.origin_position[axis];
	game_axes(offset, vr.origin_yaw, position);
	return 1;
}

/* Rebase an already-smoothed direction without advancing its filter. */
static void tracking_direction_move(const float value[3], const float old_forward[3], const float old_up[3],
	const float forward[3], const float up[3], float out[3])
{
	float old_left[3], left[3], along = 0.0f, across = 0.0f, above = 0.0f;
	int axis;

	if (!memcmp(old_forward, forward, sizeof(float) * 3) && !memcmp(old_up, up, sizeof(float) * 3))
	{
		memcpy(out, value, sizeof(float) * 3);
		return;
	}
	for (axis = 0; axis < 3; axis++)
	{
		int next = (axis + 1) % 3, last = (axis + 2) % 3;

		old_left[axis] = old_up[next] * old_forward[last] - old_up[last] * old_forward[next];
		left[axis] = up[next] * forward[last] - up[last] * forward[next];
		along += value[axis] * old_forward[axis];
		across += value[axis] * old_left[axis];
		above += value[axis] * old_up[axis];
	}
	for (axis = 0; axis < 3; axis++)
		out[axis] = along * forward[axis] + across * left[axis] + above * up[axis];
}

/* Refresh tracking after long preparation, without sampling actions or
advancing aim/visor filters, presentation counters or the frame clock. */
int halo_vr_refresh_tracking(struct halo_vr_view *view)
{
	struct vr_host_views fresh = vr.views;
	static const float forward[3] = { 0.0f, 0.0f, -1.0f };
	static const float up[3] = { 0.0f, 1.0f, 0.0f };
	float centre[3], offset[3], v[3], inverse[4];
	float old_hand_forward[3], old_hand_up[3];
	int eye;
	struct halo_vr_view before = *view;

	if (!halo_vr_frame_active() || !host_vr_locate(&fresh) ||
		!fresh.eye[0].valid || !fresh.eye[1].valid)
		return 0;
	/* Public hand axes may have been replaced by a vehicle handlebar. */
	weapon_pointing(v, offset);
	game_axes(v, vr.origin_yaw, old_hand_forward);
	game_axes(offset, vr.origin_yaw, old_hand_up);
	vr.views.head = fresh.head;
	vr.views.gaze = fresh.gaze;
	memcpy(vr.views.aim, fresh.aim, sizeof(vr.views.aim));
	memcpy(vr.views.grip, fresh.grip, sizeof(vr.views.grip));
	vr.views_unassigned = 1;
	memcpy(vr.views.eye, fresh.eye, sizeof(vr.views.eye));
	memcpy(vr.views.fov, fresh.fov, sizeof(vr.views.fov));
	latch_located();
	test_head_apply();
	test_fov_apply();
	jitter_apply();
	test_hands_apply(0);
	hands_assign_views();
	test_point_apply();
	shake_apply();
	quaternion_rotate(vr.views.eye[0].orientation, forward, v);
	game_axes(v, vr.origin_yaw, view->head_forward);
	quaternion_rotate(vr.views.eye[0].orientation, up, v);
	game_axes(v, vr.origin_yaw, view->head_up);
	/* the head's centre, between the eyes */
	for (eye = 0; eye < 3; eye++)
	{
		centre[eye] = (vr.views.eye[0].position[eye] + vr.views.eye[1].position[eye]) * 0.5f;
		offset[eye] = centre[eye] - vr.origin_position[eye];
	}
	game_axes(offset, vr.origin_yaw, view->head_position);
	/* each eye from the centre, in the head's axes */
	inverse[0] = -vr.views.eye[0].orientation[0];
	inverse[1] = -vr.views.eye[0].orientation[1];
	inverse[2] = -vr.views.eye[0].orientation[2];
	inverse[3] = vr.views.eye[0].orientation[3];
	for (eye = 0; eye < 2; eye++)
	{
		float local[3];
		int axis;

		for (axis = 0; axis < 3; axis++)
			v[axis] = vr.views.eye[eye].position[axis] - centre[axis];
		quaternion_rotate(inverse, v, local);
		game_axes(local, 0.0f, view->eye_offset[eye]);
		memcpy(view->fov[eye], vr.views.fov[eye], sizeof(view->fov[eye]));
	}
	body_place(view->head_position, view->head_forward, view->head_up, view->body_neck, view->body_eyes);
	if (!view->hud_visor)
	{
		memcpy(view->hud_forward, view->head_forward, sizeof(view->hud_forward));
		memcpy(view->hud_up, view->head_up, sizeof(view->hud_up));
	}
	/* the right controller */
	view->controller_aim = settings.controller_aim && vr.views.aim[1].valid;
	if (view->controller_aim)
	{
		const struct vr_host_pose *hand = &vr.views.aim[1];
		float pointing_up[3];

		weapon_pointing(v, pointing_up);
		game_axes(v, vr.origin_yaw, view->hand_forward);
		game_axes(pointing_up, vr.origin_yaw, view->hand_up);
		for (eye = 0; eye < 3; eye++)
			offset[eye] = hand->position[eye] - vr.origin_position[eye];
		game_axes(offset, vr.origin_yaw, view->hand_position);
		/* Move the already-smoothed aim with tracking, without another filter step. */
		if (before.controller_aim)
			tracking_direction_move(before.aim_forward, old_hand_forward, old_hand_up,
				view->hand_forward, view->hand_up, view->aim_forward);
		else
			memcpy(view->aim_forward, view->hand_forward, sizeof(view->aim_forward));
	}
	/* the hands (vr.hands): where the controllers' grips are (else where
	they point from), and where the left one points */
	view->left_hand_valid = vr.views.aim[0].valid;
	if (view->floating_hands && view->controller_aim)
	{
		const struct vr_host_pose *grip = vr.views.grip[1].valid ? &vr.views.grip[1] : &vr.views.aim[1];

		for (eye = 0; eye < 3; eye++)
			offset[eye] = grip->position[eye] - vr.origin_position[eye];
		game_axes(offset, vr.origin_yaw, view->right_grip_position);
	}
	/* (and while it holds a grenade or a magazine, drawn in it whatever the
	hands) */
	if (view->left_hand_valid && (view->floating_hands || vr.throw_state != THROW_IDLE || vr.reload.held))
	{
		const struct vr_host_pose *hand = &vr.views.aim[0];
		const struct vr_host_pose *grip = vr.views.grip[0].valid ? &vr.views.grip[0] : hand;

		quaternion_rotate(hand->orientation, forward, v);
		game_axes(v, vr.origin_yaw, view->left_hand_forward);
		quaternion_rotate(hand->orientation, up, v);
		game_axes(v, vr.origin_yaw, view->left_hand_up);
		for (eye = 0; eye < 3; eye++)
			offset[eye] = grip->position[eye] - vr.origin_position[eye];
		game_axes(offset, vr.origin_yaw, view->left_grip_position);
	}
	/* (on a vehicle's handlebars: each hand holding its handle on it, along
	the vehicle (the player's axes are turned with it), its fingers about it) */
	view->handlebars = before.handlebars && view->controller_aim;
	if (view->handlebars)
	{
		const float *along = vr.seat.handle_forward, *upright = vr.seat.handle_up;

		if (vr.bars_held[1])
		{
			memcpy(view->right_grip_position, vr.seat.handles[hand_controller(1)], sizeof(view->right_grip_position));
			memcpy(view->hand_forward, along, sizeof(view->hand_forward));
			memcpy(view->hand_up, upright, sizeof(view->hand_up));
		}
		if (vr.bars_held[0])
		{
			view->left_hand_valid = 1;
			memcpy(view->left_grip_position, vr.seat.handles[hand_controller(0)], sizeof(view->left_grip_position));
			memcpy(view->left_hand_forward, along, sizeof(view->left_hand_forward));
			memcpy(view->left_hand_up, upright, sizeof(view->left_hand_up));
		}
	}

	if (halo_load_trace_enabled())
	{
		char detail[768];

		snprintf(detail, sizeof(detail),
			"old_head=%.9g,%.9g,%.9g new_head=%.9g,%.9g,%.9g raw_eye=%.9g,%.9g,%.9g raw_rotation=%.9g,%.9g,%.9g,%.9g latch=%u:%u seconds=%.9g",
			before.head_position[0], before.head_position[1], before.head_position[2],
			view->head_position[0], view->head_position[1], view->head_position[2],
			vr.latch.located_eye[0].position[0], vr.latch.located_eye[0].position[1], vr.latch.located_eye[0].position[2],
			vr.latch.located_eye[0].orientation[0], vr.latch.located_eye[0].orientation[1],
			vr.latch.located_eye[0].orientation[2], vr.latch.located_eye[0].orientation[3],
			vr.latch.located_high, vr.latch.located_low, view->seconds);
		halo_load_trace("loading.tracking.refresh", detail);
	}
	return 1;
}

int halo_vr_view(struct halo_vr_view *view)
{
	const struct vr_host_pose *head;
	float centre[3], offset[3];
	static const float forward[3] = { 0.0f, 0.0f, -1.0f };
	static const float up[3] = { 0.0f, 1.0f, 0.0f };
	float v[3];
	float inverse[4];
	int eye;

	if (!halo_vr_frame_active())
		return 0;
	/* late: the poses the frame is drawn and submitted with */
	if (host_vr_locate(&vr.views))
	{
		latch_located();
		vr.views_unassigned = 1;
		test_head_apply();
		test_fov_apply();
		jitter_apply();
		test_hands_apply(0);
		hands_assign_views();
		test_point_apply();
		shake_apply();
	}
	if (!vr.views.eye[0].valid)
		return 0;
	head = &vr.views.head;
	quaternion_rotate(vr.views.eye[0].orientation, forward, v);
	game_axes(v, vr.origin_yaw, view->head_forward);
	quaternion_rotate(vr.views.eye[0].orientation, up, v);
	game_axes(v, vr.origin_yaw, view->head_up);
	/* the head's centre, between the eyes */
	for (eye = 0; eye < 3; eye++)
	{
		centre[eye] = (vr.views.eye[0].position[eye] + vr.views.eye[1].position[eye]) * 0.5f;
		offset[eye] = centre[eye] - vr.origin_position[eye];
	}
	game_axes(offset, vr.origin_yaw, view->head_position);
	/* each eye from the centre, in the head's axes */
	inverse[0] = -vr.views.eye[0].orientation[0];
	inverse[1] = -vr.views.eye[0].orientation[1];
	inverse[2] = -vr.views.eye[0].orientation[2];
	inverse[3] = vr.views.eye[0].orientation[3];
	for (eye = 0; eye < 2; eye++)
	{
		float local[3];
		int axis;

		for (axis = 0; axis < 3; axis++)
			v[axis] = vr.views.eye[eye].position[axis] - centre[axis];
		quaternion_rotate(inverse, v, local);
		game_axes(local, 0.0f, view->eye_offset[eye]);
		memcpy(view->fov[eye], vr.views.fov[eye], sizeof(view->fov[eye]));
	}
	(void)head;
	view->body_yaw = vr.body_yaw;
	view->body_heading_valid = vr.body_heading_valid;
	view->body_heading = vr.body_heading;
	body_place(view->head_position, view->head_forward, view->head_up, view->body_neck, view->body_eyes);
	view->hud_tangent = tanf(settings.hud_size * 0.5f * (float)M_PI / 180.0f) * (float)HUD_HEIGHT / (float)HUD_WIDTH;
	/* the HUD's camera: where the head looks, or on the visor where the HUD
	does (a moment behind the head, vr.hud_lag) */
	memcpy(view->hud_forward, view->head_forward, sizeof(view->hud_forward));
	memcpy(view->hud_up, view->head_up, sizeof(view->hud_up));
	vr_visor_follow(&vr.views.head, vr.views.eye, vr.views.fov, vr.views.display_elapsed > 0.0f ? vr.views.display_elapsed :
		vr.views.display_period, settings.hud_distance, settings.hud_size);
	view->hud_visor = vr_visor_enabled();
	if (view->hud_visor)
	{
		float orientation[4];

		vr_visor_orientation(orientation);
		quaternion_rotate(orientation, forward, v);
		game_axes(v, vr.origin_yaw, view->hud_forward);
		quaternion_rotate(orientation, up, v);
		game_axes(v, vr.origin_yaw, view->hud_up);
		view->hud_half_angles[0] = settings.hud_size * 0.5f * (float)M_PI / 180.0f;
		view->hud_half_angles[1] = view->hud_half_angles[0] * (float)HUD_HEIGHT / (float)HUD_WIDTH;
		/* (in angles, a tangent at the middle: the reticle's size there) */
		view->hud_tangent = view->hud_half_angles[1];
	}
	view->world_units_per_metre = settings.world_scale / METRES_PER_WORLD_UNIT;
	/* the right controller */
	view->controller_aim = settings.controller_aim && vr.views.aim[1].valid;
	if (view->controller_aim)
	{
		const struct vr_host_pose *hand = &vr.views.aim[1];
		float pointing_up[3];

		weapon_pointing(v, pointing_up);
		game_axes(v, vr.origin_yaw, view->hand_forward);
		game_axes(pointing_up, vr.origin_yaw, view->hand_up);
		for (eye = 0; eye < 3; eye++)
			offset[eye] = hand->position[eye] - vr.origin_position[eye];
		game_axes(offset, vr.origin_yaw, view->hand_position);
		/* (the aim steadied while zoomed: aim_steady) */
		aim_steady(v);
		game_axes(v, vr.origin_yaw, view->aim_forward);
		if (settings.aim_log && settings.test_hand_poses)
		{
			float raw[3], along = v[0] * vr.steady.out[0] + v[1] * vr.steady.out[1] + v[2] * vr.steady.out[2];

			weapon_pointing(raw, pointing_up);
			platform_log("vr aim steady: frame %lu, zoom %.2f (eased %.2f)%s, the controller %.4f %.4f, the aim %.4f "
				"%.4f (degrees: yaw left, pitch up); assist %.2f, the aim %.3f degrees off the steadied", vr.presents,
				vr.steady_zoom, vr.steady.magnification, vr.two_handed_weight > 0.5f ? ", both hands" : "",
				atan2f(-raw[0], -raw[2]) * 180.0f / (float)M_PI, asinf(raw[1]) * 180.0f / (float)M_PI,
				atan2f(-v[0], -v[2]) * 180.0f / (float)M_PI, asinf(v[1]) * 180.0f / (float)M_PI,
				vr.steady.target != -1 ? vr.assist_level : 0.0f,
				acosf(along < 1.0f ? along : 1.0f) * 180.0f / (float)M_PI);
		}
	}
	memcpy(view->weapon_offset, settings.weapon_offset, sizeof(view->weapon_offset));
	/* the hands (vr.hands): where the controllers' grips are (else where
	they point from), and where the left one points */
	view->floating_hands = settings.floating_hands && settings.controller_aim;
	view->arms = settings.arms;
	view->left_hand_valid = vr.views.aim[0].valid;
	if (view->floating_hands && view->controller_aim)
	{
		const struct vr_host_pose *grip = vr.views.grip[1].valid ? &vr.views.grip[1] : &vr.views.aim[1];

		for (eye = 0; eye < 3; eye++)
			offset[eye] = grip->position[eye] - vr.origin_position[eye];
		game_axes(offset, vr.origin_yaw, view->right_grip_position);
	}
	/* (and while it holds a grenade or a magazine, drawn in it whatever the
	hands) */
	if (view->left_hand_valid && (view->floating_hands || vr.throw_state != THROW_IDLE || vr.reload.held))
	{
		const struct vr_host_pose *hand = &vr.views.aim[0];
		const struct vr_host_pose *grip = vr.views.grip[0].valid ? &vr.views.grip[0] : hand;

		quaternion_rotate(hand->orientation, forward, v);
		game_axes(v, vr.origin_yaw, view->left_hand_forward);
		quaternion_rotate(hand->orientation, up, v);
		game_axes(v, vr.origin_yaw, view->left_hand_up);
		for (eye = 0; eye < 3; eye++)
			offset[eye] = grip->position[eye] - vr.origin_position[eye];
		game_axes(offset, vr.origin_yaw, view->left_grip_position);
	}
	view->two_handed = vr.two_handed_weight * vr.two_handed_weight * (3.0f - 2.0f * vr.two_handed_weight);
	view->right_trigger = vr.input.right_trigger;
	view->right_trigger_touch = vr.trigger_touch;
	view->left_grip = vr.input.left_grip;
	/* (on a vehicle's handlebars: each hand holding its handle on it, along
	the vehicle (the player's axes are turned with it), its fingers about it) */
	view->handlebars = seat_hands() && view->floating_hands && view->controller_aim;
	if (view->handlebars)
	{
		const float *along = vr.seat.handle_forward, *upright = vr.seat.handle_up;

		if (vr.bars_held[1])
		{
			memcpy(view->right_grip_position, vr.seat.handles[hand_controller(1)], sizeof(view->right_grip_position));
			memcpy(view->hand_forward, along, sizeof(view->hand_forward));
			memcpy(view->hand_up, upright, sizeof(view->hand_up));
			view->right_trigger = 1.0f;
			view->right_trigger_touch = 1.0f;
		}
		if (vr.bars_held[0])
		{
			view->left_hand_valid = 1;
			memcpy(view->left_grip_position, vr.seat.handles[hand_controller(0)], sizeof(view->left_grip_position));
			memcpy(view->left_hand_forward, along, sizeof(view->left_hand_forward));
			memcpy(view->left_hand_up, upright, sizeof(view->left_hand_up));
			view->left_grip = 1.0f;
		}
	}
	memcpy(view->shoulder_offset, settings.shoulder_offset, sizeof(view->shoulder_offset));
	view->seconds = vr.views.display_elapsed > 0.0f ? vr.views.display_elapsed : vr.views.display_period;
	view->vehicle_first_person = settings.vehicle_first_person;
	view->gamepad_aim = settings.gamepad_aim;
	view->gamepad_pitch = settings.gamepad_aim && settings.gamepad_pitch;
	return 1;
}

/* ---------- aiming */

int halo_vr_aim(float current_yaw, int long_gun, float *yaw, float *pitch)
{
	static const float backwards[3] = { 0.0f, 0.0f, -1.0f };
	float v[3], f[3], head_yaw;

	if (!(vr.frame || vr.pending) || !vr.views.should_render || !vr.views.head.valid)
	{
		vr.aiming = 0;
		vr.aim_line_valid = 0;
		return 0;
	}
	if (settings.gamepad_aim)
	{
		/* the gamepad aims, as on the flat screen: the facing stays the
		game's, and the body (the view's heading before the head's turn)
		follows it: with it (vr.gamepad_view = "camera", "level"), or a step
		of vr.snap_turn_angle each time the aim is more than that from it
		("snap"), so the reticle and the weapon move about the view and the
		view turns in steps. (The view's pitch: vr_render.c.) */
		float off = remainderf(current_yaw - vr.body_yaw, 2.0f * (float)M_PI);
		float step = settings.snap_angle;

		if (!vr.aim_synced || !settings.gamepad_snap || step <= 0.0f || fabsf(off) > 2.0f * step)
			vr.body_yaw = current_yaw;
		else if (fabsf(off) > step)
			vr.body_yaw += off > 0.0f ? step : -step;
		vr.aim_synced = 1;
		*yaw = current_yaw;
		*pitch = 0.0f;
		vr.aiming = 1;
		return 2;
	}
	quaternion_rotate(vr.views.head.orientation, backwards, v);
	game_axes(v, vr.origin_yaw, f);
	head_yaw = atan2f(f[1], f[0]);
	if (!vr.aim_synced)
	{
		/* the turn that keeps the view where the player faced */
		vr.body_yaw = current_yaw - head_yaw;
		vr.aim_synced = 1;
	}
	vr.long_gun = long_gun;
	vr.aim_line_valid = 0;
	/* on a vehicle's handlebars: where the bars point, from between the
	handles, along the vehicle's heading (the view's) turned and pitched as
	the bars are; let go, the turn easing out, and no line (no reticle) */
	if (seat_steers())
	{
		/* (from the vehicle's heading as the game has it, not the view's,
		eased after it: the turn is the game's error) */
		float turn = remainderf(vr.seat.steer_heading - vr.body_yaw, 2.0f * (float)M_PI) + vr.bars_turn;

		if (vr.bars_held[0] && vr.bars_held[1])
		{
			int axis;

			for (axis = 0; axis < 3; axis++)
				vr.aim_line_position[axis] = (vr.seat.handles[0][axis] + vr.seat.handles[1][axis]) * 0.5f;
			vr.aim_line_forward[0] = cosf(vr.bars_pitch) * cosf(turn);
			vr.aim_line_forward[1] = cosf(vr.bars_pitch) * sinf(turn);
			vr.aim_line_forward[2] = sinf(vr.bars_pitch);
			vr.aim_line_valid = 1;
		}
		*yaw = vr.body_yaw + turn;
		*pitch = vr.bars_pitch;
		vr.aiming = 1;
		return 1;
	}
	/* where the right controller points (vr.aim), or a long gun held in
	both hands, else where the head looks */
	if (settings.controller_aim && vr.views.aim[1].valid)
	{
		float up[3], offset[3];
		int axis;

		weapon_pointing(v, up);
		aim_steady(v);
		game_axes(v, vr.origin_yaw, f);
		head_yaw = atan2f(f[1], f[0]);
		for (axis = 0; axis < 3; axis++)
			offset[axis] = vr.views.aim[1].position[axis] - vr.origin_position[axis];
		game_axes(offset, vr.origin_yaw, vr.aim_line_position);
		memcpy(vr.aim_line_forward, f, sizeof(vr.aim_line_forward));
		vr.aim_line_valid = 1;
	}
	*yaw = vr.body_yaw + head_yaw;
	*pitch = asinf(f[2] < -1.0f ? -1.0f : f[2] > 1.0f ? 1.0f : f[2]);
	vr.aiming = 1;
	return 1;
}

void halo_vr_aim_release(void)
{
	vr.aim_synced = 0;
	vr.aiming = 0;
	vr.aim_line_valid = 0;
}

int halo_vr_aim_line(float position[3], float forward[3], float *body_yaw)
{
	if (!vr.aiming || !vr.aim_line_valid || settings.gamepad_aim)
		return 0;
	memcpy(position, vr.aim_line_position, sizeof(vr.aim_line_position));
	memcpy(forward, vr.aim_line_forward, sizeof(vr.aim_line_forward));
	*body_yaw = vr.body_yaw;
	return 1;
}

void halo_vr_menus(int active)
{
	vr.menus = active;
	vr_visor_menus(active);
}

int halo_vr_aiming(void)
{
	return vr.aiming;
}

/* test hooks run before this frame aims: the last completed frame */
int halo_vr_aimed(void)
{
	return vr.aimed;
}

int halo_vr_gamepad_aiming(void)
{
	return vr.aiming && settings.gamepad_aim;
}

void halo_vr_gamepad_back(int held)
{
	vr.gamepad_back = held;
}

/* ---------- the controllers as a gamepad */

static void analog(XINPUT_GAMEPAD *pad, int index, int down)
{
	if (down)
		pad->bAnalogButtons[index] = 0xff;
}

static void stick_merge(SHORT *axis, float value)
{
	long scaled = (long)(value * 32767.0f);

	if (scaled > 32767)
		scaled = 32767;
	if (scaled < -32767)
		scaled = -32767;
	if (labs(scaled) > labs((long)*axis))
		*axis = (SHORT)scaled;
}

void halo_vr_gamepad(void *gamepad)
{
	XINPUT_GAMEPAD *pad = gamepad;
	unsigned int buttons = vr.input.buttons;
	BYTE trigger;

	/* (vr.aim = "gamepad": the controllers play as the halves of an Xbox
	controller, as any gamepad does, their poses aiming nothing) */
	if (!vr.initialized || !(vr.frame || vr.pending))
		return;
	analog(pad, XINPUT_GAMEPAD_A, buttons & VR_BUTTON_A);
	/* (and a swing of the right controller melees, as B: vr.melee_gesture) */
	analog(pad, XINPUT_GAMEPAD_B, (buttons & VR_BUTTON_B) || (long)(vr.melee_release - GetTickCount()) > 0);
	analog(pad, XINPUT_GAMEPAD_X, buttons & VR_BUTTON_X);
	/* (and a gun drawn from its holster switches to it, as Y) */
	analog(pad, XINPUT_GAMEPAD_Y, (buttons & VR_BUTTON_Y) || vr.holster_press_frames > 0);
	/* the bumpers where the Xbox's white and black buttons were: the
	flashlight and the grenade switch */
	analog(pad, XINPUT_GAMEPAD_WHITE, buttons & VR_BUTTON_LEFT_BUMPER);
	analog(pad, XINPUT_GAMEPAD_BLACK, buttons & VR_BUTTON_RIGHT_BUMPER);
	trigger = (BYTE)(vr.input.left_trigger * 255.0f);
	/* (vr.grenade_throw = "gesture": on foot, aiming with the controller,
	only the hand throws, not the left trigger; in a vehicle it is the
	vehicle's, the Banshee's second weapon. halo_vr_crouch_able says on
	foot) */
	if (settings.throw_gesture_only && settings.controller_aim && vr.crouch_able_frame)
		trigger = 0;
	/* (the left hand's swing throws as the left trigger does: the game's
	throw, which its release then times, vr_grenade.c) */
	if (vr.throw_state == THROW_SWING || vr.throw_press_frames > 0)
		trigger = 255;
	if (trigger > pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER])
		pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = trigger;
	trigger = (BYTE)(vr.input.right_trigger * 255.0f);
	/* (its magazine out, the weapon in hand does not fire: vr.manual_reload) */
	if (vr.reload.ejected && vr.reload.enabled)
		trigger = 0;
	if (trigger > pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER])
		pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = trigger;
	if (buttons & VR_BUTTON_DPAD_UP) pad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
	if (buttons & VR_BUTTON_DPAD_DOWN) pad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
	if (buttons & VR_BUTTON_DPAD_LEFT) pad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
	if (buttons & VR_BUTTON_DPAD_RIGHT) pad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
	if (buttons & VR_BUTTON_MENU) pad->wButtons |= XINPUT_GAMEPAD_START;
	if (buttons & VR_BUTTON_VIEW) pad->wButtons |= XINPUT_GAMEPAD_BACK;
	/* the crouch: a hand's (the left stick's click, a gamepad's), or the
	head's (vr.physical_crouch), which Halo holds only while it is pressed */
	vr.crouch_button = (buttons & VR_BUTTON_LEFT_STICK) || (pad->wButtons & XINPUT_GAMEPAD_LEFT_THUMB);
	if (vr.crouch_button || vr.crouching) pad->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
	if (buttons & VR_BUTTON_RIGHT_STICK) pad->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
	/* (the left stick's sideways push steers where the sticks steer a seat
	or turn a driver: not the game's sideways throttle, the Ghost's strafe) */
	if (!seat_stick_steers() && !seat_drives())
		stick_merge(&pad->sThumbLX, vr.input.left_stick[0]);
	stick_merge(&pad->sThumbLY, vr.input.left_stick[1]);
	/* the right stick turns (input_update); while the aim does not follow
	the head (menus, cinematics, a vehicle's turret without it), and always
	with vr.aim = "gamepad", it looks as a gamepad's would */
	if (!vr.aimed || settings.gamepad_aim)
	{
		stick_merge(&pad->sThumbRX, vr.input.right_stick[0]);
		stick_merge(&pad->sThumbRY, vr.input.right_stick[1]);
	}
}

/* ---------- the HUD's prompts (port/linux/game/hud_key_prompts.c) */

/* the controller inputs that press each of the game's controller buttons
(halo_vr_gamepad's, in the game's order: A, B, X, Y, black, white, the
triggers, the d-pad, Start, Back, the sticks' clicks), and the names they
have when the runtime names none: the Frame's, or the Touch's, which have
grips where the Frame has bumpers */
static const struct
{
	int input;
	const char *frame_name;
	const char *touch_name;
} prompt_inputs[] =
{
	{ VR_BUTTON_A, "A", "A" },
	{ VR_BUTTON_B, "B", "B" },
	{ VR_BUTTON_X, "X", "X" },
	{ VR_BUTTON_Y, "Y", "Y" },
	{ VR_BUTTON_RIGHT_BUMPER, "Right Bumper", "Right Grip" },
	{ VR_BUTTON_LEFT_BUMPER, "Left Bumper", "Left Grip" },
	{ VR_INPUT_LEFT_TRIGGER, "Left Trigger", "Left Trigger" },
	{ VR_INPUT_RIGHT_TRIGGER, "Right Trigger", "Right Trigger" },
	{ VR_BUTTON_DPAD_UP, "D-Pad Up", "D-Pad Up" },
	{ VR_BUTTON_DPAD_DOWN, "D-Pad Down", "D-Pad Down" },
	{ VR_BUTTON_DPAD_LEFT, "D-Pad Left", "D-Pad Left" },
	{ VR_BUTTON_DPAD_RIGHT, "D-Pad Right", "D-Pad Right" },
	{ VR_BUTTON_MENU, "Menu", "Menu" },
	{ VR_BUTTON_VIEW, "View", "View" },
	{ VR_BUTTON_LEFT_STICK, "Left Thumbstick Click", "Left Thumbstick Click" },
	{ VR_BUTTON_RIGHT_STICK, "Right Thumbstick Click", "Right Thumbstick Click" },
};

#define PROMPT_INPUTS ((int)(sizeof(prompt_inputs) / sizeof(prompt_inputs[0])))
/* how long the runtime's names are kept before they are asked again (the
controllers, and with them the bindings, may change) */
#define PROMPT_NAMES_MS 5000

/* UTF-8 as the HUD's text takes it, Latin-1: 0 for a character beyond */
static int prompt_name_latin1(const char *utf8, char *name, unsigned long size)
{
	const unsigned char *in = (const unsigned char *)utf8;
	unsigned long used = 0;

	while (*in && used + 1 < size)
	{
		if (*in < 0x80)
			name[used++] = (char)*in++;
		else if ((in[0] == 0xc2 || in[0] == 0xc3) && (in[1] & 0xc0) == 0x80)
		{
			name[used++] = (char)(((in[0] & 0x03) << 6) | (in[1] & 0x3f));
			in += 2;
		}
		else
			return 0;
	}
	name[used] = 0;
	return used > 0;
}

int halo_vr_button_name(int button, char *name, unsigned long size)
{
	static char names[PROMPT_INPUTS][64];
	static unsigned long named_at;
	static int named;
	unsigned long now = GetTickCount();
	int index;

	if (!size)
		return 0;
	name[0] = 0;
	if (!vr.initialized || button < 0 || button >= PROMPT_INPUTS)
		return 0;
	/* (the left trigger thrown by the hand alone, on foot: vr.grenade_throw
	"gesture", halo_vr_gamepad) */
	if (button == 6 && settings.throw_gesture_only && settings.controller_aim && vr.crouch_able_frame)
	{
		snprintf(name, size, "%s", "Left Hand Throw");
		return 1;
	}
	if (!named || now - named_at >= PROMPT_NAMES_MS)
	{
		char text[64];

		named = 1;
		named_at = now;
		for (index = 0; index < PROMPT_INPUTS; index++)
		{
			char kept[sizeof(names[index])];

			memcpy(kept, names[index], sizeof(kept));
			if (!host_vr_input_name(prompt_inputs[index].input, text, (int)sizeof(text)) ||
				!prompt_name_latin1(text, names[index], sizeof(names[index])))
			{
				names[index][0] = 0;
			}
			/* (each name the runtime gives, said as it changes) */
			if (strcmp(kept, names[index]))
				platform_log("vr: the prompts' %s is \"%s\"", prompt_inputs[index].frame_name, names[index]);
		}
	}
	index = button;
	/* the game's triggers are the dominant hand's and the other's
	(hands_assign_input), the sticks' clicks the move's and the turn's */
	if (settings.left_handed && (button == 6 || button == 7))
		index = button == 6 ? 7 : 6;
	if (settings.swap_sticks && (button == 14 || button == 15))
		index = button == 14 ? 15 : 14;
	snprintf(name, size, "%s", names[index][0] ? names[index] :
		strstr(vr.info.system_name, "Frame") ? prompt_inputs[index].frame_name : prompt_inputs[index].touch_name);
	return 1;
}

void halo_vr_rumble(float left, float right)
{
	unsigned long now = GetTickCount();

	if (!vr.initialized)
		return;
	/* (not over a hand's own pattern while it plays: haptic_play) */
	if (left > 0.01f && (long)(now - vr.haptic_end[0]) >= 0)
		hand_haptic(0, left, 0.1f);
	if (right > 0.01f && (long)(now - vr.haptic_end[1]) >= 0)
		hand_haptic(1, right, 0.1f);
}
#endif
