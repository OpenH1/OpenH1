"""Film the scout's best moments (scout.py) for the trailer: each one's match
played again, its frames saved only around the moment (debug.screenshot_from
and debug.screenshot_until, seconds of simulation), at 4K and 60 frames a
second, encoded as a master clip, the frames then deleted.

usage: shoot.py <scout folder> [--count 8] [--before 3.5] [--after 1.2] [--size 3840x2160]

The moments are taken best first, at most two from a match and three from a
map, none overlapping another."""
import argparse, json, os, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
RUNS = os.environ.get('RUNS', os.path.expanduser('~/halo-capture-runs'))
sys.path.insert(0, HERE)
from scout import match_env  # the same match as the scout's

def main():
    p = argparse.ArgumentParser()
    p.add_argument('scout'); p.add_argument('--count', type=int, default=8)
    p.add_argument('--before', type=float, default=3.5); p.add_argument('--after', type=float, default=1.2)
    p.add_argument('--size', default='3840x2160')
    # (the most of a kind - snipe, rocket, vehicle, chaos, kill - so the cut has each)
    p.add_argument('--per-kind', type=int, default=4)
    p.add_argument('--parallel', type=int, default=4)
    # (the previews' numbers to film again, at --size: the picks, with the
    # previews' --count and --per-kind so that the numbers are the same)
    p.add_argument('--only')
    p.add_argument('--out', default='clips')
    a = p.parse_args()
    found = json.load(open(f'{RUNS}/{a.scout}/moments.json'))
    scouted = argparse.Namespace(**found['args'])
    chosen, per_match, per_map, per_kind = [], {}, {}, {}
    for m in found['moments']:
        if len(chosen) >= a.count:
            break
        if per_match.get(m['match'], 0) >= 2 or per_map.get(m['map'], 0) >= 3 or \
                per_kind.get(m.get('kind'), 0) >= a.per_kind:
            continue
        # (none of the same chain or overlapping another's window)
        if any(c['match'] == m['match'] and (abs(c['kill'] - m['kill']) < a.before + a.after or
                                             c.get('first') == m.get('first')) for c in chosen):
            continue
        chosen.append(m)
        per_match[m['match']] = per_match.get(m['match'], 0) + 1
        per_map[m['map']] = per_map.get(m['map'], 0) + 1
        per_kind[m.get('kind')] = per_kind.get(m.get('kind'), 0) + 1
    out = f'{RUNS}/{a.scout}/{a.out}'
    os.makedirs(out, exist_ok=True)
    jobs = list(enumerate(chosen, 1))
    if a.only:
        jobs = [(i, m) for i, m in jobs if f'{i:02d}' in a.only.split(',')]
    with ThreadPoolExecutor(a.parallel) as pool:
        for line in pool.map(lambda job: film(job, a, scouted, out), jobs):
            print(line, flush=True)

def film(job, a, scouted, out):
    """one moment's match played again, its frames saved around the moment and
    encoded, the frames deleted"""
    index, m = job
    name = f"{index:02d}-{m.get('kind', 'kill')}-{m['match']}-{m['kill']:.0f}s"
    # (a chain of kills - a double, a triple - from before the first to
    # after the last; a rocket's blast with the bodies in the air after it)
    first, last = m.get('first', m['kill']), m.get('last', m['kill'])
    start = min(m['kill'] - a.before, first - 1.5)
    end = max(m['kill'] + a.after, last + 1.0) + (0.8 if m.get('kind') == 'rocket_multi' else 0.0)
    # (a preview's frames in memory, /dev/shm: a 4K clip's, ten gigabytes, on
    # the disk)
    width = int(a.size.split('x')[0])
    runs = f'/dev/shm/halo-shoot/{a.scout}' if width <= 1920 and os.path.isdir('/dev/shm') else f'{RUNS}/{a.scout}/shoot'
    env = dict(os.environ, **match_env(m['map'], m['seed'], scouted, 0), PORT='3600', SIZE=a.size,
               RUNS=runs, TRACE_SECONDS=f'{end + 0.3:.2f}',
               HALO_SCREENSHOT_FROM=f'{start:.3f}', HALO_SCREENSHOT_UNTIL=f'{end:.3f}')
    subprocess.run([f'{HERE}/capture.sh', name, 'openh1', '-', '', str(int(end) + 180)], env=env,
                   capture_output=True, text=True)
    frames = f'{runs}/{name}/frames'
    names = sorted(os.listdir(frames)) if os.path.isdir(frames) else []
    if not names:
        return f'{name}: no frames'
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-framerate', '60', '-start_number',
                    names[0][5:10], '-i', f'{frames}/frame%05d.bmp', '-frames:v', str(len(names)),
                    # (the GPU's encoder: many times x264's speed at 4K, and
                    # the processors left to the games)
                    '-an', '-c:v', 'h264_nvenc', '-preset', 'p5', '-rc', 'vbr', '-cq', '17', '-b:v', '0',
                    '-pix_fmt', 'yuv420p',
                    '-movflags', '+faststart', f'{out}/{name}.mp4'], check=True)
    shutil.rmtree(f'{runs}/{name}')
    # (where in the clip its kills fall, for the cut)
    json.dump({'start': start, 'kill': m['kill'], 'first': first, 'last': last, 'kind': m.get('kind')},
              open(f'{out}/{name}.json', 'w'))
    return f"{name}: {len(names)} frames, score {m['score']}"

if __name__ == '__main__':
    main()
