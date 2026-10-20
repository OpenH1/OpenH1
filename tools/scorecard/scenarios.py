"""The scorecard's scenarios (README.md): each a StageKit map (tools/stagekit/scenarios) and its parameters, a
script on the simulation's clock (debug.test_input "script:"), how long it plays, cues (console lines at seconds),
what it measures (metrics.py) and its tags, by which a swarm's unit runs its own (the plan's units, U1-U20, and
the parts of the game).

A scenario is data: the hidden scenarios (holdouts, README.md) are files of the same, kept out of the repository.
One that needs what CI does not have (the retail maps) names it in "needs", and is skipped.

Script times are on the 30 Hz grid (multiples of 1/30), where every rate has a step, so a script plays the same
at 30, 60 and 120.
"""

# the plan's work units (60hz-audit.md 5.1) whose subset each part of the game is
UNIT_TAGS = {
    "jump": ["U1", "U3"],
    "biped": ["U1"],
    "projectile": ["U4"],
    "weapon": ["U5"],
    "grenade": ["U4", "U6"],
    "elevator": ["U1", "U7", "U19"],
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
    scenario("grenade_thrown", "stagekit", "grenades", ["grenade", "U2"],
             needs="a StageKit biped with a grenade throw animation (U12)"),
    scenario("shield_recharge", "stagekit", "shield", ["shield", "U2"], needs="a StageKit shield (U14)"),
    scenario("ledge_fall", "stagekit", "fall", ["biped", "U1"], needs="a StageKit ledge (U15)"),
    scenario("vr_hand_throw", "vr", "throw", ["vr", "U10"], needs="the VR mode's test hands (tools/vr_test)"),
    scenario("classic_network_rate", "network", "rate", ["network", "U9"],
             needs="a two-machine network test (debug.network_test)"),
]
