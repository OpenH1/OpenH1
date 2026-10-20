"""Survey grenade spots for the lights clip: each candidate captured on both sides
at one frame a tick, scored for how differently the two light the blast and for
how well the loop closes, then its frames deleted."""
import itertools, json, os, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from PIL import Image

# usage: survey.py <map,map,...>  (RUNS holds the runs; capture.sh is beside this)
RUNS = os.environ.get('RUNS', os.path.expanduser('~/halo-capture-runs'))
import argparse
_p = argparse.ArgumentParser()
_p.add_argument('maps')
_p.add_argument('--seed', type=int, default=7)
_p.add_argument('--yaws', default='0,45,90,135,180,225,270,315')
_p.add_argument('--pitch', default='-5')
# the clip's event: two blasts ("boom", a grenade before the clip and one in
# it), or two bursts of fire ("fire": the last two runs of shots); the script
# throws or fires at the same spot twice, the clip's 4 s after the first
_p.add_argument('--event', default='boom', choices=['boom', 'fire'])
_p.add_argument('--script', default='0 aim={yaw},{pitch};1 throw;5 throw')
_p.add_argument('--console', default='')
_p.add_argument('--seconds', type=float, default=10.5)
_p.add_argument('--out', default='survey')
ARGS = _p.parse_args()
MAPS = ARGS.maps.split(',')
SEEDS = [ARGS.seed]
YAWS = [float(y) for y in ARGS.yaws.split(',')]
PITCHES = [float(x) for x in ARGS.pitch.split(',')]
SCRIPT = ARGS.script
OUT = f'{RUNS}/{ARGS.out}'

def capture(job):
    (m, seed, yaw, pitch), side, port = job
    name = f'{ARGS.out}/{m}-{seed}-{yaw:g}-{pitch:g}-{side}'
    env = dict(os.environ, PORT=str(port), SEED=str(seed), FT='0.0333333', TRACE_SECONDS=str(ARGS.seconds),
               CONSOLE=ARGS.console)
    r = subprocess.run([os.path.join(os.path.dirname(os.path.abspath(__file__)), 'capture.sh'), name, side, f'levels\\test\\{m}\\{m}',
                        SCRIPT.format(yaw=f'{yaw:g}', pitch=f'{pitch:g}'), '60'], env=env, capture_output=True, text=True)
    return r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr[-200:]

def luma(path, size=(640, 480)):
    im = Image.open(path).convert('L')
    if im.size != size:
        im = im.resize(size, Image.BOX)
    return np.asarray(im, dtype=np.float32)

def frames(d):
    return sorted(os.listdir(d))

def trace(path):
    """the steps' times, the player's place and shields at each, and the events:
    the blasts, or the bursts of fire (their first shots)"""
    steps, booms, shots = [], [], []
    for line in open(path):
        f = line.split()
        if f[0] == 's':
            steps.append([float(f[1]), None, None, int(f[2])])
        elif f[0] == 'p' and f[1] == '0' and steps:
            steps[-1][1] = np.array([float(x) for x in f[2:5]])
            steps[-1][2] = float(f[13]) + float(f[12])
        elif f[:2] == ['e', 'boom']:
            booms.append((steps[-1][0], np.array([float(x) for x in f[3:6]])))
        elif f[:2] == ['e', 'fire'] and f[2] == '0':
            shots.append(steps[-1][0])
    if ARGS.event == 'fire':
        booms = [(t, np.zeros(3)) for i, t in enumerate(shots) if i == 0 or t - shots[i - 1] > 0.5]
    return steps, booms

def score(c):
    m, seed, yaw, pitch = c
    base = f'{OUT}/{m}-{seed}-{yaw:g}-{pitch:g}'
    o, h = f'{base}-original/frames', f'{base}-openh1/frames'
    result = {'map': m, 'seed': seed, 'yaw': yaw, 'pitch': pitch}
    try:
        on, hn = frames(o), frames(h)
        so, booms = trace(f'{base}-original/data/trace.txt')
        sh, _ = trace(f'{base}-openh1/data/trace.txt')
        result['frames'] = [len(on), len(hn)]
        result['booms'] = len(booms)
        if len(booms) < 2:
            result['error'] = f'fewer than two {ARGS.event} events'; return result
        if any(st[1] is None for st in so) or any(st[1] is None for st in sh):
            result['error'] = 'the player died'; return result
        t2, where = booms[-1]
        step = lambda t: min(range(len(so)), key=lambda i: abs(so[i][0] - t))
        i2 = step(t2)
        before, after = so[step(t2 - 2.0)], so[min(step(t2 + 2.0), len(so) - 1)]
        result['distance'] = float(np.linalg.norm(where - before[1]))
        result['knock'] = float(np.linalg.norm(after[1] - before[1]))
        result['damage'] = float(before[2] - after[2])
        result['spot_gap'] = float(np.linalg.norm(booms[-1][1] - booms[-2][1]))
        # a frame a tick once the scenario runs: its frames are the last len(steps)
        # (a frame a tick: by the steps' ticks, of which a tick can have several)
        fo = lambda i: len(on) - (so[-1][3] + 1) + so[i][3]
        fh = lambda i: len(hn) - (sh[-1][3] + 1) + sh[min(i, len(sh) - 1)][3]
        b, hb = fo(i2), fh(i2)
        lit = 2 if ARGS.event == 'boom' else 9
        do = luma(f'{o}/{on[b + lit]}') - luma(f'{o}/{on[b - 8]}')
        dh = luma(f'{h}/{hn[hb + lit]}') - luma(f'{h}/{hn[hb - 8]}')
        result['light_diff'] = float(np.abs(do - dh).mean())
        result['false_light'] = float(((do - dh) > 20).mean())
        result['lit_original'] = float((do > 20).mean())
        # half a second before the clip's throw (a blast comes about 1.6 s
        # after it) or burst
        start = fo(step(t2 - (2.1 if ARGS.event == 'boom' else 0.5)))
        s = luma(f'{o}/{on[start]}')
        best = (-1, 0)
        for k in range(b + (40 if ARGS.event == 'boom' else 30), min(b + 100, len(on))):
            mse = float(((luma(f'{o}/{on[k]}') - s) ** 2).mean())
            best = max(best, (10 * np.log10(255 ** 2 / max(mse, 1e-6)), k))
        result['loop_psnr'], result['loop_end'] = best
        result['start'], result['blast'], result['blast_openh1'] = start, b, hb
        keep = f'{OUT}/keep/{m}-{seed}-{yaw:g}-{pitch:g}'; os.makedirs(keep, exist_ok=True)
        for tag, src in [('start', f'{o}/{on[start]}'), ('blast-original', f'{o}/{on[b + lit]}'),
                         ('blast-openh1', f'{h}/{hn[hb + lit]}'), ('end', f'{o}/{on[best[1]]}'),
                         ('before-openh1', f'{h}/{hn[hb - 8]}'), ('before-original', f'{o}/{on[b - 8]}')]:
            Image.open(src).convert('RGB').save(f'{keep}/{tag}.png')
    except Exception as e:
        result['error'] = repr(e)[:200]
    finally:
        for d in (f'{base}-original', f'{base}-openh1'):
            shutil.rmtree(f'{d}/frames', ignore_errors=True)
    return result

if __name__ == '__main__':
    os.makedirs(f'{OUT}/keep', exist_ok=True)
    cands = list(itertools.product(MAPS, SEEDS, YAWS, PITCHES))
    jobs = [(c, side) for c in cands for side in ('original', 'openh1')]
    pending = {c: 2 for c in cands}
    results = []
    with ThreadPoolExecutor(10) as games, ThreadPoolExecutor(6) as scorers:
        futures = {}
        for i, (c, side) in enumerate(jobs):
            futures[games.submit(capture, (c, side, 2300 + i))] = (c, side)
        scored = []
        for f in futures:
            c, side = futures[f]
            line = f.result()
            pending[c] -= 1
            if pending[c] == 0:
                scored.append(scorers.submit(score, c))
        for f in scored:
            r = f.result(); results.append(r)
            print(json.dumps(r), flush=True)
    json.dump(results, open(f'{OUT}/results.json', 'w'), indent=1)
