"""Scout a multiplayer match for the trailer: local games with bots, the first
player played by a bot of its own (debug.bot_player), run without saving a
frame (seconds a match), and the moments worth filming found from the network
test's log and the sim trace: each of the player's kills, scored.

usage: scout.py <map:variant,...> [--seeds 7,8] [--bots 11] [--skill legendary] [--seconds 180] [--out scout]

A moment is a kill of the player's (its count rises in the network test's
line, each second), scored by the kills near it (a double kill, a spree), the
shots the player fired and the blasts within 15 world units of it in the four
seconds before, less the player's death soon after (the camera leaves the
body). The matches play the same again, so shoot.py films a moment by running
its match again with debug.screenshot_from and debug.screenshot_until."""
import argparse, json, os, re, subprocess
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
RUNS = os.environ.get('RUNS', os.path.expanduser('~/halo-capture-runs'))
LINE = re.compile(r'network test: tick (\d+) player 0: (?:\((-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\)|dead).*? k(-?\d+) d(-?\d+)')

def match_env(map_variant, seed, a, index):
    """the settings of a match: the same for the scout and the shoot"""
    # (a score no one reaches: the game ends at its score limit, after which
    # the simulation's clock stops short of the trace's end)
    return {'NETTEST': f'local:{map_variant}', 'HALO_NETWORK_TEST_START': '2', 'HALO_NETWORK_TEST_SCORE': '500',
            'HALO_NET_ADDRESS': f'127.0.{index // 250}.{index % 250 + 2}', 'HALO_NET_BROADCAST': '127.255.255.255',
            'HALO_BOTS': str(a.bots), 'HALO_BOT_SKILL': a.skill, 'HALO_BOT_PLAYER': '1', 'SEED': str(seed)}

def scout(job):
    (map_variant, seed), a, index = job
    name = f"{map_variant.replace(':', '-')}-{seed}"
    env = dict(os.environ, **match_env(map_variant, seed, a, index), PORT=str(3400 + index),
               RUNS=f'{RUNS}/{a.out}', SIZE='640x360', TRACE_SECONDS=str(a.seconds), HALO_SCREENSHOT_FROM='999999')
    subprocess.run([f'{HERE}/capture.sh', name, 'openh1', '-', '', str(a.seconds + 120)], env=env,
                   capture_output=True, text=True)
    base = f'{RUNS}/{a.out}/{name}'
    seconds, kills, deaths, places = [], [], [], []
    for line in open(f'{base}/log.txt', errors='replace'):
        m = LINE.search(line)
        if m:
            seconds.append(int(m.group(1)) / 30)
            places.append(tuple(float(x) for x in m.group(2, 3, 4)) if m.group(2) else None)
            kills.append(int(m.group(5))); deaths.append(int(m.group(6)))
    # the sim trace: the player's place each step, its shots and the blasts
    player, shots, booms = {}, [], []
    now = 0.0
    try:
        for line in open(f'{base}/data/trace.txt'):
            f = line.split()
            if f[0] == 's':
                now = float(f[1])
            elif f[0] == 'p' and f[1] == '0':
                player[round(now * 30)] = tuple(float(x) for x in f[2:5])
            elif f[:3] == ['e', 'fire', '0']:
                shots.append(now)
            elif f[:2] == ['e', 'boom']:
                booms.append((now, tuple(float(x) for x in f[3:6])))
    except FileNotFoundError:
        return {'match': name, 'error': 'no trace'}
    kill_times = [seconds[i] for i in range(1, len(kills)) for _ in range(max(0, kills[i] - kills[i - 1]))]
    death_times = [seconds[i] for i in range(1, len(deaths)) if deaths[i] > deaths[i - 1]]
    moments = []
    for t in kill_times:
        near = sum(1 for u in kill_times if 0 < abs(u - t) <= 4)
        fired = sum(1 for u in shots if t - 4 <= u <= t)
        blasts = 0
        for u, (x, y, z) in booms:
            p = player.get(round(u * 30))
            if t - 4 <= u <= t + 1 and p and (p[0] - x) ** 2 + (p[1] - y) ** 2 + (p[2] - z) ** 2 < 225:
                blasts += 1
        died = any(t - 0.5 <= u <= t + 2 for u in death_times)
        score = 3 + 4 * near + min(fired, 30) / 6 + 2 * blasts - (5 if died else 0)
        moments.append({'match': name, 'map': map_variant, 'seed': seed, 'kill': t, 'near_kills': near,
                        'shots': fired, 'blasts': blasts, 'died': died, 'score': round(score, 1)})
    return {'match': name, 'kills': kills[-1] if kills else 0, 'deaths': deaths[-1] if deaths else 0,
            'moments': moments}

def main():
    p = argparse.ArgumentParser()
    p.add_argument('maps'); p.add_argument('--seeds', default='7')
    p.add_argument('--bots', type=int, default=11); p.add_argument('--skill', default='legendary')
    p.add_argument('--seconds', type=float, default=180); p.add_argument('--out', default='scout')
    p.add_argument('--parallel', type=int, default=1)  # (local games side by side slow each other)
    a = p.parse_args()
    jobs = [((m, int(s)), a, i) for i, (m, s) in enumerate((m, s) for m in a.maps.split(',') for s in a.seeds.split(','))]
    with ThreadPoolExecutor(a.parallel) as pool:
        results = list(pool.map(scout, jobs))
    moments = sorted((m for r in results for m in r.get('moments', [])), key=lambda m: -m['score'])
    os.makedirs(f'{RUNS}/{a.out}', exist_ok=True)
    json.dump({'args': vars(a), 'matches': [{k: v for k, v in r.items() if k != 'moments'} for r in results],
               'moments': moments}, open(f'{RUNS}/{a.out}/moments.json', 'w'), indent=1)
    for r in results:
        print(r['match'], r.get('error') or f"k{r['kills']} d{r['deaths']} moments {len(r['moments'])}")
    for m in moments[:15]:
        print(f"{m['score']:5}  {m['match']:28} kill at {m['kill']:6.1f}s  near {m['near_kills']} shots {m['shots']:3} "
              f"blasts {m['blasts']} {'died' if m['died'] else ''}")

if __name__ == '__main__':
    main()
