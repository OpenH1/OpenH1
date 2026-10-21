"""Film the scout's best moments (scout.py) for the trailer: each one's match
played again, its frames saved only around the moment (debug.screenshot_from
and debug.screenshot_until, seconds of simulation), at 4K and 60 frames a
second, encoded as a master clip, the frames then deleted.

usage: shoot.py <scout folder> [--count 8] [--before 3.5] [--after 1.2] [--size 3840x2160]

The moments are taken best first, at most two from a match and three from a
map, none overlapping another."""
import argparse, json, os, shutil, subprocess, sys

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
        if any(c['match'] == m['match'] and abs(c['kill'] - m['kill']) < a.before + a.after for c in chosen):
            continue
        chosen.append(m)
        per_match[m['match']] = per_match.get(m['match'], 0) + 1
        per_map[m['map']] = per_map.get(m['map'], 0) + 1
        per_kind[m.get('kind')] = per_kind.get(m.get('kind'), 0) + 1
    out = f'{RUNS}/{a.scout}/clips'
    os.makedirs(out, exist_ok=True)
    for index, m in enumerate(chosen, 1):
        name = f"{index:02d}-{m.get('kind', 'kill')}-{m['match']}-{m['kill']:.0f}s"
        start, end = m['kill'] - a.before, m['kill'] + a.after
        env = dict(os.environ, **match_env(m['map'], m['seed'], scouted, 0), PORT='3600', SIZE=a.size,
                   RUNS=f'{RUNS}/{a.scout}/shoot', TRACE_SECONDS=f'{end + 0.3:.2f}',
                   HALO_SCREENSHOT_FROM=f'{start:.3f}', HALO_SCREENSHOT_UNTIL=f'{end:.3f}')
        subprocess.run([f'{HERE}/capture.sh', name, 'openh1', '-', '', str(int(end) + 180)], env=env,
                       capture_output=True, text=True)
        frames = f'{RUNS}/{a.scout}/shoot/{name}/frames'
        names = sorted(os.listdir(frames)) if os.path.isdir(frames) else []
        if not names:
            print(f'{name}: no frames', flush=True)
            continue
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-framerate', '60', '-start_number',
                        names[0][5:10], '-i', f'{frames}/frame%05d.bmp', '-frames:v', str(len(names)),
                        '-an', '-c:v', 'libx264', '-crf', '16', '-preset', 'slow', '-pix_fmt', 'yuv420p',
                        '-movflags', '+faststart', f'{out}/{name}.mp4'], check=True)
        shutil.rmtree(frames)
        print(f"{name}: {len(names)} frames, score {m['score']}", flush=True)

if __name__ == '__main__':
    main()
