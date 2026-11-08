# The replay service's protocol

`halo_replay <recording> --serve` replays a render recording
(`HALO_RENDER_RECORD`: `port/android/host/host_render_record.h`) and answers
requests about its frames: their pictures, which draw made each pixel, and
what each draw was. A client (a review tool, a script, an agent) starts it as
a child process and talks to it in JSON lines. This document is what a client
may rely on; the code is `port/tools/replay/host_render_replay.c`, shared by
Metal's replay (`port/macos/tools/halo_replay.m`, `ninja macos_replay`) and
Vulkan's (`port/linux/vulkan/tools/halo_replay.c`, `ninja linux_replay`).

Recordings and the pictures the service draws hold the game's art: keep them
on your own machines.

## Transport

- One request a line on standard input, one reply a line on standard output,
  in order. The log is on standard error. A client sends one request at a
  time and waits for its reply; it may run more than one service on the same
  recording (one for the frames, one for thumbnails, say).
- The service runs locally or over ssh (`ssh <host> halo_replay <recording>
  --serve`, the recording copied there first). Over ssh, ask for inline
  replies (`"inline": true`) and compressed ones (`"compress": true`); ssh's
  own compression helps little on top.
- A failed request is `{"ok": false, "error": "<words>"}`; the service carries
  on. If the replayed stream itself stops (`"the stream stopped"`), the
  service exits after replying.
- `{"cmd": "quit"}` ends it (as does closing its standard input).

## Versions

`info`'s `"protocol"` is the protocol's version, `HOST_RENDER_REPLAY_PROTOCOL`
in `host_render_replay.h`: **1**. It is raised when a request or a reply
changes so that a client of the old version would misread it; added fields
and requests do not raise it, and a client ignores fields it does not know. A
service without the field speaks version 1. A client refuses a service whose
version it does not speak, and says so.

## Requests

### info

```json
{"cmd": "info"}
{"ok": true, "protocol": 1, "frames": 1676, "width": 2560, "height": 1440, "backend": "metal",
 "build": "85ccc010a", "marks": true, "records": 4196312}
```

`frames`: the recording's frames, numbered from 0. `width`, `height`: the
last frame's back buffer (a frame's own size is in its `frame` reply).
`backend`: `"metal"` or `"vulkan"`. `build`: the replay's git commit, which a
client can key a cache of pictures on (with `backend`). `marks`: the
recording has MARK records (`debug.render_marks`, below). `records`: the
stream's records.

### frame

```json
{"cmd": "frame", "n": 120, "png": "/abs/120.png", "ids": "/abs/120.ids"}
{"ok": true, "n": 120, "ms": 54.2, "width": 2560, "height": 1440}
```

Frame `n` is played exactly (the frames before it first; seeking back plays
from the start again, the frames more than eight before the one asked for
without their GPU work) and its picture is the back buffer it presents.

- `"png": <path>`: the picture written there as a PNG (stored, not
  deflated: fast to write).
- `"image": "qoi"`: the picture as a [QOI](https://qoiformat.org) file
  (lossless RGB, smaller than PNG on the game's frames and a few times faster
  to make), written to `"qoi"` (or `"png"`) when given.
- `"ids": <path>` writes the ID buffer there; `"ids": true` asks for it
  inline. Without `ids` the frame is played once, for its picture alone,
  which is faster.
- `"inline": true`: no files; the reply carries `"png_base64"` (or
  `"qoi_base64"`) and, with `ids`, `"ids_base64"`.
- `"compress": true` (with `inline`): the PNG is deflated, and the ID buffer
  comes as `"ids_zlib_base64"`, a zlib stream, in place of `"ids_base64"`. A
  client accepts either. A 1440p frame's reply with its ID buffer is about
  11 MB instead of 34.

`ms` is the time the play took. `width` and `height` are the frame's picture.

### The ID buffer

`width` x `height` little-endian `uint32`, row-major from the top-left: each
pixel's topmost draw that changed it, as the draw's number in its frame plus
one (0: no draw). A draw is a `DRAW`, `DRAW_INDICES`, `DRAW_INDEXED` or
`DRAW_IMMEDIATE` record of the frame, counted from 0 in the order they were
recorded: the `"i"` of `draws` and `pick`. A draw counts where it changed the
pixel's value: an alpha-tested leaf's pixels, not its quad's cut-out corners.
It covers the draws into the target the frame presents. On a backend without
one, `frame` with `ids` and `pick` answer an error.

### draws

```json
{"cmd": "draws", "n": 120}
{"ok": true, "n": 120, "frame": {"n": 120, "present": 3, ...the view, below}, "draws": [<draw>, ...]}
```

Every draw of the frame, in order. `frame.present` is the frame's present
record's number.

### pick

```json
{"cmd": "pick", "n": 120, "x": 640, "y": 360}
{"ok": true, "n": 120, "x": 640, "y": 360, "stack": [<draw>, ...]}
```

The draws that changed the pixel, topmost first, up to 8
(`HOST_RENDER_REPLAY_ID_DEPTH`). It plays the frame with its ID buffer if the
frame last played was another; a pick on the frame last asked for with
`ids` is immediate.

### thumbs

```json
{"cmd": "thumbs", "every": 30, "width": 160, "dir": "/abs/thumbs"}
{"ok": true, "thumbs": [{"n": 0, "png": "/abs/thumbs/thumb_0.png"}, {"n": 30, "png": "..."}, ...]}
```

Small pictures for a timeline: every `every`th frame (default 30), or the
frames of `"frames": [n, ...]`, scaled to `width` pixels across (default 160).
Written to `dir` as `thumb_<n>.png` (`thumb_<n>.qoi` and `"qoi"` with `"image":
"qoi"`); with `"inline": true` or no `dir`, each is `{"n", "png_base64"}` (or
`"qoi_base64"`), deflated with `"compress": true`. A frame that cannot be
played is left out.

## A draw

Every draw has:

| Field | |
| --- | --- |
| `i` | its number in the frame (the ID buffer holds `i + 1`) |
| `kind` | what it belongs to (marks); `"other"` without marks |
| `label` | its owner's label (marks), else its record and primitive (`"draw_indexed triangles"`) |
| `id` | the owner's id (an object's handle, a widget's instance, a lightmap's `index << 16 \| material`), or `null` |
| `tags` | the tag paths known of the owner: `object`, `model`, `shader`, `bitmap` (only those known; `{}` without marks) |
| `source` | a menu widget's generator source, `{"file", "text"}`, or `null` |
| `category` | the game renderer's profile category (`rasterizer.h`'s), or `null` |
| `record` | `draw`, `draw_indices`, `draw_indexed` or `draw_immediate` |
| `primitive`, `count`, `prims` | the primitive, its vertex or index count, the primitives it makes |
| `pipeline` | the pipeline's number, as a string |
| `target`, `depth_target` | the colour and depth targets drawn into |
| `textures` | each bound stage: `{"stage", "source": "texture" \| "target" \| "image", "resource", "target", "format"}` (`resource` and `format` in hexadecimal) |

A menu widget's draw also has `widget`, its tag's path. Its `source.text` is
the widget's whole source attribute (`tools/menu_sources.py`: its `locate`
finds the generator function's line), and `source.file` its first word, the
generator's file.

`kind` is one of `other`, `world_object`, `first_person`, `bsp`, `sky`,
`decal`, `effect`, `particle`, `contrail`, `lightning`, `weather`, `water`,
`hud`, `text`, `menu_widget`, `cinematic_letterbox`, `debug`,
`detail_object`, `lens_flare`, `fog`, `screen_effect`, `light_volume`,
`shadow`, `clear`. A client shows a kind it does not know as `other`.

## Marks

A recording made with `debug.render_marks` (`HALO_RENDER_MARKS=1` beside
`HALO_RENDER_RECORD`, and `HALO_METAL_UNPACED=1` on a Mac) carries the
stream's MARK records (`port/linux/game/render_marks.c`,
`port/linux/src/xgpu_stream.h`): what each run of draws belongs to, and each
view's camera. `info`'s `marks` says whether it has them. With them, the
draws' `kind`, `label`, `id`, `tags`, `widget` and `source` are filled, and
`draws`' `frame` gains the frame's view:

| Field | |
| --- | --- |
| `map` | the map's name, or `null` |
| `tick` | the game tick |
| `window` | the view's window (split screen's), `null` for the frame's start |
| `camera` | `{"position": [x, y, z], "forward": [x, y, z], "up": [x, y, z], "fov": <horizontal, radians>}` |
| `viewport` | `[x, y, width, height]` in the back buffer's pixels |
| `views` | every view of the frame, each with the fields above: the frame's start's first (`window` `null`), then one per window |

The frame's own fields are its first window's view (or the frame's start's,
if it has no window). A recording without marks plays the same; its draws are
unnamed and its frame has `n` and `present` alone.

## Notes: build/notes.jsonl

A client may leave review notes on what it shows, in the checkout's
`build/notes.jsonl` (out of git), one JSON object a line; `tools/notes.py`
lists, batches and briefs them for whoever fixes them, and marks them taken,
answered and ready for review (`python tools/notes.py --help`). They are the
menu notes' format (`tools/menu_notes.py`, `build/menu_notes.jsonl`) with an
anchor. A client rewrites the file whole (to a temporary file, then renamed
over it) and reads it again when it changes.

| Field | |
| --- | --- |
| `id` | unique, short |
| `time` | when it was written, ISO 8601 UTC (`2026-11-08T00:00:00Z`) |
| `author` | who wrote it (`reviewer`) |
| `origin` | what wrote it (the client's name) |
| `kind` | `fix`, or `question` (no code changed until the reviewer answers on its thread) |
| `scope` | `variant`: this moment; `screen`: this thing (the object or tag wherever it draws); `everywhere`: every draw like it (its kind, its shader) |
| `status` | `open`, `in_progress`, `fixed` (ready for review: `done` is its older name), `closed` (approved) |
| `note` | the words, at most 2000 characters |
| `thread` | replies, `[{"author", "text", "time"}, ...]` |
| `anchor` | the moment, below |
| `picture` | the frame with the draw outlined, a PNG, as a path relative to the checkout (`build/inspector/notes/<id>.png`) |
| `after` | once fixed: the frame drawn again (`tools/notes.py fixed` renders it into `build/inspector/after/`) |
| `commit`, `changed` | once fixed: the fix's commit, when it was marked |

The anchor:

```json
{"recording": "build/r.hrpl", "frame": 120, "pixel": [640, 360],
 "draw": {"i": 3, "kind": "world_object", "label": "warthog #2", "id": 65558,
          "tags": {"object": "vehicles\\warthog\\warthog", "shader": "..."}, "source": null,
          "textures": [...], "pipeline": "12"},
 "camera": {...}, "map": "bloodgulch", "tick": 4200, "backend": "metal", "build": "85ccc010a",
 "source_name": "Metal"}
```

`recording` is relative to the checkout when it is in it. `draw` is the
picked draw as the service gave it (any of its fields), `null` for a note on
the frame as a whole; `camera`, `map` and `tick` are the frame's view;
`backend` and `build` are `info`'s; `source_name` is the client's name for the
service it drew the frame with.

A note on a menu widget (its draw's `kind` `menu_widget`) also carries the
menu notes' `screen`, `widget` and `source`, and is a menu note: its brief and
`fixed` are `tools/menu_notes.py`'s.
