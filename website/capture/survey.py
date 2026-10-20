"""Survey grenade spots for the lights clip: each candidate captured on both sides
at one frame a tick, scored for how differently the two light the blast and for
how well the loop closes, then its frames deleted."""
import itertools, json, os, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from PIL import Image

# usage: survey.py <map,map,...>  (RUNS holds the runs; capture.sh is beside this)
RUNS = os.environ.get('RUNS', os.path.expanduser('~/halo-capture-runs'))
MAPS = sys.argv[1].split(',')
SEEDS = [7]
YAWS = [0, 45, 90, 135, 180, 225, 270, 315]
SCRIPT = '0 aim={yaw},-5;1 throw;5 throw'
OUT = f'{RUNS}/survey'

def capture(job):
    (m, seed, yaw), side, port = job
    name = f'survey/{m}-{seed}-{yaw}-{side}'
    env = dict(os.environ, PORT=str(port), SEED=str(seed), FT='0.0333333', TRACE_SECONDS='10.5')
    r = subprocess.run([os.path.join(os.path.dirname(os.path.abspath(__file__)), 'capture.sh'), name, side, f'levels\\test\\{m}\\{m}',
                        SCRIPT.format(yaw=yaw), '40'], env=env, capture_output=True, text=True)
    return r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr[-200:]

def luma(path, size=(640, 480)):
    im = Image.open(path).convert('L')
    if im.size != size:
        im = im.resize(size, Image.BOX)
    return np.asarray(im, dtype=np.float32)

def frames(d):
    return sorted(os.listdir(d))

def trace(path):
    """the steps' times, the player's place and shields at each, and the blasts"""
    steps, booms = [], []
    for line in open(path):
        f = line.split()
        if f[0] == 's':
            steps.append([float(f[1]), None, None, int(f[2])])
        elif f[0] == 'p' and f[1] == '0' and steps:
            steps[-1][1] = np.array([float(x) for x in f[2:5]])
            steps[-1][2] = float(f[13]) + float(f[12])
        elif f[:2] == ['e', 'boom']:
            booms.append((steps[-1][0], np.array([float(x) for x in f[3:6]])))
    return steps, booms

def score(c):
    m, seed, yaw = c
    base = f'{OUT}/{m}-{seed}-{yaw}'
    o, h = f'{base}-original/frames', f'{base}-openh1/frames'
    result = {'map': m, 'seed': seed, 'yaw': yaw}
    try:
        on, hn = frames(o), frames(h)
        so, booms = trace(f'{base}-original/data/trace.txt')
        sh, _ = trace(f'{base}-openh1/data/trace.txt')
        result['frames'] = [len(on), len(hn)]
        result['booms'] = len(booms)
        if len(booms) < 2:
            result['error'] = 'fewer than two blasts'; return result
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
        do = luma(f'{o}/{on[b + 2]}') - luma(f'{o}/{on[b - 8]}')
        dh = luma(f'{h}/{hn[hb + 2]}') - luma(f'{h}/{hn[hb - 8]}')
        result['light_diff'] = float(np.abs(do - dh).mean())
        result['false_light'] = float(((do - dh) > 20).mean())
        result['lit_original'] = float((do > 20).mean())
        start = fo(step(5.0 - 0.5))
        s = luma(f'{o}/{on[start]}')
        best = (-1, 0)
        for k in range(b + 40, min(b + 100, len(on))):
            mse = float(((luma(f'{o}/{on[k]}') - s) ** 2).mean())
            best = max(best, (10 * np.log10(255 ** 2 / max(mse, 1e-6)), k))
        result['loop_psnr'], result['loop_end'] = best
        result['start'], result['blast'], result['blast_openh1'] = start, b, hb
        keep = f'{OUT}/keep/{m}-{seed}-{yaw}'; os.makedirs(keep, exist_ok=True)
        for tag, src in [('start', f'{o}/{on[start]}'), ('blast-original', f'{o}/{on[b + 2]}'),
                         ('blast-openh1', f'{h}/{hn[hb + 2]}'), ('end', f'{o}/{on[best[1]]}'),
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
    cands = list(itertools.product(MAPS, SEEDS, YAWS))
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
