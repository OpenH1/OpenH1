# openh1.com

The project's landing page: one static page (`public/index.html`), a small
script for its pictures and clips (`public/media.js`), the fonts
(`public/fonts/`) and the app's icon (`public/icon.png`, `favicon.png`,
`apple-touch-icon.png`, from `port/assets/icon`).

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

To preview the page, open `public/index.html` in a browser. It needs no
server, and the pictures and clips show from the file too.

`design-board.html` (not published) is the board the page's direction was
chosen from.

## The page

- Light by default, and the game's menu colours (navy, shield blue, visor
  gold) when the visitor's system is dark. The colours are the custom
  properties at the top of the style sheet, in `:root` and in its
  `prefers-color-scheme: dark` block.
- No download buttons yet: "Downloads coming soon" and the release alerts
  (GitHub's Watch, the releases feed) stand in for them.
- Every claim on the page is in this repository's READMEs. Check a new one
  there before adding it.
- The footer keeps the disclaimer: an unofficial fan project, not endorsed
  by or affiliated with Microsoft; Halo is Microsoft's trademark; no game
  data included.

## Fonts

`fonts/Overpass-Latin.woff2` and `fonts/OverpassMono-Latin.woff2` are the
Latin subsets of Overpass (weights 400 to 900) and Overpass Mono (400 to
700), from Google Fonts. Overpass is the font the game's port draws its HUD
and menu text with (`port/assets/fonts`). Both are under the SIL Open Font
License 1.1 (`fonts/Overpass-OFL.txt`).

## The logo

The header and the footer show the name as text, `Open<span>H1</span>`.
When the logo is ready:

1. Put the mark in `public/logo-mark.svg`: square, drawn to read at 28 px,
   on a transparent background. An SVG shown as an image doesn't take the
   page's text colour, so if the mark should change colour in dark mode,
   give the SVG its own `<style>` with a `@media (prefers-color-scheme:
   dark)` rule.
2. In `index.html`, find `LOGO:` in the header and uncomment the
   `<img class="brand-mark" …>` line. Its size is `.brand-mark` in the style
   sheet.
3. Replace `favicon.png` (32 px), `apple-touch-icon.png` (180 px) and the
   link preview, `og:image` (now `icon.png`; a 1200 × 630 picture is best).

## Pictures and clips

The page has three places for media, all empty and hidden until they have
something:

- **The hero**: one before/after pair or one clip, under the headline, the
  full width of the page. On a phone it takes the place of the numbers.
- **See the difference** (after the hero): before/after pairs and clips.
- **Gallery** (before "Bring your own game"): pictures that open full size.

They come from the media list at the end of `index.html`, `<script
type="application/json" id="media-data">`. `media.js` reads it when the page
loads. Put the files in `public/media/` and add an entry to the list. The
list is JSON: double quotes, commas between entries and none after the last.
If it isn't valid, no media shows, and the browser's console says why.

Use only real captures of the game, and say in the caption what is
compared (for example the Classic and Enhanced modes of Video Setup's
Dynamic Lights).

### A before/after pair

Two pictures of the same scene from the same camera, at the same size. The
visitor drags across the picture (mouse or touch), or focuses it with Tab and
uses the arrow keys, Page Up and Down, Home and End.

- Files: `public/media/<scene>-<state>.avif`, lowercase with hyphens, for
  example `media/a10-lights-classic.avif` and
  `media/a10-lights-enhanced.avif`.
- Size: 1920 × 1080 (16:9). Another shape works if both pictures have it and
  the entry says it (`width`, `height`).
- Format: AVIF (quality about 60), or WebP (quality about 80). JPEG works,
  but is larger.
- Budget: at most 250 KB a picture; at most 200 KB for the hero's pair,
  which loads with the page. The others load as the visitor scrolls to
  them.

```json
"compare": [
  {
    "id": "a10-lights",
    "before": "media/a10-lights-classic.avif",
    "after": "media/a10-lights-enhanced.avif",
    "beforeLabel": "Classic",
    "afterLabel": "Enhanced",
    "beforeAlt": "A plasma bolt in a corridor of the first level, lit the game's own way",
    "afterAlt": "The same corridor, the bolt lighting the walls around it",
    "caption": "Dynamic Lights, Classic and Enhanced.",
    "width": 1920,
    "height": 1080
  }
]
```

`before` shows on the left, `after` on the right. The labels are the tags in
the picture's top corners (they default to Before and After). The alt texts
describe each picture for screen readers.

### A clip

A short loop with no sound. It plays while it's on screen, with a Pause
button. For a visitor whose system asks for less motion, it waits on its
poster until they choose Play.

- Files: `public/media/<name>.webm`, `<name>.mp4` and `<name>-poster.avif`.
- Length: 8 to 20 seconds, looping cleanly, no audio track.
- Size: 1920 × 1080 or 1280 × 720, 30 or 60 frames a second.
- Formats: both WebM (AV1 or VP9) and MP4 (H.264, for older Safari). The
  browser takes the first it can play.
- Budget: at most 3 MB a file (2 MB for the hero's); the poster (its first
  frame) at most 150 KB.

```json
"video": [
  {
    "id": "warthog-run",
    "webm": "media/warthog-run.webm",
    "mp4": "media/warthog-run.mp4",
    "poster": "media/warthog-run-poster.avif",
    "label": "A Warthog crossing a canyon at sunset",
    "caption": "On a Mac, at 120 Hz.",
    "width": 1920,
    "height": 1080
  }
]
```

`label` is what screen readers say for the clip and its button.

### A gallery picture

- Files: `public/media/<name>.avif`, and, for a large original, a smaller
  `<name>-thumb.avif` for the grid.
- Size: 16:9. The grid's picture about 800 px wide (at most 80 KB); the
  full one 1920 px or more (at most 400 KB).

```json
"gallery": [
  {
    "src": "media/beach-thumb.avif",
    "full": "media/beach.avif",
    "alt": "Marines holding a beach as a dropship comes in",
    "caption": "The second level, at 4K.",
    "width": 800,
    "height": 450
  }
]
```

`full` is optional: without it, the grid's picture opens.

### The hero

Set `"hero"` to the `id` of a pair or a clip in the list, for example
`"hero": "a10-lights"`. It shows in the hero and not again in See the
difference. `""` leaves the hero without media.

### Making the files

With ffmpeg (`brew install ffmpeg`) and the WebP tools (`brew install
webp`):

```sh
# a picture: AVIF, or WebP
ffmpeg -i shot.png -c:v libaom-av1 -still-picture 1 -crf 32 -pix_fmt yuv420p shot.avif
cwebp -q 80 shot.png -o shot.webp

# a smaller copy for the gallery's grid
ffmpeg -i shot.png -vf scale=800:-2 -c:v libaom-av1 -still-picture 1 -crf 34 -pix_fmt yuv420p shot-thumb.avif

# a clip: 12 seconds from 0:30, no sound, 1080p
ffmpeg -ss 30 -t 12 -i capture.mov -an -vf scale=1920:-2 -c:v libvpx-vp9 -b:v 0 -crf 36 -row-mt 1 clip.webm
ffmpeg -ss 30 -t 12 -i capture.mov -an -vf scale=1920:-2 -c:v libx264 -crf 26 -preset slow -pix_fmt yuv420p -movflags +faststart clip.mp4

# its poster: the first frame
ffmpeg -i clip.mp4 -frames:v 1 -c:v libaom-av1 -still-picture 1 -crf 32 -pix_fmt yuv420p clip-poster.avif
```

Check the sizes against the budgets (`ls -lh public/media`), and raise
`-crf` (smaller, softer) or lower the resolution for a file that is over.
