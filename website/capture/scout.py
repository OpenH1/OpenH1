"""Scout a multiplayer match for the trailer: local games with bots, the first
player played by a bot of its own (debug.bot_player), run without saving a
frame (seconds a match), and the moments worth filming found from the network
test's log and the sim trace: each of the player's kills, scored.

usage: scout.py <map:variant,...> [--seeds 7,8] [--bots 11] [--skill legendary] [--seconds 180] [--out scout]

A moment is a kill of the player's (its count rises in the network test's
line, each second), scored from the sim trace by how much of its window has
an enemy in view (within 28 degrees of the player's aim, 3 to 30 world units
away) and how many, whether one was in view just before the kill, the kills
near it (a double kill, a spree) and the shots fired, less the player's death
soon after (the camera leaves the body). The matches play the same again, so shoot.py films a moment by running
its match again with debug.screenshot_from and debug.screenshot_until."""
import argparse, json, os, re, subprocess
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
RUNS = os.environ.get('RUNS', os.path.expanduser('~/halo-capture-runs'))
LINE = re.compile(r'network test: tick (\d+) player 0: (?:\((-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\)|dead).*? k(-?\d+) d(-?\d+)')

VEHICLES = ('warthog', 'ghost', 'scorpion', 'banshee', 'rwarthog')
# a round that struck this far above its victim's place (its feet) or more hit
# the head (world units): of 155 sniper rounds aimed at heads and striking
# players, most struck 0.5 to 0.7 above the feet, the body's few 0.25 to 0.45
HEAD_HEIGHT = 0.52

def match_env(map_variant, seed, a, index):
    """the settings of a match: the same for the scout and the shoot. A match
    is "<map>:<variant>[+<loadout>]": the loadout a weapon the player's bot
    spawns with ("sniper", "rocket", "needler"), a vehicle it boards
    ("warthog", "ghost"), or "plasma" for plasma grenades thrown for sticks"""
    map_variant, _, loadout = map_variant.partition('+')
    extra = {}
    if loadout == 'plasma':
        extra['HALO_BOT_PLAYER_GRENADES'] = 'plasma'
    elif loadout:
        extra['HALO_BOT_PLAYER_VEHICLE' if loadout in VEHICLES else 'HALO_BOT_PLAYER_WEAPON'] = loadout
    # (a score no one reaches: the game ends at its score limit, after which
    # the simulation's clock stops short of the trace's end)
    return {**extra, 'NETTEST': f'local:{map_variant}', 'HALO_NETWORK_TEST_START': '2', 'HALO_NETWORK_TEST_SCORE': '500',
            'HALO_NET_ADDRESS': f'127.0.{index // 250}.{index % 250 + 2}', 'HALO_NET_BROADCAST': '127.255.255.255',
            'HALO_BOTS': str(a.bots), 'HALO_BOT_SKILL': a.skill, 'HALO_BOT_PLAYER': '1', 'SEED': str(seed),
            # (multiplayer keeps the whole HUD: the trailer's firefights show it;
            # but not the port's names over every player's head, only the stock
            # game's of the one under the crosshair)
            'HIDE_HUD': '0', 'HALO_PLAYER_NAMES': 'none',
            # (each match in a network of its own: they run side by side)
            'PRIVATE_NET': '1'}

def scout(job):
    (map_variant, seed), a, index = job
    name = f"{map_variant.replace(':', '-').replace('+', '-')}-{seed}"
    env = dict(os.environ, **match_env(map_variant, seed, a, index), PORT=str(3400 + index),
               RUNS=f'{RUNS}/{a.out}', SIZE='640x360', TRACE_SECONDS=str(a.seconds), HALO_SCREENSHOT_FROM='999999')
    base = f'{RUNS}/{a.out}/{name}'
    if not (a.rescore and os.path.exists(f'{base}/data/trace.txt')):
        subprocess.run([f'{HERE}/capture.sh', name, 'openh1', '-', '', str(a.seconds + 120)], env=env,
                       capture_output=True, text=True)
    seconds, kills, deaths, places = [], [], [], []
    for line in open(f'{base}/log.txt', errors='replace'):
        m = LINE.search(line)
        if m:
            seconds.append(int(m.group(1)) / 30)
            places.append(tuple(float(x) for x in m.group(2, 3, 4)) if m.group(2) else None)
            kills.append(int(m.group(5))); deaths.append(int(m.group(6)))
    # the sim trace: each player's place, aim and body each tick, the
    # player's shots
    ticks, shots, weapon_events, driving = {}, [], [], set()
    owned, hits, booms = {}, {}, []
    now = 0.0
    try:
        for line in open(f'{base}/data/trace.txt'):
            f = line.split()
            if f[0] == 's':
                now = float(f[1])
            elif f[0] == 'p':
                ticks.setdefault(round(now * 30), {})[int(f[1])] = (
                    tuple(float(x) for x in f[2:5]), tuple(float(x) for x in f[8:11]), float(f[12]))
            elif f[:3] == ['e', 'fire', '0']:
                shots.append(now)
            elif f[:2] == ['e', 'new']:
                # (a projectile: its tag's name has spaces, its owner is last)
                owned[f[2]] = (' '.join(f[3:-7]), int(f[-1]), now)
            elif f[:2] == ['e', 'hit']:
                hits.setdefault(f[2], []).append((now, tuple(float(x) for x in f[4:7])))
            elif f[:2] == ['e', 'boom']:
                booms.append((now, f[2], tuple(float(x) for x in f[3:6])))
            elif f[0] == 'v' and f[1] == '0':
                driving.add(round(now * 30))
            elif f[0] == 'e' and f[1] in ('weapon', 'wstate', 'zoom', 'act') and f[2] == '0':
                # (the player's: a weapon's tag has spaces)
                weapon_events.append((round(now, 4), f[1], ' '.join(f[3:])))
    except FileNotFoundError:
        return {'match': name, 'error': 'no trace'}
    kill_times = [seconds[i] for i in range(1, len(kills)) for _ in range(max(0, kills[i] - kills[i - 1]))]
    death_times = [seconds[i] for i in range(1, len(deaths)) if deaths[i] > deaths[i - 1]]
    # (the log counts kills each second, up to a second late: each kill's tick
    # is a victim's, the first not taken that the player looked at, whose body
    # emptied or was gone in the second before its line)
    def facing(k, index):
        """how nearly the player looks at that one in the tick (a cosine), -1
        for none"""
        players = ticks.get(k, {})
        if 0 not in players or index not in players:
            return -1.0
        (x, y, z), (ax, ay, az), _ = players[0]
        (ox, oy, oz), _, _ = players[index]
        d = max(1e-3, ((ox - x) ** 2 + (oy - y) ** 2 + (oz - z) ** 2) ** 0.5)
        return ((ox - x) * ax + (oy - y) * ay + (oz - z) * az) / d

    fallen = []
    for k in sorted(ticks):
        for index, (_, _, body) in ticks.get(k - 1, {}).items():
            now_p = ticks[k].get(index)
            if index and body > 0 and (now_p is None or now_p[2] <= 0) and facing(k - 1, index) > 0.8:
                fallen.append(k / 30)
    taken = set()
    exact = []
    for t in kill_times:
        u = next((u for u in fallen if t - 1.05 < u <= t + 0.05 and u not in taken), None)
        if u is not None:
            taken.add(u)
        exact.append(round(u, 4) if u is not None else t)
    kill_times = exact

    def in_view(tick):
        """the enemies the player looks at in this tick: within 28 degrees of its
        aim, 3 to 30 world units away, alive"""
        players = ticks.get(tick, {})
        if 0 not in players:
            return []
        (x, y, z), (ax, ay, az), _ = players[0]
        seen = []
        for index, ((ox, oy, oz), _, body) in players.items():
            dx, dy, dz = ox - x, oy - y, oz - z
            d = (dx * dx + dy * dy + dz * dz) ** 0.5
            if index == 0 or body <= 0 or not 3 <= d <= 30:
                continue
            if (dx * ax + dy * ay + dz * az) / d > 0.883:  # cos 28 degrees
                seen.append((index, d))
        return seen

    # (each kill's chain: the kills before and after it, each within 4 seconds
    # of the next - a double, a triple - shown whole)
    def chain(t):
        first = last = t
        for u in sorted(kill_times, reverse=True):
            if first - 4 <= u < first:
                first = u
        for u in sorted(kill_times):
            if last < u <= last + 4:
                last = u
        return first, last

    moments = []
    for t in kill_times:
        window = range(round((t - a.before) * 30), round((t + a.after) * 30))
        views = [in_view(k) for k in window]
        watched = sum(1 for v in views if v) / max(1, len(views))
        crowd = sum(len(v) for v in views) / max(1, len(views))
        near = sum(1 for u in kill_times if 0 < abs(u - t) <= 4)
        fired = sum(1 for u in shots if t - 4 <= u <= t)
        died = any(t - 0.5 <= u <= t + 2 for u in death_times)
        # the kill seen: an enemy in view in the second before it
        seen_kill = any(in_view(k) for k in range(round((t - 1) * 30), round(t * 30) + 1))
        # what it was: the weapon in hand and the zoom at the kill, a vehicle
        weapon = next((v for u, k, v in reversed(weapon_events) if k == 'weapon' and u <= t), '-')
        zoomed = next((int(v) for u, k, v in reversed(weapon_events) if k == 'zoom' and u <= t), -1) >= 0
        in_vehicle = any(k in driving for k in range(round((t - 1) * 30), round(t * 30) + 1))
        kind = ('vehicle' if in_vehicle else 'snipe' if 'sniper' in weapon and zoomed else
                'rocket' if 'rocket' in weapon else 'needler' if 'needler' in weapon else
                'chaos' if crowd >= 3 or near >= 2 else 'kill')
        score = 10 * watched + 3 * min(crowd, 2) + 4 * near + (4 if seen_kill else 0) + min(fired, 30) / 10 - (6 if died else 0)
        score += {'vehicle': 8, 'snipe': 8, 'rocket': 8, 'needler': 8, 'chaos': 4}.get(kind, 0)
        first, last = chain(t)
        moments.append({'match': name, 'map': map_variant, 'seed': seed, 'kill': t, 'kind': kind, 'weapon': weapon,
                        'first': first, 'last': last, 'kills': sum(1 for u in kill_times if first <= u <= last),
                        'watched': round(watched, 2),
                        'crowd': round(crowd, 2), 'near_kills': near, 'seen_kill': seen_kill, 'shots': fired,
                        'died': died, 'score': round(score, 1)})
    def dies(index, t0, t1):
        """the player's body gone or emptied between t0 and t1"""
        for k in range(round(t0 * 30), round(t1 * 30) + 1):
            p = ticks.get(k, {})
            if index not in p or p[index][2] <= 0:
                return index in ticks.get(round(t0 * 30) - 1, {})
        return False

    # a rocket of the player's whose blast two or more die in: its launch,
    # flight and the group thrown
    for t, pid, (x, y, z) in booms:
        tag, owner, launched = owned.get(pid, ('', -1, 0))
        if owner != 0 or 'rocket' not in tag:
            continue
        near = [i for i, (pos, _, body) in ticks.get(round(t * 30) - 1, {}).items()
                if i != 0 and body > 0 and sum((a - b) ** 2 for a, b in zip(pos, (x, y, z))) < 36]
        dead = [i for i in near if dies(i, t, t + 0.6)]
        if len(dead) >= 2:
            moments.append({'match': name, 'map': map_variant, 'seed': seed, 'kill': round(t, 3), 'kind': 'rocket_multi',
                            'weapon': tag, 'dead': len(dead), 'flight': round(t - launched, 2),
                            'score': round(30 + 10 * len(dead), 1)})
    # a plasma grenade of the player's stuck to its victim: its blast on a
    # player (within 0.8 world units) who dies in the 0.3 s after; the throw
    # and the flight in the window before
    for t, pid, (x, y, z) in booms:
        tag, owner, launched = owned.get(pid, ('', -1, 0))
        if owner != 0 or 'plasma grenade' not in tag:
            continue
        stuck = [i for i, (pos, _, body) in ticks.get(round(t * 30) - 1, {}).items()
                 if i != 0 and body > 0 and sum((a - b) ** 2 for a, b in zip(pos, (x, y, z))) < 0.64 and dies(i, t, t + 0.3)]
        if stuck:
            moments.append({'match': name, 'map': map_variant, 'seed': seed, 'kill': round(t, 3), 'kind': 'sticky',
                            'weapon': tag, 'first': round(launched, 3), 'last': round(t, 3), 'kills': len(stuck),
                            'flight': round(t - launched, 2), 'score': round(40 + 10 * len(stuck), 1)})
    # a zoomed sniper kill's round: where it struck its victim, above its feet
    for m in moments:
        if m['kind'] != 'snipe':
            continue
        rounds = [(launched, pid) for pid, (tag, owner, launched) in owned.items()
                  if owner == 0 and 'sniper' in tag and m['kill'] - 1.5 <= launched <= m['kill'] + 0.1]
        for launched, pid in sorted(rounds, reverse=True)[:1]:
            for u, (hx, hy, hz) in hits.get(pid, []):
                for i, (pos, _, body) in ticks.get(round(u * 30) - 1, {}).items():
                    if i and (pos[0] - hx) ** 2 + (pos[1] - hy) ** 2 < 0.36:
                        m['hit_height'] = round(hz - pos[2], 3)
        if m.get('hit_height', 0) > HEAD_HEIGHT:
            m['kind'] = 'headshot'
            m['score'] += 10
    moments.sort(key=lambda m: -m['score'])
    return {'match': name, 'kills': kills[-1] if kills else 0, 'deaths': deaths[-1] if deaths else 0,
            'moments': moments, 'weapon_events': weapon_events}

def main():
    p = argparse.ArgumentParser()
    p.add_argument('maps'); p.add_argument('--seeds', default='7')
    p.add_argument('--bots', type=int, default=11); p.add_argument('--skill', default='legendary')
    p.add_argument('--seconds', type=float, default=180); p.add_argument('--out', default='scout')
    p.add_argument('--before', type=float, default=3.5); p.add_argument('--after', type=float, default=1.2)
    p.add_argument('--parallel', type=int, default=1)
    # (the matches' traces and logs kept: scored again, not played again)
    p.add_argument('--rescore', action='store_true')  # (local games side by side slow each other)
    a = p.parse_args()
    jobs = [((m, int(s)), a, i) for i, (m, s) in enumerate((m, s) for m in a.maps.split(',') for s in a.seeds.split(','))]
    with ThreadPoolExecutor(a.parallel) as pool:
        results = list(pool.map(scout, jobs))
    moments = sorted((m for r in results for m in r.get('moments', [])), key=lambda m: -m['score'])
    os.makedirs(f'{RUNS}/{a.out}', exist_ok=True)
    # (each match's weapon events, the player's: the trailer's cuts on the
    # same action are found from them)
    json.dump({'args': vars(a), 'matches': [{k: v for k, v in r.items() if k != 'moments'} for r in results],
               'moments': moments}, open(f'{RUNS}/{a.out}/moments.json', 'w'), indent=1)
    for r in results:
        print(r['match'], r.get('error') or f"k{r['kills']} d{r['deaths']} moments {len(r['moments'])}")
    for m in moments[:15]:
        detail = (f"dead {m['dead']} flight {m['flight']}s" if m['kind'] == 'rocket_multi' else
                  f"watched {m['watched']:.2f} crowd {m['crowd']:.1f} near {m['near_kills']} seen {int(m['seen_kill'])}"
                  f"{' died' if m['died'] else ''}{' hit ' + str(m['hit_height']) if 'hit_height' in m else ''}")
        print(f"{m['score']:5}  {m['kind']:12} {m['match']:32} kill at {m['kill']:6.1f}s  {detail}")

if __name__ == '__main__':
    main()
