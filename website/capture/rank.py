"""Rank the survey's candidates and draw the best as a contact sheet:
each row the Original and OpenH1 at the blast, then the loop's first and last frames."""
import json, sys
from PIL import Image, ImageDraw
OUT = sys.argv[1]; top = int(sys.argv[2]) if len(sys.argv) > 2 else 8
rs = json.load(open(f'{OUT}/results.json'))
ok = [r for r in rs if 'error' not in r and r['knock'] < 0.05 and r['damage'] < 0.05
      and r['spot_gap'] < 0.5 and r['loop_psnr'] > 33]
ok.sort(key=lambda r: -r['light_diff'])
print(f'{len(rs)} candidates, {len(ok)} usable; errors:',
      {e: sum(1 for r in rs if r.get('error') == e) for e in set(r.get('error') for r in rs if 'error' in r)})
for r in ok[:top]:
    print(f"{r['map']:15} yaw {r['yaw']:5g} pitch {r.get('pitch', -5):4g}  light {r['light_diff']:5.1f}  false {r['false_light']:.2f}  loop {r['loop_psnr']:.1f}  dist {r['distance']:.1f}")
W, H = 400, 300
sheet = Image.new('RGB', (W * 4, H * min(top, len(ok))), 'black')
d = ImageDraw.Draw(sheet)
for row, r in enumerate(ok[:top]):
    k = f"{OUT}/keep/{r['map']}-{r['seed']}-{r['yaw']:g}-{r.get('pitch', -5):g}"
    for col, tag in enumerate(['blast-original', 'blast-openh1', 'start', 'end']):
        im = Image.open(f'{k}/{tag}.png').resize((W, H), Image.NEAREST if 'original' in tag or tag in ('start', 'end') else Image.BOX)
        sheet.paste(im, (col * W, row * H))
    d.text((6, row * H + 4), f"{row + 1}. {r['map']} yaw {r['yaw']:g} pitch {r.get('pitch', -5):g}  light {r['light_diff']:.1f}  loop {r['loop_psnr']:.0f} dB", fill='white')
sheet.save(f'{OUT}/sheet.png')
