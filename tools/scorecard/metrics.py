"""The scorecard's measures (README.md): a trace read (debug.trace, port/linux/game/sim_trace.c), what each kind of
scenario measures in it, in real units (seconds, world units), and how far two rates' measures may be apart.

Every measure is taken so that it means the same at any simulation rate: times are the simulation's, in seconds;
a shot's time is the start of the step it was fired in (the input's time, on the 30 Hz grid, at every rate); a
strike's is in its update (a sub-step's, or the tick's) by the fraction the projectile code found; positions are compared at times every rate has
a step at (the 30 Hz grid: 1/30 is a whole number of steps at 60 and 120); a jump's curve is fitted, not sampled.
"""

import math

# how far apart two rates' measures of a kind may be (tools/scorecard/README.md: the plan's tolerances): an
# absolute amount in the measure's unit, a fraction of the measure ("relative"), or none at all ("exact"); "step"
# adds the step of the faster of the two rates
TOLERANCES = {
    "apex_height": dict(absolute=0.003, unit="wu"),
    "jump_time": dict(absolute=0.001, unit="s"),
    "jump_distance": dict(relative=0.005, unit="wu"),
    "shots": dict(exact=True, unit="shots"),
    "fire_time": dict(absolute=0.001, unit="s"),
    "shot_times": dict(absolute=0.001, unit="s max"),
    "instant_range": dict(relative=0.001, unit="wu"),
    "impact_point": dict(absolute=0.005, unit="wu"),
    "impact_time": dict(absolute=0.001, step=True, unit="s"),
    "grenade_impact": dict(absolute=0.01, unit="wu"),
    "grenade_time": dict(absolute=0.001, unit="s"),
    "rest_point": dict(absolute=0.25, unit="wu"),
    "fuse": dict(absolute=0.001, step=True, unit="s"),
    "height_curve": dict(absolute=0.01, unit="wu rms"),
    "ai_path": dict(absolute=0.01, unit="wu rms"),
    "ai_walked": dict(relative=0.005, unit="wu"),
    "ai_between": dict(absolute=0.002, unit="wu"),
    # a player's vehicle (U17): the plan's row 9 (0 to its top speed 2%, its top speed 1%, its turning circle 2%);
    # its path and height through a run within a tenth of a unit RMS; a jump's apex and its suspension's travel
    # after landing within 3 hundredths; a crash's moment within two ticks (vehicles find each other at a tick's
    # end, and what it strikes, of the macro lane, answers on its tick: a few milliseconds either way is a tick
    # either way); a roll-over within 5 30ths of a second
    "vehicle_time": dict(relative=0.02, unit="s"),
    "vehicle_speed": dict(relative=0.01, unit="wu/s"),
    "vehicle_turn": dict(relative=0.02, unit="wu"),
    "vehicle_path": dict(absolute=0.1, unit="wu rms"),
    "vehicle_height": dict(absolute=0.03, unit="wu"),
    "vehicle_impact": dict(absolute=2.0 / 30 + 0.001, unit="s"),
    "vehicle_flip": dict(absolute=0.167, unit="s"),
}

# a step's time against a time on the grid
GRID_SLACK = 1e-4


class Trace:
    """a trace read: its rate, mutation and steps; each step (t, tick, micro, players {index: fields}, objects
    {id: (x, y, z, vx, vy, vz)}, the AI's bipeds ai {id: (x, y, z, vx, vy, vz, airborne)}, events [words])"""

    def __init__(self, path):
        self.rate = None
        self.mutation = None
        self.steps = []
        step = None
        with open(path, errors="replace") as lines:
            for line in lines:
                words = line.split()
                if not words:
                    continue
                if words[0] == "#" and "rate" in words:
                    self.rate = int(words[words.index("rate") + 1])
                    self.mutation = words[words.index("mutation") + 1] if "mutation" in words else "-"
                elif words[0] == "s":
                    step = dict(t=float(words[1]), tick=int(words[2]), micro=int(words[3]), players={}, objects={},
                                ai={}, events=[])
                    self.steps.append(step)
                elif step is None:
                    continue
                elif words[0] == "p":
                    step["players"][int(words[1])] = [float(word) for word in words[2:12]]
                elif words[0] == "o":
                    step["objects"][words[1]] = tuple(float(word) for word in words[2:8])
                elif words[0] == "u":
                    step["ai"][words[1]] = tuple(float(word) for word in words[2:9])
                elif words[0] == "e":
                    step["events"].append(words[1:])
                elif words[0] == "v":
                    step.setdefault("vehicles", {})[words[2]] = (int(words[1]),
                                                                  [float(word) for word in words[3:18]],
                                                                  int(words[18]))
        self.step_seconds = 1.0 / self.rate if self.rate else None

    def events(self, kind):
        """each event of the kind: (the step's start, the step, its words after the kind)"""
        return [(step["t"] - self.step_seconds, step, words[1:]) for step in self.steps for words in step["events"]
                if words[0] == kind]

    def at(self, t):
        """the step that ends at t (on the grid), else None"""
        for step in self.steps:
            if abs(step["t"] - t) < GRID_SLACK:
                return step
        return None


def distance(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def fit_quadratic(samples):
    """least squares a t^2 + b t + c through (t, value) samples"""
    n = len(samples)
    t0 = samples[0][0]
    sums = [0.0] * 5
    rhs = [0.0] * 3
    for t, value in samples:
        x = t - t0
        powers = [1.0, x, x * x, x ** 3, x ** 4]
        for index in range(5):
            sums[index] += powers[index]
        for index in range(3):
            rhs[index] += value * powers[index]
    matrix = [[sums[0], sums[1], sums[2]], [sums[1], sums[2], sums[3]], [sums[2], sums[3], sums[4]]]
    c, b, a = solve3(matrix, rhs) if n >= 3 else (samples[0][1], 0.0, 0.0)
    # (back from t - t0 to t)
    return a, b - 2 * a * t0, c - b * t0 + a * t0 * t0


def solve3(m, r):
    (a, b, c), (d, e, f), (g, h, i) = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    x = (r[0] * (e * i - f * h) - b * (r[1] * i - f * r[2]) + c * (r[1] * h - e * r[2])) / det
    y = (a * (r[1] * i - f * r[2]) - r[0] * (d * i - f * g) + c * (d * r[2] - r[1] * g)) / det
    z = (a * (e * r[2] - r[1] * h) - b * (d * r[2] - r[1] * g) + r[0] * (d * h - e * g)) / det
    return x, y, z


# ---------- what each kind of scenario measures: {name: (value, kind)}


def measure_jump(trace, after=0.0, player=0):
    """the first jump after a time: the player off the ground (airborne) and rising. Its height over time is fitted
    (a parabola, as constant gravity makes it at any rate), and its takeoff and landing are where the fit crosses
    the ground's height; its distance is how far it went across, takeoff to landing"""
    steps = [step for step in trace.steps if player in step["players"] and step["t"] > after]
    for index, step in enumerate(steps):
        fields = step["players"][player]
        if fields[9] and index and not steps[index - 1]["players"][player][9] and fields[5] > 0:
            ground = steps[index - 1]["players"][player][2]
            air = []
            for later in steps[index:]:
                if not later["players"][player][9]:
                    break
                air.append(later)
            if len(air) < 4:
                return {}
            a, b, c = fit_quadratic([(s["t"], s["players"][player][2]) for s in air])
            if a >= 0:
                return {}
            apex_t = -b / (2 * a)
            apex = a * apex_t ** 2 + b * apex_t + c
            root = math.sqrt(max(0.0, b * b - 4 * a * (c - ground)))
            takeoff, landing = sorted(((-b + root) / (2 * a), (-b - root) / (2 * a)))
            across = [fit_quadratic([(s["t"], s["players"][player][axis]) for s in air]) for axis in (0, 1)]

            def place(t):
                return tuple(p * t * t + q * t + r for p, q, r in across)

            return {"apex_height": (apex - ground, "apex_height"),
                    "apex_time": (apex_t - takeoff, "jump_time"),
                    "air_time": (landing - takeoff, "jump_time"),
                    "takeoff_time": (takeoff, "jump_time"),
                    "distance": (distance(place(takeoff), place(landing)), "jump_distance")}
    return {}


def jump_takeoff_expected(press, rate):
    """where a jump pressed at a time (seconds; off the 30 Hz grid or on it) takes off at a rate: the scripted input
    first has the press at the first step that starts at or after it (the script's clock is a step's start), and the
    jump launches at that step's end, as a tick at 30 launches one at its end, its press read at its start; but a
    press read at a tick's first step (the tick's own input: every step at 30, a step on the 30 Hz grid at 60 and
    120) launches at the tick's end. So a press on the grid takes off a tick after it at every rate, and one between
    a tick's steps a step of that rate after the step that read it (bipeds.c, biped_substep_jump)"""
    step = 1.0 / rate
    # (a hair before a step's time is that step's, as the script reads it: xinput_sdl.c)
    start = math.ceil((press - 1e-6) * rate) / rate
    on_grid = abs(start * 30 - round(start * 30)) < GRID_SLACK * 30
    return start + (1.0 / 30 if on_grid else step)


def measure_jump_off_grid(trace, press, after=0.0, player=0):
    """a jump pressed off the 30 Hz grid (README.md): its takeoff legitimately differs between the rates (by under a
    tick of the press's real time), so it is measured against where its own rate puts it (takeoff_error:
    jump_takeoff_expected; 0 at 30 by its mechanics, so a pair checks each rate's), and the jump's arc from its
    takeoff (its apex's height and time, its air time, its distance) is compared between the rates as every jump's"""
    result = measure_jump(trace, after, player)
    if "takeoff_time" not in result:
        return {}
    takeoff = result.pop("takeoff_time")[0]
    result["takeoff_error"] = (takeoff - jump_takeoff_expected(press, trace.rate), "jump_time")
    return result


def shots_of(trace):
    """each shot: (its time, its first projectile's id and where it started)"""
    news = trace.events("new")
    shots = []
    for t, step, words in trace.events("fire"):
        made = [w for s_t, s, w in news if s is step]
        if made:
            shots.append((t, made[0][0], tuple(float(x) for x in made[0][2:5])))
    return shots


def hits_of(trace, projectile):
    """each strike of a projectile: (its time, point)"""
    return [(float(words[1]), tuple(float(x) for x in words[2:5]))
            for t, step, words in trace.events("hit") if words[0] == projectile]


def measure_shots(trace, ranges=(), impact=False, cadence=False):
    """each shot's time, how far its projectile has gone from where it started at each of the grid's times after
    it (ranges, in 30ths of a second: 1 is its "instant range", where it is once the step it was fired in is over at
    30 Hz), and where and when it first strikes (impact); with cadence, how many shots"""
    result = {}
    shots = shots_of(trace)
    if cadence:
        result["shots"] = (len(shots), "shots")
        result["shot_times"] = (tuple(t for t, _, _ in shots), "shot_times")
    for number, (t, projectile, origin) in enumerate(shots):
        if cadence:
            continue
        result[f"shot{number}.time"] = (t, "fire_time")
        for n in ranges:
            step = trace.at(t + n / 30.0)
            if step and projectile in step["objects"]:
                result[f"shot{number}.range{n}"] = (distance(step["objects"][projectile][:3], origin),
                                                    "instant_range")
        if impact:
            hits = hits_of(trace, projectile)
            if hits:
                result[f"shot{number}.impact"] = (hits[0][1], "impact_point")
                result[f"shot{number}.flight"] = (hits[0][0] - t, "impact_time")
    return result


def measure_grenades(trace):
    """each grenade's first landing (where, and how long after it was fired), where it lay when it went off, and its
    fuse (fired to gone off)"""
    result = {}
    booms = {words[0]: (t, tuple(float(x) for x in words[1:4])) for t, step, words in trace.events("boom")}
    for number, (t, projectile, origin) in enumerate(shots_of(trace)):
        hits = hits_of(trace, projectile)
        if hits:
            result[f"grenade{number}.landing"] = (hits[0][1], "grenade_impact")
            result[f"grenade{number}.flight"] = (hits[0][0] - t, "grenade_time")
        if projectile in booms:
            result[f"grenade{number}.rest"] = (booms[projectile][1], "rest_point")
            result[f"grenade{number}.fuse"] = (booms[projectile][0] - t, "fuse")
    return result


def measure_height_curve(trace, start=0.0, end=None, player=0):
    """the player's height at every 30th of a second from start to end: the curve, compared by its RMS"""
    curve = []
    n = round(start * 30)
    while end is None or n / 30.0 <= end + GRID_SLACK:
        step = trace.at(n / 30.0)
        if step is None:
            break
        if player in step["players"]:
            curve.append(step["players"][player][2])
        n += 1
    return {"height": (tuple(curve), "height_curve")}


def measure_ai_path(trace, start=0.0, end=None):
    """each of the AI's bipeds' place at every 30th of a second from start to end: its path, compared by the RMS of
    the distances between the rates' places; and how far it walked (so a path that never moves cannot pass by
    agreeing)"""
    paths = {}
    n = round(start * 30)
    while end is None or n / 30.0 <= end + GRID_SLACK:
        step = trace.at(n / 30.0)
        if step is None:
            break
        for unit, fields in step["ai"].items():
            paths.setdefault(unit, []).append(tuple(fields[:3]))
        n += 1
    # (and between the grid's times, on the ground: how far each step of a tick before its last is from where the
    # tick's straight line puts it, which a body moved only once a tick misses by the tick's share still to come)
    ticks = {}
    for step in trace.steps:
        if start - GRID_SLACK <= step["t"] <= (end if end is not None else math.inf) + GRID_SLACK:
            ticks.setdefault(step["tick"], []).append(step)
    off_line = {}
    for tick, steps in ticks.items():
        before = ticks.get(tick - 1)
        if len(steps) < 2 or not before:
            continue
        for index, step in enumerate(steps[:-1]):
            fraction = (index + 1) / len(steps)
            for unit, fields in step["ai"].items():
                a, b = before[-1]["ai"].get(unit), steps[-1]["ai"].get(unit)
                if a is None or b is None or fields[6] or a[6] or b[6]:
                    continue
                expected = tuple(x + (y - x) * fraction for x, y in zip(a[:3], b[:3]))
                off_line.setdefault(unit, []).append(distance(fields[:3], expected))
    result = {}
    for number, unit in enumerate(sorted(paths)):
        path = paths[unit]
        gaps = off_line.get(unit, [])
        result[f"ai{number}.path"] = (tuple(path), "ai_path")
        result[f"ai{number}.walked"] = (sum(distance(a, b) for a, b in zip(path, path[1:])), "ai_walked")
        result[f"ai{number}.between"] = (math.sqrt(sum(g * g for g in gaps) / len(gaps)) if gaps else 0.0,
                                         "ai_between")
    return result


def vehicle_steps(trace, player=0):
    """each step the player rides a vehicle: (t, its fields: place, velocity, forward, up, angular velocity; on the
    ground)"""
    steps = []
    for step in trace.steps:
        for rider, fields, ground in step.get("vehicles", {}).values():
            if rider == player:
                steps.append((step["t"], fields, ground))
    return steps


def on_grid(steps):
    return [s for s in steps if abs(s[0] * 30 - round(s[0] * 30)) < GRID_SLACK * 30]


def circle_radius(points):
    """the circle through points (x, y), least squares (Kasa's)"""
    n = len(points)
    mx = sum(p[0] for p in points) / n
    my = sum(p[1] for p in points) / n
    u = [p[0] - mx for p in points]
    v = [p[1] - my for p in points]
    suu, svv, suv = sum(a * a for a in u), sum(b * b for b in v), sum(a * b for a, b in zip(u, v))
    suuu, svvv = sum(a ** 3 for a in u), sum(b ** 3 for b in v)
    suvv, svuu = sum(a * b * b for a, b in zip(u, v)), sum(b * a * a for a, b in zip(u, v))
    det = suu * svv - suv * suv
    if abs(det) < 1e-12:
        return math.inf
    uc = (svv * (suuu + suvv) - suv * (svvv + svuu)) / (2 * det)
    vc = (suu * (svvv + svuu) - suv * (suuu + suvv)) / (2 * det)
    return math.sqrt(uc * uc + vc * vc + (suu + svv) / n)


# a struck vehicle's speed (world units a second) from which it is moving
CRASH_SPEED = 0.1


def measure_vehicle(trace, kind, start, end=None, player=0, window=2.0):
    """the vehicle the player drives, from start (seconds) to end (the trace's end): its path (where it is at every
    30th of a second, x, y and z, compared by RMS), and as the kind says:
    straight: its top speed (across the ground, at the end) and when it reaches half and 90% of it;
    turn: its turning circle (the circle through its last window seconds) and how fast it turns;
    jump: the highest it gets, and how low and high it rides in its suspension in the second after it next touches
    the ground; landing: the same but the highest (a drop's is where it leaves);
    flip: when it rolls past 60 degrees (its up's height under a half);
    crash: when the first vehicle no one rides starts to move (it is struck: a tenth of a unit a second), where it is a
    second later, and how fast the player's goes then"""
    steps = [s for s in vehicle_steps(trace, player) if s[0] >= start - GRID_SLACK and
             (end is None or s[0] <= end + GRID_SLACK)]
    grid = on_grid(steps)
    if not grid:
        return {}
    result = {"path": (tuple(c for _, fields, _ in grid for c in fields[0:3]), "vehicle_path")}
    if kind == "straight":
        speeds = [(t, math.hypot(fields[3], fields[4])) for t, fields, _ in grid]
        top = speeds[-1][1]
        result["top_speed"] = (top, "vehicle_speed")
        for level in (0.5, 0.9):
            for (t0, v0), (t1, v1) in zip(speeds, speeds[1:]):
                if v1 >= level * top > v0:
                    result[f"t{int(level * 100)}"] = (t0 + (level * top - v0) / (v1 - v0) * (t1 - t0) - start,
                                                      "vehicle_time")
                    break
    elif kind == "turn":
        last = [s for s in grid if s[0] >= grid[-1][0] - window - GRID_SLACK]
        result["turning_radius"] = (circle_radius([fields[0:2] for _, fields, _ in last]), "vehicle_turn")
        result["turn_rate"] = (abs(sum(fields[14] for _, fields, _ in last) / len(last)), "vehicle_turn")
    elif kind in ("jump", "landing"):
        # (on the grid: what a vehicle meets is the tick's, physics.c, whose end the grid is at every rate)
        airborne = next((index for index, s in enumerate(grid) if not s[2]), None)
        if airborne is None:
            return result
        landing = next((s for s in grid[airborne:] if s[2]), None)
        if kind == "jump":
            result["apex"] = (max(fields[2] for _, fields, _ in grid[airborne:]), "vehicle_height")
        if landing:
            after = [s for s in grid if landing[0] - GRID_SLACK <= s[0] <= landing[0] + 1.0]
            result["suspension_low"] = (min(fields[2] for _, fields, _ in after), "vehicle_height")
            result["suspension_high"] = (max(fields[2] for _, fields, _ in after[len(after) // 4:]),
                                         "vehicle_height")
    elif kind == "flip":
        flipped = next((t for t, fields, _ in steps if fields[11] < 0.5), None)
        if flipped is not None:
            result["roll_over"] = (flipped, "vehicle_flip")
    elif kind == "crash":
        # (on the grid: the struck vehicle, of the macro lane, moves once a tick, logged at its end at 30)
        struck = None
        for step in trace.steps:
            if step["t"] < start or struck is not None or abs(step["t"] * 30 - round(step["t"] * 30)) > 1e-3:
                continue
            for name, (rider, fields, ground) in step.get("vehicles", {}).items():
                if rider < 0 and math.hypot(fields[3], fields[4]) > CRASH_SPEED and struck is None:
                    struck = (step["t"], name, fields)
        if struck is not None:
            result["impact"] = (struck[0], "vehicle_impact")
            later = trace.at(struck[0] + 1.0)
            mine = [fields for t, fields, _ in grid if abs(t - struck[0] - 1.0) < GRID_SLACK]
            if later and struck[1] in later.get("vehicles", {}):
                result["struck_place"] = (tuple(later["vehicles"][struck[1]][1][0:3]), "vehicle_path")
            if mine:
                result["speed_after"] = (math.hypot(mine[0][3], mine[0][4]), "vehicle_speed")
    return result


MEASURES = {
    "vehicle": measure_vehicle,
    "jump": measure_jump,
    "jump_off_grid": measure_jump_off_grid,
    "shots": measure_shots,
    "grenades": measure_grenades,
    "height_curve": measure_height_curve,
    "ai_path": measure_ai_path,
}


def measure(trace, kind, arguments):
    return MEASURES[kind](trace, **(arguments or {}))


# ---------- two rates' measures compared


def tolerance(kind, rates):
    """how far apart two measures of a kind at two rates may be, and whether relatively"""
    rule = TOLERANCES[kind]
    if rule.get("exact"):
        return 0.0, False
    if "relative" in rule:
        return rule["relative"], True
    return rule["absolute"] + (1.0 / max(rates) if rule.get("step") else 0.0), False


def apart(a, b, kind):
    """how far apart two measures are: numbers by their difference, places by their distance, curves by the RMS of
    their differences (over the times both have)"""
    if TOLERANCES[kind]["unit"] == "wu rms":
        count = min(len(a), len(b))
        if not count or len(a) != len(b):
            return math.inf
        return math.sqrt(sum((distance(x, y) if isinstance(x, tuple) else x - y) ** 2 for x, y in zip(a, b)) / count)
    if TOLERANCES[kind]["unit"] == "s max":
        return max((abs(x - y) for x, y in zip(a, b)), default=0.0) if len(a) == len(b) else math.inf
    if isinstance(a, tuple):
        return distance(a, b)
    return abs(a - b)


def compare(a, b, kind, rates):
    """(how far apart, the tolerance, within it) of two rates' measures"""
    limit, relative = tolerance(kind, rates)
    gap = apart(a, b, kind)
    if relative:
        gap = gap / max(abs(a), abs(b), 1e-9)
    return gap, limit, gap <= limit + 1e-12
