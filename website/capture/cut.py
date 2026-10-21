"""Cut the trailer from the shoot's clips (shoot.py): each clip trimmed to its
beat around the kill, the kinds alternated and the best kept for last, joined
with hard cuts.

usage: cut.py <clips folder> <out.mp4> [--pick 01,07,03,...] [--before 3.5]

A clip's kills are in shoot.py's note beside it (else its kill is --before
seconds in). Its beat runs from a second and a bit before its first kill to
a little after its last, so that a double or a triple is shown whole: longer
before for a vehicle's (the drive in), shorter for a chaos clip's.
--pick takes the clips in that order instead."""
import argparse, json, os, re, subprocess

BEATS = {'vehicle': (2.2, 0.6), 'snipe': (1.6, 0.5), 'headshot': (1.8, 0.6), 'rocket': (1.4, 0.7),
         'rocket_multi': (1.3, 1.3), 'chaos': (1.0, 0.4), 'kill': (1.2, 0.4)}

def main():
    p = argparse.ArgumentParser()
    p.add_argument('clips'); p.add_argument('out')
    p.add_argument('--pick'); p.add_argument('--before', type=float, default=3.5)
    a = p.parse_args()
    clips = sorted(f for f in os.listdir(a.clips) if f.endswith('.mp4'))
    kind = lambda f: re.match(r'\d+-([a-z_]+)-', f).group(1)
    if a.pick:
        order = [next(f for f in clips if f.startswith(n + '-')) for n in a.pick.split(',')]
    else:
        # (the kinds in turn, each's best first; the best clip of all last)
        by_kind = {}
        for f in clips:
            by_kind.setdefault(kind(f), []).append(f)
        order = []
        while any(by_kind.values()):
            for k in ('vehicle', 'kill', 'headshot', 'snipe', 'chaos', 'rocket_multi', 'rocket'):
                if by_kind.get(k):
                    order.append(by_kind[k].pop(0))
        order.append(order.pop(order.index(clips[0])))
    inputs, parts = [], []
    for index, f in enumerate(order):
        before, after = BEATS.get(kind(f), (1.2, 0.4))
        # (the clip's kills, from shoot.py's note: a chain is shown from
        # before its first to after its last)
        note = os.path.join(a.clips, f[:-4] + '.json')
        if os.path.exists(note):
            n = json.load(open(note))
            begin, finish = n['first'] - n['start'] - before, n['last'] - n['start'] + after
        else:
            begin, finish = a.before - before, a.before + after
        inputs += ['-i', os.path.join(a.clips, f)]
        parts.append(f'[{index}:v]trim=start={max(begin, 0):.3f}:end={finish:.3f},setpts=PTS-STARTPTS[v{index}]')
    graph = ';'.join(parts) + ';' + ''.join(f'[v{i}]' for i in range(len(order))) + f'concat=n={len(order)}:v=1:a=0[out]'
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', *inputs, '-filter_complex', graph, '-map', '[out]',
                    '-c:v', 'libx264', '-crf', '18', '-pix_fmt', 'yuv420p', a.out], check=True)
    print('\n'.join(order))

if __name__ == '__main__':
    main()
