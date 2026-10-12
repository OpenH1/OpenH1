"""Flicker scan: consumes eyesNNNNN.bmp screenshots as the game writes them,
finds pixels that change and change back over three consecutive frames
(left eye), and keeps per-frame counts, a heat map and the worst frames."""
import sys, os, glob, time, json, re
import numpy as np

shots, out, game = sys.argv[1], sys.argv[2], sys.argv[3]  # the game's PID, or a file that appears when it ends
os.makedirs(out, exist_ok=True)
T = 40

def alive():
    if not game.isdigit():
        return not os.path.exists(game)
    try:
        os.kill(int(game), 0); return True
    except OSError:
        return False

def load(path):
    b = np.fromfile(path, dtype=np.uint8)
    off = int.from_bytes(b[10:14].tobytes(), 'little')
    w = int.from_bytes(b[18:22].tobytes(), 'little', signed=True)
    h = int.from_bytes(b[22:26].tobytes(), 'little', signed=True)
    bpp = int.from_bytes(b[28:30].tobytes(), 'little') // 8
    stride = (w * bpp + 3) & ~3
    img = b[off:off + stride * abs(h)].reshape(abs(h), stride)[:, :w * bpp].reshape(abs(h), w, bpp)[:, :, :3]
    if h > 0:
        img = img[::-1]
    return img if os.environ.get("FLICK_FULL") else img[:, : w // 2]  # left eye, BGR (uint8, a view)

def write_bmp(path, rgb):
    h, w, _ = rgb.shape
    stride = (w * 3 + 3) & ~3
    data = np.zeros((h, stride), np.uint8)
    data[:, :w * 3] = rgb[::-1, :, ::-1].reshape(h, w * 3)
    hdr = b'BM' + (54 + data.size).to_bytes(4, 'little') + b'\0' * 4 + (54).to_bytes(4, 'little')
    hdr += (40).to_bytes(4, 'little') + w.to_bytes(4, 'little') + h.to_bytes(4, 'little') + (1).to_bytes(2, 'little')
    hdr += (24).to_bytes(2, 'little') + b'\0' * 4 + data.size.to_bytes(4, 'little') + b'\0' * 16
    open(path, 'wb').write(hdr + data.tobytes())

window = []  # (index, image)
counts = {}
heat = None
worst = []  # (count, index, overlay)
middle = None

def consider(i, a, b, c, full):
    global heat, middle
    dab = np.abs(a - b).max(axis=2); dbc = np.abs(b - c).max(axis=2); dac = np.abs(a - c).max(axis=2)
    mask = (dab > T) & (dbc > T) & (dac < T // 2)
    n = int(mask.sum()); counts[i] = n
    heat = mask.astype(np.uint32) if heat is None else heat + mask
    if middle is None and len(counts) > 300:
        middle = full[1][::2, ::2, ::-1].copy()
    if n > 0 and (len(worst) < 8 or n > worst[-1][0]):
        if all(abs(i - j) > 36 or n > m for m, j, _ in worst):
            worst[:] = [w for w in worst if abs(w[1] - i) > 36]
            big = np.repeat(np.repeat(mask, 2, axis=0), 2, axis=1)[: full[1].shape[0], : full[1].shape[1]]
            ov = full[1][:, :, ::-1].copy()
            ov[big] = (255, 0, 0)
            ov = (ov, full[0][:, :, ::-1].copy(), full[1][:, :, ::-1].copy(), full[2][:, :, ::-1].copy())
            worst.append((n, i, ov)); worst.sort(key=lambda w: -w[0]); del worst[8:]

def index_of(p):
    return int(re.search(r'(\d+)\.bmp$', p).group(1))

done = False
while True:
    for p in glob.glob(os.path.join(shots, 'hud*.bmp')):
        try: os.remove(p)
        except OSError: pass
    files = sorted(glob.glob(os.path.join(shots, os.environ.get("FLICK_PATTERN", "eyes") + '*.bmp')), key=index_of)
    game_over = not alive()
    ready = files if game_over else files[:-1]  # the newest may be half written
    for p in ready:
        try:
            img = load(p)
        except Exception:
            img = None
        os.remove(p)
        if img is None:
            window = []; continue
        i = index_of(p)
        if window and window[-1][0] != i - 1:
            window = []
        window.append((i, img[::2, ::2].astype(np.int16), np.ascontiguousarray(img))); window = window[-3:]
        if len(window) == 3:
            consider(window[1][0], window[0][1], window[1][1], window[2][1], [w[2] for w in window])
    if game_over:
        break
    if not ready:
        time.sleep(0.02)

json.dump({'frames': len(counts), 'scale': 0.5, 'counts': counts}, open(os.path.join(out, 'stats.json'), 'w'))
if heat is not None:
    base = middle if middle is not None else np.zeros(heat.shape + (3,), np.uint8)
    g = (base.mean(axis=2) * 0.5).astype(np.uint8)
    ov = np.stack([g, g, g], axis=2)
    hm = np.clip(heat.astype(np.float32) / max(1.0, np.percentile(heat[heat > 0], 99) if (heat > 0).any() else 1.0), 0, 1)
    ov[..., 0] = np.maximum(ov[..., 0], (hm * 255).astype(np.uint8))
    ov[..., 1] = np.maximum(ov[..., 1], (hm * 160).astype(np.uint8) * (hm > 0.6))
    write_bmp(os.path.join(out, 'heat.bmp'), ov)
for n, i, (img, a, b, c) in worst:
    write_bmp(os.path.join(out, 'worst_%05d_%d.bmp' % (i, n)), img)
    write_bmp(os.path.join(out, 'raw_%05d_a.bmp' % i), a)
    write_bmp(os.path.join(out, 'raw_%05d_b.bmp' % i), b)
    write_bmp(os.path.join(out, 'raw_%05d_c.bmp' % i), c)
print('frames', len(counts), 'flicker px mean %.0f' % (np.mean(list(counts.values())) if counts else 0), 'worst', [(i, n) for n, i, _ in worst])
