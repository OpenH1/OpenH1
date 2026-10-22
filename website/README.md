# openh1.com

The project's landing page: one static page (`public/index.html`), its
pictures and clips (`public/media/`), its fonts (`public/fonts/`), the logo
(`public/brand/`) and the icons (`public/favicon.svg`, `favicon-16.png`,
`favicon-32.png`, `apple-touch-icon.png`).

It is a Cloudflare Worker of static assets, `openh1`, on the custom domains
`openh1.com` and `www.openh1.com` (`cloudflare.config.ts`), in Paul's
Cloudflare account. To publish a change, from this folder:

```sh
npm install                                   # once: the Cloudflare CLI, Wrangler and Node 22
PATH="$PWD/node_modules/.bin:$PATH" npx cf deploy
```

The CLI needs Node 22.18 or later to load `cloudflare.config.ts`;
`node_modules/.bin` puts the `node` package's first, as the system's may be
older.

To preview the page, serve `public/` (`python3 -m http.server -d public`)
and open `http://localhost:8000/`. Opened as a file, the fonts may not load.

## The page

The look is "Boot Glow": green on black, after the Xbox era's boot glow and
the plasma's green, with wide Archivo for the titles, Inter for reading and
Overpass Mono (the family of the port's HUD font) for labels. The colours
are the custom properties at the top of the style sheet.

From the top:

- **The hero**: a muted loop of the game (`media/hero.mp4`, `hero.webm`,
  `hero-poster.webp`) under the title. It starts once the page has loaded,
  pauses off screen, has a Pause button, and a visitor whose system asks
  for less motion sees the poster only.
- **The headline features**: 128 players, online co-op, the Steam Frame
  build, 64-bit, Vulkan and Metal, for any player.
- **The picture sections**, each a comparison of the same moment captured
  twice, the Original (as the game drew it) and OpenH1:
  - 01, dynamic lights: a wipe, with callouts on the OpenH1 side;
  - 02, anti-aliasing: a lens, OpenH1 inside it;
  - 03, the plasma glow: two clips side by side, with a numbered legend;
  - 04, frames between ticks: two clips side by side.
  The wipe and the lens play one short demo the first time it scrolls into view
  (none for less motion), and stops as soon as the visitor touches it.
- **VR**, **Platforms**, **Under the hood** (the details for purists, in a
  dense list), **Bring your own game**, **Get it** (the downloads), and the
  footer.
- **Get it**: a card for each machine. Windows and the Steam Deck link the
  latest release's zip (`releases/latest/download/…`) with short install
  steps, and so does the Mac (signed and notarized: `OpenH1.dmg`). The Steam Frame (until it has a simpler install) says
  "Coming soon". The script puts the visitor's machine
  first, and marks it, when it has a build. Android has no card: its builds
  cannot update over each other until it has a signing key (#92).

Rules for the content:

- Download links only for builds someone who isn't a developer can install:
  Windows, the Mac and the Steam Deck for now.
- Every claim is in this repository's READMEs. Check a new one there first.
- Every picture or clip is a real capture of the game in OpenH1, captioned
  with what it compares. No Microsoft or Halo branding, logos or art.
- The footer keeps the credit to OpenCE and the disclaimer: an unofficial
  fan project, not endorsed by or affiliated with Microsoft; Halo is
  Microsoft's trademark; no game data included.

## Pictures and clips

`public/media/`:

| Files | Where | What |
| --- | --- | --- |
| `hero-720.*`, `hero-1080.*`, `hero-1440.*`, `hero-poster.webp` | The hero | Six multiplayer moments cut together (`capture/`, "hero, 2026-10-20"): a Warthog's splatter on Blood Gulch, a pistol triple on Sidewinder, a needler on Prisoner, a sticky on Hang 'Em High, rockets on Chill Out and Damnation. Enhanced at 60 fps, HUD on, filmed at 3840 × 2160; MP4 and WebM at 1280, 1920 and 2560 wide (1.6, 3.2 and 5.5 Mbit/s, a keyframe each second), the page choosing by the screen (a phone the smallest); 21 s, hard cuts; the poster its first frame |
| `lights-original.*`, `lights-openh1.*` | 01 | Rat Race, a frag grenade at a spot already scorched (`capture/`): the Classic preset at 640 × 480 a frame a tick, and Enhanced at 60 fps between ticks. 4 s, 1920 × 1440 (from 3840 × 2880 masters), a loop without a fade, MP4 and WebM with posters |
| `edges-original.*`, `edges-openh1.*` | 02 | Blood Gulch's cliffs against the sky, a slow strafe out and back (`capture/`): the Classic preset at 640 × 480 and Enhanced (SMAA). A quarter of the frame (320 × 180 of the Original's pixels) at 1280 × 720, 4× by nearest neighbour, shown with `image-rendering: pixelated`; 5.3 s, a loop without a fade |
| `glow-original.*`, `glow-openh1.*` | 03 | Rat Race, the plasma rifle fired down a dark corridor, a burst before the clip leaving the scorch marks (`capture/`): the Classic preset a frame a tick, and Enhanced at 60 fps between ticks. 4 s, 1280 × 960, a loop without a fade |
| `strafe-original.*`, `strafe-openh1.*` | 04 | Rat Race, a strafe past pillars and back to where it began (`capture/`): the Classic preset at one frame a tick, and Enhanced at 60 fps between ticks. 3.9 s, 1280 × 720 (a 16:9 crop of 4:3 masters), a loop without a fade |

Each comparison is a pair of layers, `.layer.before` (the Original) under
`.layer.after` (OpenH1), each holding an `<img>` or a `<video>`. A video
needs `muted loop playsinline preload="none"`, a `poster`, and
`data-webm` / `data-mp4`: the script adds the sources when the pair comes
into view, plays the pair together and keeps the second in step with the
first. How they are captured is in `capture/README.md`.

Callouts are `<span class="co">` in the OpenH1 layer: `--x` and `--y` put
the pin on the spot it names (in percent of the picture), `--len` is the
leader's length, `west` puts the label to the left of its pin and `down`
below it. On a phone the pins become numbers, and the `<ol class="legend">`
under the picture says what each one is: keep the two in the same order.

Budgets: a still at most 300 KB, a comparison clip at most 1 MB a file,
the hero at most 4 MB. Everything below the hero loads lazily.

## Fonts

`fonts/Archivo-Latin.woff2`, `fonts/Inter-Latin.woff2` and
`fonts/OverpassMono-Latin.woff2` are the Latin subsets of Archivo (its
width and weight axes), Inter and Overpass Mono, from Google Fonts, under
the SIL Open Font License 1.1 (`fonts/LICENSES.txt`, linked from the
footer). They are served from this site: the page loads nothing from
other sites, and has no trackers or analytics.

## The logo

The logo is "Tail Code" (round 5, concept 01, `website/brand/logo-r5` on
the `logo-r5` branch): OPEN in light, wide caps, then H1 knocked out of a
green plate with one clipped corner. `brand/openh1-lockup.svg` is the
header's and the footer's wordmark, and `brand/openh1-mark.svg` the plate
alone. `favicon.svg` is the plate on a dark tile; `favicon-16.png`,
`favicon-32.png` and `apple-touch-icon.png` (180 px) are it drawn at those
sizes. The link preview, `og:image`, is `media/og.jpg` (1200 × 630): the
hero's poster with the logo and the headline.
