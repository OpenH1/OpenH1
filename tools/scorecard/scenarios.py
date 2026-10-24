"""The scorecard's scenarios (README.md): each a StageKit map (tools/stagekit/scenarios) and its parameters, a
script on the simulation's clock (debug.test_input "script:"), how long it plays, cues (console lines at seconds),
what it measures (metrics.py) and its tags, by which a swarm's unit runs its own (the plan's units, U1-U20, and
the parts of the game).

A scenario is data: the hidden scenarios (holdouts, README.md) are files of the same, kept out of the repository.
One that needs what CI does not have (the retail maps) names it in "needs", and is skipped.

Script times are on the 30 Hz grid (multiples of 1/30), where every rate has a step, so a script plays the same
at 30, 60 and 120; but for the jumps pressed off it (jump_off_grid_*), whose takeoffs differ between the rates by
design.
"""

# the plan's work units (60hz-audit.md 5.1) whose subset each part of the game is
UNIT_TAGS = {
    "jump": ["U1", "U3"],
    "biped": ["U1"],
    "projectile": ["U4"],
    "weapon": ["U5"],
    "grenade": ["U4", "U6"],
    "elevator": ["U1", "U7", "U19"],
    "vehicle": ["U17"],
}


def scenario(name, map, metric, tags, script="", seconds=3.0, params=None, arguments=None, cues=(), needs=None):
    return dict(name=name, map=map, params=params or {}, script=script, seconds=seconds, cues=list(cues),
                metric=metric, arguments=arguments or {}, tags=sorted(set(tags + [t for tag in tags
                                                                                   for t in UNIT_TAGS.get(tag, [])])),
                needs=needs)


def grenade_volley(pitches):
    """a grenade fired every second, each at its pitch, each in its own direction"""
    events = []
    for number, pitch in enumerate(pitches):
        start = 0.5 + number
        events.append(f"{start - 0.1:.1f} aim={number * 72},{pitch}")
        events.append(f"{start:.1f} fire")
    return "; ".join(events)


SCENARIOS = [
    # jumps (the plan's row 2): apex height and time, air time, distance
    scenario("jump_standing", "walk", "jump", ["jump", "biped"], script="0.5 jump", seconds=2.5,
             arguments=dict(after=0.4)),
    scenario("jump_running", "walk", "jump", ["jump", "biped"], script="0.2-3 move=0,1; 1 jump", seconds=3.0,
             arguments=dict(after=0.9)),
    scenario("jump_strafing", "walk", "jump", ["jump", "biped"], script="0.2-3 move=1,0.5; 1.5 jump", seconds=3.5,
             arguments=dict(after=1.4)),
    # jumps pressed off the 30 Hz grid (the sub-tick jump): at 120 a press read between a tick's sub-steps takes off a
    # sub-step after it is read, not at the tick's end. Their takeoffs differ between the rates by design, each
    # checked against where its rate puts it (metrics.jump_takeoff_expected), and their arcs from takeoff compared.
    # 0.5125: read at 120's third sub-step of a tick, 60's second; 0.7375: the second of each; 1.02: 120's last and
    # 60's first (the tick's own input, decided at the tick's end); running and strafing, their distances too
    scenario("jump_off_grid", "walk", "jump_off_grid", ["jump", "biped"], script="0.5125 jump", seconds=2.5,
             arguments=dict(press=0.5125, after=0.4)),
    scenario("jump_off_grid_running", "walk", "jump_off_grid", ["jump", "biped"],
             script="0.2-3 move=0,1; 0.7375 jump", seconds=2.7, arguments=dict(press=0.7375, after=0.6)),
    scenario("jump_off_grid_strafing", "walk", "jump_off_grid", ["jump", "biped"],
             script="0.2-3 move=1,0.5; 1.02 jump", seconds=3.0, arguments=dict(press=1.02, after=0.9)),
    # a crouch (U1's, synthetic): the player crouches for a second, then taps crouch (a tenth of a second, so it
    # stands again before it is all the way down), standing and in a jump (where the crouch lifts its feet): how far
    # it is crouched and its eye's height every 30th of a second, when each change starts and ends, and how far its
    # eye is from the ticks' lines between the grid's times (the view, which stands where the newest step left it:
    # render_interpolation.c)
    scenario("crouch_standing", "crouch", "crouch", ["crouch", "biped"], script="0.5-1.5 crouch; 2.5 crouch",
             seconds=3.5, arguments=dict(start=0.4)),
    scenario("crouch_jumping", "crouch", "crouch", ["crouch", "biped"], script="0.5 jump; 0.6-1.6 crouch",
             seconds=2.5, arguments=dict(start=0.4)),
    # the first shot's travel (row 6): where it is a 30th, 2, 3 and 5 30ths of a second after it is fired, flying
    # free (a wall 500 away), level and aimed up
    scenario("first_shot_level", "shoot", "shots", ["first_shot", "projectile", "weapon"],
             script="0.4 aim=270,0; 0.5 fire", seconds=1.5, arguments=dict(ranges=[1, 2, 3, 5])),
    scenario("first_shot_up", "shoot", "shots", ["first_shot", "projectile", "weapon"],
             script="0.4 aim=270,20; 0.5 fire", seconds=1.5, arguments=dict(ranges=[1, 2, 3, 5])),
    scenario("first_shot_slow", "shoot", "shots", ["first_shot", "projectile", "weapon"],
             script="0.4 aim=270,0; 0.5 fire", seconds=1.5, params=dict(bullet_speed=6),
             arguments=dict(ranges=[1, 2, 3, 5])),
    # strikes (row 7): where and when the shot strikes a wall 2, 10 and 40 away, and the floor
    scenario("impact_walls", "shoot", "shots", ["impact", "projectile", "weapon"],
             script="0.4 aim=0,0; 0.5 fire; 0.9 aim=90,0; 1 fire; 1.4 aim=180,0; 1.5 fire; 1.9 aim=300,-30; 2 fire",
             seconds=3.0, arguments=dict(ranges=[1], impact=True)),
    scenario("impact_drop", "shoot", "shots", ["impact", "projectile", "weapon"],
             script="0.4 aim=180,1; 0.5 fire", seconds=3.0, params=dict(bullet_speed=30, bullet_gravity=1),
             arguments=dict(ranges=[1, 3, 10], impact=True)),
    # a held trigger (row 5, synthetic): how many shots in 3 seconds and when, at rates of fire 1, 7, 15 and 30
    *[scenario(f"cadence_{rate}", "shoot", "shots", ["cadence", "weapon"], script="0.4 aim=270,0; 0.5-3.5 fire",
               seconds=4.0, params=dict(rifle_rate=rate), arguments=dict(cadence=True)) for rate in (1, 7, 15, 30)],
    # grenades (row 4): where and when each first lands, where it lies when it goes off, and its fuse, at 5 pitches
    scenario("grenade_arcs", "lob", "grenades", ["grenade"], script=grenade_volley([-30, 0, 15, 30, 45]),
             seconds=11.0),
    # riding a lift (row 15): the player's height every 30th of a second, a lift of the macro lane under a player of
    # the micro lane
    scenario("elevator_ride", "elevator", "height_curve", ["elevator"],
             cues=[(1, "device_set_position elevator 1"), (5, "device_set_position elevator 0")], seconds=8.0,
             arguments=dict(start=0.5, end=7.5)),
    # the AI's biped (row 12's, synthetic; U18): an actor made of a biped walks a command list (3 units along x, 2
    # at 25 degrees, 2 along x again, each a list of its own): its place every 30th of a second, how far it walked,
    # and how far its steps between the grid's times are from the ticks' lines (a body moved once a tick lags
    # them). Its brain is the tick's, its body moves in the sub-steps
    scenario("ai_patrol", "patrol", "ai_path", ["ai", "U18"],
             cues=[(0.5, "ai_attach_free walker walker"), (1, "ai_command_list_by_unit walker leg1"),
                   (4, "ai_command_list_by_unit walker leg2"), (7, "ai_command_list_by_unit walker leg3")],
             seconds=10.0, arguments=dict(start=0.5, end=9.5)),
    # a ledge (U15, synthetic): the player walks off a block 2 high: when it goes over the edge, how long it falls,
    # how far it drops and goes across, and its height every 30th of a second through the fall (and U1's: the biped
    # leaves the ground when the tick's own step does, bipeds.c's biped_substep_move)
    scenario("ledge_fall", "ledge", "fall", ["ledge", "U15", "biped", "U1"], script="0.5-3 move=0,1", seconds=3.0,
             arguments=dict(after=0.5)),
    # a shield (U14, synthetic): the player's biped with a shield of 75 recharging in 2 seconds, dropped to nothing
    # at a second and to half at 4: its vitality every 30th of a second, when it was lowest and when full again
    scenario("shield_recharge", "shield", "shield", ["shield", "U14"],
             cues=[(1, "unit_set_current_vitality (unit (list_get (players) 0)) 75 0"),
                   (4, "unit_set_current_vitality (unit (list_get (players) 0)) 75 37.5")],
             seconds=6.0, arguments=dict(start=0.5)),
    # grenades thrown by hand (U12, synthetic; row 4's): the throw animation lets go of each a third of a second after
    # the press, at three pitches, each in its own direction: where and when each first lands, where it lies when it
    # goes off, and its fuse
    scenario("grenade_thrown", "throw", "thrown", ["grenade", "U12"],
             script="0.4 aim=0,0; 0.5 throw; 1.9 aim=120,20; 2 throw; 3.4 aim=240,40; 3.5 throw", seconds=10.5),
]


# an AI's biped riding a lift (rows 12 and 15's, synthetic; U19): an actor on the elevator's platform walks across
# it as it rises 5 units and back as it comes down: its place every 30th of a second, and how far its steps between
# the grid's times are from the ticks' lines (a rider that moves with the lift a whole tick at once, at one of the
# tick's sub-steps, misses them by the share it moved too soon). The lift moves on the tick, its rider in the
# sub-steps. The rider is updated before the lift in each tick, or after it (late: placed by the console), and rides
# on its floor, or is carried by it (body: a biped with a collision model, as the game's are)
def ai_rider(name, late=False, body=False):
    cues = [(0.5, "ai_attach_free rider rider"), (1, "device_set_position lift 1"),
            (1.2, "ai_command_list_by_unit rider across"), (5, "device_set_position lift 0"),
            (5.2, "ai_command_list_by_unit rider back")]
    params = dict(rider_late=1) if late else {}
    if late:
        cues.append((0.2, "object_create rider"))
    if body:
        params["rider_body"] = 1
    return scenario(name, "lift", "ai_path", ["elevator", "ai", "U19"], params=params, cues=cues, seconds=8.0,
                    arguments=dict(start=0.5, end=7.5))


SCENARIOS += [
    ai_rider("elevator_ai_rider"),
    ai_rider("elevator_ai_rider_late", late=True),
    ai_rider("elevator_ai_rider_carried", body=True),
    ai_rider("elevator_ai_rider_carried_late", late=True, body=True),
]

# a player's vehicle (the plan's row 9, synthetic): StageKit's car, which the player sits in (the cue) and drives
# (the script): along x to its top speed, round in a circle at full lock, over a ramp, off a platform, rolled over
# by turning too hard, and into a parked car
def drive(car):
    return [(0.1, f'unit_enter_vehicle (unit (list_get (players) 0)) {car} "stand"')]


SCENARIOS += [
    scenario("vehicle_straight", "drive", "vehicle", ["vehicle"], script="0.5 aim=0,0; 1-4.5 move=0,1",
             seconds=4.5, cues=drive("car"), arguments=dict(kind="straight", start=1.0)),
    scenario("vehicle_turn", "drive", "vehicle", ["vehicle"], script="0.5 aim=0,0; 1-8 move=0,1; 2-8 look=0.35,0",
             seconds=8.0, cues=drive("car"), arguments=dict(kind="turn", start=1.0)),
    scenario("vehicle_jump", "drive", "vehicle", ["vehicle"], script="0.5 aim=0,0; 1-9 move=0,1", seconds=9.0,
             cues=drive("car"), arguments=dict(kind="jump", start=6.0)),
    scenario("vehicle_ledge", "drive", "vehicle", ["vehicle"], script="0.5 aim=180,0; 1-6 move=0,1", seconds=6.0,
             cues=drive("high"), arguments=dict(kind="landing", start=3.0)),
    scenario("vehicle_flip", "drive", "vehicle", ["vehicle"], script="0.5 aim=0,0; 1-8 move=0,1; 2-8 look=1,0",
             seconds=8.0, cues=drive("car"), arguments=dict(kind="flip", start=1.0, end=4.5)),
    scenario("vehicle_crash", "drive", "vehicle", ["vehicle"], script="0.5 aim=0,0; 1-8 move=0,1", seconds=8.0,
             cues=drive("racer"), arguments=dict(kind="crash", start=1.0)),
]

# what CI cannot play, the retail maps' (the plan's rows 5, 9, 10, 12-14 and the retail halves of 4, 6-8), and
# what StageKit has no content for yet, each skipped until it can be
RETAIL = "retail maps"
SCENARIOS += [
    *[scenario(f"retail_rof_{weapon}", "retail", "shots", ["cadence", "weapon", "retail"], needs=RETAIL)
      for weapon in ("pistol", "assault_rifle", "shotgun", "sniper_rifle", "rocket_launcher", "flamethrower",
                     "plasma_pistol", "plasma_rifle", "needler", "fuel_rod", "plasma_cannon")],
    *[scenario(f"retail_first_shot_{weapon}", "retail", "shots", ["first_shot", "projectile", "retail"], needs=RETAIL)
      for weapon in ("pistol", "sniper_rifle", "assault_rifle")],
    *[scenario(f"retail_grenade_{kind}", "retail", "grenades", ["grenade", "retail", "U2"], needs=RETAIL)
      for kind in ("frag", "plasma")],
    scenario("retail_projectile_guided_needler", "retail", "shots", ["projectile", "retail"], needs=RETAIL),
    scenario("retail_projectile_rocket", "retail", "shots", ["projectile", "retail"], needs=RETAIL),
    *[scenario(f"retail_vehicle_{vehicle}", "retail", "vehicle", ["vehicle", "retail", "U17"], needs=RETAIL)
      for vehicle in ("warthog", "ghost")],
    scenario("retail_melee_lunge", "retail", "melee", ["melee", "retail", "U1", "U2"], needs=RETAIL),
    *[scenario(f"retail_ai_reaction_{level}", "retail", "ai", ["ai", "retail", "U18"], needs=RETAIL)
      for level in ("a10", "a30")],
    *[scenario(f"retail_script_timing_{level}", "retail", "scripts", ["scripts", "retail"], needs=RETAIL)
      for level in ("a10", "b30")],
    *[scenario(f"retail_cinematic_{level}", "retail", "cinematic", ["cinematic", "retail"], needs=RETAIL)
      for level in ("a10", "c10")],
    scenario("vr_hand_throw", "vr", "throw", ["vr", "U10"], needs="the VR mode's test hands (tools/vr_test)"),
    scenario("classic_network_rate", "network", "rate", ["network", "U9"],
             needs="a two-machine network test (debug.network_test)"),
]
