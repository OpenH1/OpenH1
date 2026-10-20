"""A clip's final pair: both sides captured (OpenH1 at 60 frames a second and
SIZE, the Original a frame a tick), the loop chosen and the pair encoded.

usage: final.py <clip> <scenario> <script> <start> <end-from> <end-to> [--seed N] [--keep]

The loop starts at <start> (seconds of simulation) and ends at the tick in
[<end-from>, <end-to>] whose Original frame is most like the first: the clip
cuts from the tick before it back to the first. The Original's frame for a
tick is found from the sim trace (its frames are its loading frames, then
one a tick); OpenH1's, which draws two a tick a tick behind, by the
brightest jump in both (a blast) when there is one, else twice the
Original's. Masters: H.264 at CRF 16 and a poster, at WIDTH x HEIGHT, the
Original scaled by nearest neighbour."""
import argparse, os, shutil, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
RUNS = os.environ.get('RUNS', os.path.expanduser('~/halo-capture-runs'))
SIZE = os.environ.get('SIZE', '3840x2880')
WIDTH, HEIGHT = (int(x) for x in SIZE.split('x'))

def capture(clip, side, a, port):
    env = dict(os.environ, PORT=str(port), SIZE=SIZE, TRACE_SECONDS=str(a.end_to + 1.0),
               FT='0.0333333' if side == 'original' else '0.0166667')
    if a.seed is not None:
        env['SEED'] = str(a.seed)
    return subprocess.Popen([f'{HERE}/capture.sh', f'final/{clip}-{side}', side, a.scenario, a.script, '120'],
                            env=env)

def ticks(path):
    """the last tick, and each step's time and tick"""
    steps = [(float(f[1]), int(f[2])) for f in (l.split() for l in open(path)) if f[0] == 's']
    return steps[-1][1] + 1, steps

def luma(path, size=(320, 240)):
    return np.asarray(Image.open(path).convert('L').resize(size, Image.BOX), dtype=np.float32)

def main():
    p = argparse.ArgumentParser()
    p.add_argument('clip'); p.add_argument('scenario'); p.add_argument('script')
    p.add_argument('start', type=float); p.add_argument('end_from', type=float); p.add_argument('end_to', type=float)
    p.add_argument('--seed', type=int); p.add_argument('--keep', action='store_true')
    a = p.parse_args()
    base = f'{RUNS}/final/{a.clip}'
    procs = [capture(a.clip, 'original', a, 2600), capture(a.clip, 'openh1', a, 2601)]
    if any(proc.wait() for proc in procs):
        sys.exit('a capture failed')
    o, h = f'{base}-original/frames', f'{base}-openh1/frames'
    on, hn = sorted(os.listdir(o)), sorted(os.listdir(h))
    count, steps = ticks(f'{base}-original/data/trace.txt')
    tick = lambda t: min(steps, key=lambda s: abs(s[0] - t))[1]
    fo = lambda k: len(on) - count + k
    start = fo(tick(a.start))
    first = luma(f'{o}/{on[start]}')
    best = max((10 * np.log10(255 ** 2 / max(float(((luma(f'{o}/{on[k]}') - first) ** 2).mean()), 1e-6)), k)
               for k in range(fo(tick(a.end_from)), min(fo(tick(a.end_to)), len(on) - 1) + 1))
    psnr, end = best
    # OpenH1's frames: by the brightest jump in both, else twice the Original's
    vo = np.array([luma(f'{o}/{n}', (80, 60)).mean() for n in on[start:end]])
    jo = int(np.argmax(vo[2:] - vo[:-2])) + 2 + start
    hcount, _ = ticks(f'{base}-openh1/data/trace.txt')
    guess = len(hn) - 2 * hcount + 2 * (jo - (len(on) - count))
    window = range(max(guess - 8, 2), min(guess + 8, len(hn)))
    vh = {k: luma(f'{h}/{hn[k]}', (80, 60)).mean() - luma(f'{h}/{hn[k - 2]}', (80, 60)).mean() for k in window}
    jh = max(vh, key=vh.get) if vo[jo - start] - vo[jo - start - 2] > 3 else guess
    hstart = jh - 2 * (jo - start)
    seconds = (end - start) / 30
    print(f'loop: Original frames {start}-{end - 1}, OpenH1 from {hstart}, {seconds:.2f} s, '
          f'the cut {psnr:.1f} dB; blast at Original {jo}, OpenH1 {jh} (guessed {guess})', flush=True)
    env = dict(os.environ, RUNS=f'{RUNS}/final', WIDTH=str(WIDTH), HEIGHT=str(HEIGHT), MP4_CRF='16', WEBM_CRF='30')
    subprocess.run([f'{HERE}/encode.sh', a.clip, str(start), str(hstart), f'{seconds:.4f}'], env=env, check=True)
    if not a.keep:
        for d in (o, h):
            shutil.rmtree(d, ignore_errors=True)

if __name__ == '__main__':
    main()
