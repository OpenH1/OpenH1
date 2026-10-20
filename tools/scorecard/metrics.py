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
}

# a step's time against a time on the grid
GRID_SLACK = 1e-4


class Trace:
    """a trace read: its rate, mutation and steps; each step (t, tick, micro, players {index: fields}, objects
    {id: (x, y, z, vx, vy, vz)}, events [words])"""

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
                                events=[])
                    self.steps.append(step)
                elif step is None:
                    continue
                elif words[0] == "p":
                    step["players"][int(words[1])] = [float(word) for word in words[2:12]]
                elif words[0] == "o":
                    step["objects"][words[1]] = tuple(float(word) for word in words[2:8])
                elif words[0] == "e":
                    step["events"].append(words[1:])
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


MEASURES = {
    "jump": measure_jump,
    "shots": measure_shots,
    "grenades": measure_grenades,
    "height_curve": measure_height_curve,
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
        return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)) / count)
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
