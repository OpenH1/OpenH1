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

def match_env(map_variant, seed, a, index):
    """the settings of a match: the same for the scout and the shoot. A match
    is "<map>:<variant>[+<loadout>]": the loadout a weapon the player's bot
    goes for ("sniper", "rocket") or a vehicle it boards ("warthog", "ghost")"""
    map_variant, _, loadout = map_variant.partition('+')
    extra = {}
    if loadout:
        extra['HALO_BOT_PLAYER_VEHICLE' if loadout in VEHICLES else 'HALO_BOT_PLAYER_WEAPON'] = loadout
    # (a score no one reaches: the game ends at its score limit, after which
    # the simulation's clock stops short of the trace's end)
    return {**extra, 'NETTEST': f'local:{map_variant}', 'HALO_NETWORK_TEST_START': '2', 'HALO_NETWORK_TEST_SCORE': '500',
            'HALO_NET_ADDRESS': f'127.0.{index // 250}.{index % 250 + 2}', 'HALO_NET_BROADCAST': '127.255.255.255',
            'HALO_BOTS': str(a.bots), 'HALO_BOT_SKILL': a.skill, 'HALO_BOT_PLAYER': '1', 'SEED': str(seed),
            # (multiplayer keeps the whole HUD: the trailer's firefights show it)
            'HIDE_HUD': '0'}

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
            elif f[0] == 'v' and f[1] == '0':
                driving.add(round(now * 30))
            elif f[0] == 'e' and f[1] in ('weapon', 'wstate', 'zoom', 'act') and f[2] == '0':
                # (the player's: a weapon's tag has spaces)
                weapon_events.append((round(now, 4), f[1], ' '.join(f[3:])))
    except FileNotFoundError:
        return {'match': name, 'error': 'no trace'}
    kill_times = [seconds[i] for i in range(1, len(kills)) for _ in range(max(0, kills[i] - kills[i - 1]))]
    death_times = [seconds[i] for i in range(1, len(deaths)) if deaths[i] > deaths[i - 1]]

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
                'rocket' if 'rocket' in weapon else 'chaos' if crowd >= 3 or near >= 2 else 'kill')
        score = 10 * watched + 3 * min(crowd, 2) + 4 * near + (4 if seen_kill else 0) + min(fired, 30) / 10 - (6 if died else 0)
        score += {'vehicle': 8, 'snipe': 8, 'rocket': 8, 'chaos': 4}.get(kind, 0)
        moments.append({'match': name, 'map': map_variant, 'seed': seed, 'kill': t, 'kind': kind, 'weapon': weapon,
                        'watched': round(watched, 2),
                        'crowd': round(crowd, 2), 'near_kills': near, 'seen_kill': seen_kill, 'shots': fired,
                        'died': died, 'score': round(score, 1)})
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
        print(f"{m['score']:5}  {m['kind']:7} {m['match']:28} kill at {m['kill']:6.1f}s  watched {m['watched']:.2f} crowd {m['crowd']:.1f} "
              f"near {m['near_kills']} seen {int(m['seen_kill'])} {'died' if m['died'] else ''}")

if __name__ == '__main__':
    main()
