"""Cut the trailer from the shoot's clips (shoot.py): each clip trimmed to its
beat around the kill, the kinds alternated and the best kept for last, joined
with hard cuts.

usage: cut.py <clips folder> <out.mp4> [--pick 01,07,03,...] [--before 3.5]

A clip's kill is --before seconds into it (shoot.py's window). Its beat is
the second and a bit before the kill and a little after: longer for a
vehicle's (the drive in), shorter for a chaos clip's (the fastest cuts).
--pick takes the clips in that order instead."""
import argparse, os, re, subprocess

BEATS = {'vehicle': (2.2, 0.6), 'snipe': (1.6, 0.5), 'rocket': (1.4, 0.7), 'chaos': (1.0, 0.4), 'kill': (1.2, 0.4)}

def main():
    p = argparse.ArgumentParser()
    p.add_argument('clips'); p.add_argument('out')
    p.add_argument('--pick'); p.add_argument('--before', type=float, default=3.5)
    a = p.parse_args()
    clips = sorted(f for f in os.listdir(a.clips) if f.endswith('.mp4'))
    kind = lambda f: re.match(r'\d+-([a-z]+)-', f).group(1)
    if a.pick:
        order = [next(f for f in clips if f.startswith(n + '-')) for n in a.pick.split(',')]
    else:
        # (the kinds in turn, each's best first; the best clip of all last)
        by_kind = {}
        for f in clips:
            by_kind.setdefault(kind(f), []).append(f)
        order = []
        while any(by_kind.values()):
            for k in ('vehicle', 'kill', 'snipe', 'chaos', 'rocket'):
                if by_kind.get(k):
                    order.append(by_kind[k].pop(0))
        order.append(order.pop(order.index(clips[0])))
    inputs, parts = [], []
    for index, f in enumerate(order):
        before, after = BEATS.get(kind(f), (1.2, 0.4))
        inputs += ['-i', os.path.join(a.clips, f)]
        parts.append(f'[{index}:v]trim=start={a.before - before:.3f}:end={a.before + after:.3f},setpts=PTS-STARTPTS[v{index}]')
    graph = ';'.join(parts) + ';' + ''.join(f'[v{i}]' for i in range(len(order))) + f'concat=n={len(order)}:v=1:a=0[out]'
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', *inputs, '-filter_complex', graph, '-map', '[out]',
                    '-c:v', 'libx264', '-crf', '18', '-pix_fmt', 'yuv420p', a.out], check=True)
    print('\n'.join(order))

if __name__ == '__main__':
    main()
