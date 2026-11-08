#!/usr/bin/env python3
"""The menus reviewed on the gallery's pictures, in a browser: a small web app
of this machine's, for the maintainer's phone and desktop.

    python tools/menu_review.py serve [--host 0.0.0.0] [--port 8765]
    python tools/menu_review.py render [--ref REF]      # the noted screens again, before | after
    python tools/menu_review.py secret                  # print the login's secret

It serves the galleries in build/ (tools/menu_gallery.py's: build/menu_gallery,
the latest, and the runs in build/menu_review/runs, a --compare run's before
and after among them), behind a shared secret (--secret, MENU_REVIEW_SECRET,
or one made once in build/menu_review/secret; typed at /login, or a link
/login?key=<secret> that signs a phone in once): the pictures are the game's,
so the app is never open, and nothing leaves this machine (no outside
service, no library from elsewhere). Bind it to the LAN, or to a Tailscale
or Fabric address with --host.

- The gallery: the screens grouped by menu, each with its variants side by
  side and a badge of its open notes.
- A screen: its picture at full size. The widget under the pointer (or a
  finger) is outlined, from the picture's layout (debug.menu_layout's JSON,
  which the gallery keeps beside it); a tap picks it, a drag marks an area,
  and a tap with Point on pins a place; then a note is typed. Notes go to the
  studio's notes file (build/menu_notes.jsonl, tools/menu_notes.py's), as
  its F9 notes do, with "source": "web", the author, the run and the
  picture's coordinates, and a copy of the picture with the widget or area
  outlined (build/menu_notes/); the studio's notes show here too.
- The notes: open, in progress, done, closed, by screen, variant or author,
  each with its picture and, once done, its screen before | after (from the
  runs render makes); reopen, start and approve (closing it).
- A --compare run (a pull request's) opens like the gallery, its before
  beside its after; notes go on the after.
- Render (the button, or `render`): the noted screens again with
  tools/menu_gallery.py --compare <ref> --notes, into a run of their own:
  the ref is the parent of the earliest commit the done notes name.
- --watch-brief (off by default): when notes arrive, tools/menu_notes.py's
  brief written to build/menu_notes_brief.txt for an agent to pick up.

The notes, their pictures and the galleries stay in build/, out of git.
"""
import argparse
import hashlib
import hmac
import html
import json
import mimetypes
import os
import re
import secrets
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
from http import cookies
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import menu_gallery  # noqa: E402
import menu_notes  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
REVIEW = BUILD / "menu_review"
COOKIE = "menu_review"
NOTES_WATCH_SECONDS = 2.0

# the studio's inputs and shapes as the gallery's variants
STUDIO_VARIANTS = {("keyboard", "16:9"): "keyboard-16x9", ("controller", "16:10"): "controller-16x10",
                   ("touch", "phone 20:9"): "touch-phone", ("keyboard", "4:3"): "keyboard-4x3",
                   ("keyboard", "21:9"): "keyboard-21x9"}
INPUT_VARIANTS = {"keyboard": "keyboard-16x9", "controller": "controller-16x10", "touch": "touch-phone"}


# ---------- the login

def secret_of(argument: str = None) -> str:
    """the login's secret: given, MENU_REVIEW_SECRET's, or the one made once in build/menu_review/secret"""
    if argument:
        return argument
    if os.environ.get("MENU_REVIEW_SECRET"):
        return os.environ["MENU_REVIEW_SECRET"]
    path = REVIEW / "secret"
    if path.is_file():
        return path.read_text().strip()
    REVIEW.mkdir(parents=True, exist_ok=True)
    made = secrets.token_urlsafe(9)
    path.write_text(made + "\n")
    os.chmod(path, 0o600)
    return made


def token_of(secret: str) -> str:
    """the cookie a login gives: the secret's HMAC (the secret itself never stored in the browser)"""
    return hmac.new(secret.encode(), b"menu-review", hashlib.sha256).hexdigest()


def signed_in(cookie_header: str, secret: str) -> bool:
    jar = cookies.SimpleCookie()
    try:
        jar.load(cookie_header or "")
    except cookies.CookieError:
        return False
    given = jar.get(COOKIE)
    return bool(given) and hmac.compare_digest(given.value, token_of(secret))


# ---------- the galleries

def find_runs(build: Path = BUILD) -> list:
    """the galleries there are: build/menu_gallery ("latest") and build/menu_review/runs/<name>, newest first,
    each (name, folder, has a before)"""
    runs = []
    latest = build / "menu_gallery"
    if (latest / "after").is_dir():
        runs.append(("latest", latest))
    folder = build / "menu_review" / "runs"
    if folder.is_dir():
        for run in sorted(folder.iterdir(), key=lambda path: path.stat().st_mtime, reverse=True):
            if (run / "after").is_dir():
                runs.append((run.name, run))
    return [(name, path, (path / "before").is_dir()) for name, path in runs]


def run_folder(name: str, build: Path = BUILD):
    for run, folder, _ in find_runs(build):
        if run == name:
            return folder
    return None


def picture_of(folder: Path, side: str, variant: str, slug: str):
    """a gallery's picture of a screen in a variant (JPEG, else PNG), or None"""
    for suffix in (".jpg", ".png"):
        path = folder / side / variant / (slug + suffix)
        if path.is_file():
            return path
    return None


def run_screens(folder: Path) -> list:
    """the screens a gallery has pictures of, in the menu files' order, with its variants: [(screen, [variant])]"""
    variants = sorted(path.name for path in (folder / "after").iterdir() if path.is_dir())
    ordered = [name for name in menu_gallery.VARIANTS if name in variants] + \
        [name for name in variants if name not in menu_gallery.VARIANTS]
    found = []
    for screen in menu_gallery.enumerate_screens(ROOT / menu_gallery.MENUS, True):
        shown = [variant for variant in ordered if picture_of(folder, "after", variant, screen.slug)]
        if shown:
            found.append((screen, shown))
    return found


def menu_of(screen) -> str:
    """the menu a screen is in, for the gallery's groups: its path's first part after main_menu"""
    parts = screen.name.split("/")
    if parts[0] != "main_menu" or len(parts) < 3:
        return "main menu" if parts[0] == "main_menu" else parts[0]
    return parts[1].replace("_select", "").replace("_", " ")


def safe_file(relative: str, roots: list):
    """a file under one of the roots, by a path relative to the first that has it; None for anything else (no
    .., no other place)"""
    if not relative or "\x00" in relative:
        return None
    for root in roots:
        root = root.resolve()
        path = (root / relative).resolve()
        if path.is_relative_to(root) and path.is_file():
            return path
    return None


# ---------- a click on a picture, as the menus' widget under it

def menu_point(layout: dict, size, x: float, y: float):
    """a point of a picture (its pixels, of its size) in the menus' units, as the layout has them"""
    x0, y0, x1, y1 = layout["screen"]
    width, height = size
    return x0 + x * (x1 - x0) / width, y0 + y * (y1 - y0) / height


def picture_point(layout: dict, size, x: float, y: float):
    """a point in the menus' units on a picture of that size"""
    x0, y0, x1, y1 = layout["screen"]
    width, height = size
    return (x - x0) * width / (x1 - x0), (y - y0) * height / (y1 - y0)


def widget_at(layout: dict, x: float, y: float):
    """the widget a point (the menus' units) is on: the last drawn that holds it (the deepest, on top), those
    over the whole screen passed over as the screen's own (menu_layout.c's menu_layout_widget_at); None"""
    widgets = [item for item in layout.get("items", []) if item["kind"] == "widget"]
    for widget in reversed(widgets):
        x0, y0, x1, y1 = widget["box"]
        if x0 <= x < x1 and y0 <= y < y1 and (x1 - x0 < 640 or y1 - y0 < 480):
            return widget
    return None


def widget_fields(layout: dict, widget) -> dict:
    """a widget as a note has it (the studio's menu_layout_widget_json): its name, its path down the screen,
    the words it and its children draw, its box"""
    if widget is None:
        return {"widget": None, "widget_path": [], "widget_text": "", "box": None}
    widgets = {item["id"]: item for item in layout.get("items", []) if item["kind"] == "widget"}
    path, step = [], widget["id"]
    while step in widgets and len(path) < 32:
        path.append(widgets[step]["name"])
        step = widgets[step]["parent"]
    words = []
    for item in layout.get("items", []):
        if item["kind"] != "text":
            continue
        owner = item.get("widget")
        while owner in widgets and owner != widget["id"]:
            owner = widgets[owner]["parent"]
        if owner == widget["id"]:
            words.append(" ".join(item.get("text", "").split()))
    return {"widget": widget["name"], "widget_path": list(reversed(path)), "widget_text": " ".join(words),
            "box": list(widget["box"])}


def widgets_in(layout: dict, area) -> list:
    """the widgets with words wholly in an area (the menus' units): their names, for an area's note"""
    x0, y0, x1, y1 = area
    widgets = {item["id"]: item for item in layout.get("items", []) if item["kind"] == "widget"}
    found = []
    for item in layout.get("items", []):
        if item["kind"] != "text" or not item.get("extent") or item.get("widget") not in widgets:
            continue
        ex0, ey0, ex1, ey1 = item["extent"]
        name = widgets[item["widget"]]["name"]
        if ex0 >= x0 and ey0 >= y0 and ex1 <= x1 and ey1 <= y1 and name not in found:
            found.append(name)
    return found[:12]


def variant_input_shape(variant: str):
    """the studio's input and shape names of a gallery's variant"""
    for (input_name, shape), name in STUDIO_VARIANTS.items():
        if name == variant:
            return input_name, shape
    return "keyboard", variant


def note_variant(note: dict) -> str:
    """the gallery's variant a note was seen in: the web's own, else the studio's input and shape's"""
    if note.get("variant"):
        return note["variant"]
    return STUDIO_VARIANTS.get((note.get("input"), note.get("shape")),
                               INPUT_VARIANTS.get(note.get("input"), "keyboard-16x9"))


def make_web_note(layout: dict, size, request: dict, run: str, picture: str = None) -> dict:
    """a note from the app: on the widget at a point, on an area (a drag), at a point (a pin) or on the screen
    (none of them), in the studio's notes' format with the web's fields"""
    kind = request.get("kind", "widget")
    image = {"run": run, "variant": request["variant"], "size": list(size)}
    widget = None
    area = None
    if kind == "widget":
        x, y = menu_point(layout, size, float(request["x"]), float(request["y"]))
        widget = widget_at(layout, x, y)
        image.update(x=round(float(request["x"])), y=round(float(request["y"])))
    elif kind in ("area", "point"):
        px0, py0 = float(request["x"]), float(request["y"])
        px1, py1 = (float(request.get("x1", px0)), float(request.get("y1", py0))) if kind == "area" else (px0, py0)
        px0, px1 = sorted((px0, px1))
        py0, py1 = sorted((py0, py1))
        mx0, my0 = menu_point(layout, size, px0, py0)
        mx1, my1 = menu_point(layout, size, px1, py1)
        area = [round(mx0), round(my0), round(mx1), round(my1)]
        image.update(x=round(px0), y=round(py0))
        if kind == "area":
            image.update(x1=round(px1), y1=round(py1))
    input_name, shape = variant_input_shape(request["variant"])
    tag = layout.get("screen_name", "")
    note = {"id": menu_notes.new_id(), "status": "open", "time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "note": str(request.get("note", ""))[:menu_notes.NOTE_LENGTH].strip(),
            "screen": menu_notes.menu_name(tag) or request.get("screen", ""), "screen_tag": tag,
            **widget_fields(layout, widget),
            "input": input_name, "shape": shape, "variant": request["variant"],
            "screen_width": layout["screen"][2] - layout["screen"][0],
            "source": "web", "author": str(request.get("author", "")).strip()[:60] or "reviewer",
            "kind": kind if widget is not None or kind != "widget" else "screen", "image": image,
            "picture": picture}
    if area is not None:
        note["area"] = area
        note["area_widgets"] = widgets_in(layout, area)
    return note


def outline_picture(source: Path, destination: Path, layout: dict, size, note: dict) -> Path:
    """the picture with what the note is about outlined (PIL's, where it is there; else as it is)"""
    destination.parent.mkdir(parents=True, exist_ok=True)
    box = note.get("box") or note.get("area")
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        shutil.copy2(source, destination.with_suffix(source.suffix))
        return destination.with_suffix(source.suffix)
    with Image.open(source) as opened:
        image = opened.convert("RGB")
    if box:
        x0, y0 = picture_point(layout, image.size, box[0], box[1])
        x1, y1 = picture_point(layout, image.size, box[2], box[3])
        if note.get("kind") == "point":
            x0, y0, x1, y1 = x0 - 14, y0 - 14, x0 + 14, y0 + 14
        width = max(2, image.width // 400)
        ImageDraw.Draw(image).rectangle([x0, y0, x1, y1], outline=(255, 255, 0), width=width)
    image.save(destination, "PNG")
    return destination


# ---------- before | after of a note fixed

def note_before_after(note: dict, build: Path = BUILD):
    """the newest run with the note's screen before and after in its variant (render's): (run, before, after
    relative paths) or None"""
    screen = next((screen for screen in menu_gallery.enumerate_screens(ROOT / menu_gallery.MENUS, True)
                   if screen.name == note.get("screen")), None)
    if screen is None:
        return None
    variant = note_variant(note)
    for name, folder, compared in find_runs(build):
        if not compared:
            continue
        before = picture_of(folder, "before", variant, screen.slug)
        after = picture_of(folder, "after", variant, screen.slug)
        if before and after:
            return name, before.relative_to(folder).as_posix(), after.relative_to(folder).as_posix()
    return None


def render_ref(notes: list) -> str:
    """the ref a fix batch is compared with: the parent of the earliest commit the done notes name (HEAD~1 when
    none does)"""
    commits = [note["commit"] for note in notes if note.get("status") in ("done", "closed") and note.get("commit")]
    earliest = None
    for commit in commits:
        if subprocess.run(["git", "-C", str(ROOT), "cat-file", "-e", f"{commit}^{{commit}}"],
                          capture_output=True).returncode:
            continue
        if earliest is None or subprocess.run(["git", "-C", str(ROOT), "merge-base", "--is-ancestor", commit,
                                               earliest], capture_output=True).returncode == 0:
            earliest = commit
    return f"{earliest}~1" if earliest else "HEAD~1"


def render(notes_path: Path, ref: str = None, extra: list = ()) -> tuple:
    """the noted screens again, before | after (menu_gallery.py --compare REF --notes), into a run of their own:
    (the run's folder, the exit status, its output's tail)"""
    ref = ref or render_ref(menu_notes.read_notes(notes_path))
    out = REVIEW / "runs" / f"fixes-{time.strftime('%Y%m%d-%H%M%S')}"
    command = [sys.executable, str(ROOT / "tools" / "menu_gallery.py"), "--compare", ref, "--notes", str(notes_path),
               "--out", str(out), "--no-fail", *extra]
    result = subprocess.run(command, capture_output=True, text=True)
    return out, result.returncode, (result.stdout + result.stderr)[-2000:]


# ---------- the pages

STYLE = """
:root { --bg:#0b0f14; --panel:#141b24; --line:#26313f; --text:#dbe4ee; --dim:#8796a8; --accent:#4aa3ff;
  --bad:#ff6b5e; --warn:#ffb648; --good:#57d17c; color-scheme: dark; }
@media (prefers-color-scheme: light) { :root { --bg:#f4f6f9; --panel:#fff; --line:#d7dde5; --text:#18212b;
  --dim:#5d6b7c; --accent:#0b6bd3; --bad:#c62f22; --warn:#a96500; --good:#1d8a43; color-scheme: light; } }
* { box-sizing: border-box; }
[hidden] { display: none !important; }
h1, h2, h3, p, li { overflow-wrap: anywhere; }
body { margin:0; background:var(--bg); color:var(--text); font:15px/1.45 system-ui,-apple-system,sans-serif; }
a { color: var(--accent); }
nav { display:flex; gap:14px; align-items:center; flex-wrap:wrap; padding:10px 16px; border-bottom:1px solid var(--line);
  position:sticky; top:0; background:var(--bg); z-index:5; }
nav b { margin-right:auto; }
main { padding:12px 16px 40px; max-width:1800px; margin:0 auto; }
h1 { font-size:20px; margin:8px 0; } h2 { font-size:16px; margin:18px 0 6px; }
.dim { color:var(--dim); font-size:13px; }
.grid { display:grid; gap:12px; grid-template-columns:repeat(auto-fill,minmax(min(100%,420px),1fr)); }
.card { background:var(--panel); border:1px solid var(--line); border-radius:12px; padding:10px; }
.card h3 { font-size:14px; margin:0 0 6px; word-break:break-word; }
.row { display:grid; gap:6px; grid-template-columns:repeat(auto-fit,minmax(min(100%,120px),1fr)); }
.row img, .pair img { width:100%; display:block; border-radius:6px; border:1px solid var(--line); }
.badge { display:inline-block; font-size:12px; padding:1px 8px; border-radius:99px; background:var(--warn); color:#000;
  margin-left:6px; }
.badge.quiet { background:transparent; border:1px solid var(--line); color:var(--dim); }
select, input, textarea, button { font:inherit; color:var(--text); background:var(--panel); border:1px solid var(--line);
  border-radius:8px; padding:7px 10px; }
button { cursor:pointer; } button.primary { background:var(--accent); color:#fff; border-color:var(--accent); }
.stage { position:relative; display:inline-block; max-width:100%; touch-action:none; }
.stage img { max-width:100%; display:block; user-select:none; -webkit-user-select:none; }
.stage canvas { position:absolute; inset:0; width:100%; height:100%; }
.tools { display:flex; gap:8px; flex-wrap:wrap; align-items:center; margin:8px 0; }
.form { display:grid; gap:8px; margin:10px 0; max-width:720px; }
.form textarea { min-height:80px; }
.form input, .form textarea { width:100%; }
figure { margin:0; }
.note { display:grid; gap:10px; grid-template-columns:minmax(0,320px) 1fr; }
@media (max-width:700px) { .note { grid-template-columns:1fr; } }
.pair { display:grid; gap:6px; grid-template-columns:1fr 1fr; }
.status-open { color:var(--warn); } .status-in_progress { color:var(--accent); }
.status-done { color:var(--good); } .status-closed { color:var(--dim); }
form.inline { display:inline; }
"""

SCREEN_SCRIPT = """
const img = document.getElementById('shot'), canvas = document.getElementById('over'), ctx = canvas.getContext('2d');
const info = document.getElementById('info'), form = document.getElementById('form');
let layout = null, mode = 'widget', picked = null, drag = null, area = null;
const notes = JSON.parse(document.getElementById('notes').textContent);
const author = document.getElementById('author');
try { author.value = localStorage.getItem('menu-review-author') || ''; } catch (e) {}
fetch(img.dataset.layout).then(r => r.ok ? r.json() : null).then(j => { layout = j; draw(); });
function size() { canvas.width = img.naturalWidth; canvas.height = img.naturalHeight; }
function toMenu(x, y) { const s = layout.screen; return [s[0] + x * (s[2]-s[0]) / canvas.width, s[1] + y * (s[3]-s[1]) / canvas.height]; }
function toPic(b) { const s = layout.screen; const sx = canvas.width / (s[2]-s[0]), sy = canvas.height / (s[3]-s[1]);
  return [(b[0]-s[0])*sx, (b[1]-s[1])*sy, (b[2]-b[0])*sx, (b[3]-b[1])*sy]; }
function widgetAt(x, y) { const ws = layout.items.filter(i => i.kind === 'widget');
  for (let i = ws.length - 1; i >= 0; i--) { const b = ws[i].box;
    if (x >= b[0] && x < b[2] && y >= b[1] && y < b[3] && (b[2]-b[0] < 640 || b[3]-b[1] < 480)) return ws[i]; }
  return null; }
function wordsOf(w) { const ws = {}; layout.items.filter(i => i.kind === 'widget').forEach(i => ws[i.id] = i);
  return layout.items.filter(i => { if (i.kind !== 'text') return false; let o = i.widget;
    while (o in ws && o !== w.id) o = ws[o].parent; return o === w.id; }).map(i => i.text.trim()).join(' '); }
function point(e) { const r = canvas.getBoundingClientRect(); const t = e.touches ? e.touches[0] || e.changedTouches[0] : e;
  return [(t.clientX - r.left) * canvas.width / r.width, (t.clientY - r.top) * canvas.height / r.height]; }
let hover = null;
function draw() { if (!img.naturalWidth) return; size(); ctx.clearRect(0, 0, canvas.width, canvas.height);
  const lw = Math.max(2, canvas.width / 500);
  if (layout) notes.forEach(n => { const b = n.box || n.area; if (!b) return; const p = toPic(b);
    ctx.strokeStyle = 'rgba(255,200,0,.9)'; ctx.lineWidth = lw; ctx.setLineDash([lw*3, lw*2]); ctx.strokeRect(...p); ctx.setLineDash([]); });
  if (layout && hover && !picked) { const p = toPic(hover.box); ctx.strokeStyle = 'rgba(80,255,120,.95)'; ctx.lineWidth = lw; ctx.strokeRect(...p); }
  if (picked && picked.box) { const p = toPic(picked.box); ctx.strokeStyle = '#ffff00'; ctx.lineWidth = lw * 1.6; ctx.strokeRect(...p); }
  if (area) { ctx.strokeStyle = '#ffff00'; ctx.lineWidth = lw * 1.6; ctx.strokeRect(area[0], area[1], area[2]-area[0], area[3]-area[1]); }
  if (drag) { ctx.strokeStyle = '#ffff00'; ctx.lineWidth = lw; ctx.strokeRect(drag[0], drag[1], drag[2]-drag[0], drag[3]-drag[1]); } }
img.addEventListener('load', draw); window.addEventListener('resize', draw);
document.querySelectorAll('[data-mode]').forEach(b => b.addEventListener('click', () => { mode = b.dataset.mode;
  document.querySelectorAll('[data-mode]').forEach(o => o.classList.toggle('primary', o === b)); reset(); }));
function reset() { picked = null; area = null; drag = null; form.hidden = true; info.textContent = 'Point at a widget; tap to note it. Area: drag. Point: tap.'; draw(); }
function choose(p) { form.hidden = false; form.querySelector('textarea').focus(); draw(); }
canvas.addEventListener('pointermove', e => { if (!layout) return; const p = point(e);
  if (drag) { drag[2] = p[0]; drag[3] = p[1]; draw(); return; }
  if (mode === 'widget' && !picked) { const m = toMenu(...p); hover = widgetAt(...m);
    info.textContent = hover ? hover.name.split('\\\\').pop() + (wordsOf(hover) ? ': ' + wordsOf(hover) : '') : 'the screen'; draw(); } });
canvas.addEventListener('pointerdown', e => { if (!layout) return; const p = point(e); canvas.setPointerCapture(e.pointerId);
  if (mode === 'area') { drag = [p[0], p[1], p[0], p[1]]; area = null; picked = null; } });
canvas.addEventListener('pointerup', e => { if (!layout) return; const p = point(e);
  if (mode === 'area' && drag) { area = [Math.min(drag[0], drag[2]), Math.min(drag[1], drag[3]), Math.max(drag[0], drag[2]), Math.max(drag[1], drag[3])];
    drag = null; if (area[2]-area[0] < 6 || area[3]-area[1] < 6) { area = null; draw(); return; }
    picked = {kind: 'area', x: area[0], y: area[1], x1: area[2], y1: area[3]}; info.textContent = 'An area'; choose(); return; }
  if (mode === 'point') { picked = {kind: 'point', x: p[0], y: p[1]}; area = [p[0]-12, p[1]-12, p[0]+12, p[1]+12];
    info.textContent = 'A point'; choose(); return; }
  const m = toMenu(...p); const w = widgetAt(...m);
  picked = {kind: 'widget', x: p[0], y: p[1], box: w ? w.box : null};
  info.textContent = w ? 'Note on ' + w.name.split('\\\\').pop() + (wordsOf(w) ? ' (' + wordsOf(w) + ')' : '') : 'Note on the screen as a whole';
  choose(); });
document.getElementById('cancel').addEventListener('click', reset);
form.addEventListener('submit', e => { e.preventDefault(); const text = form.querySelector('textarea').value.trim();
  if (!text || !picked) return; try { localStorage.setItem('menu-review-author', author.value); } catch (err) {}
  const body = {...picked, note: text, author: author.value, run: img.dataset.run, variant: img.dataset.variant,
    screen: img.dataset.screen, side: img.dataset.side};
  delete body.box;
  form.querySelector('button.primary').disabled = true;
  fetch('api/notes', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)})
    .then(r => r.json()).then(n => { form.querySelector('button.primary').disabled = false;
      if (n.error) { info.textContent = n.error; return; }
      notes.push(n); form.querySelector('textarea').value = ''; reset(); info.textContent = 'Saved (' + n.id + ').';
      document.getElementById('count').textContent = notes.length; }); });
reset();
"""


def page(title: str, body: str, script: str = "") -> bytes:
    return (f"""<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>{html.escape(title)}</title>
<style>{STYLE}</style></head><body><nav><b>Menu review</b><a href="./">Gallery</a><a href="notes">Notes</a>
<a href="runs">Runs</a></nav><main>{body}</main>{f'<script>{script}</script>' if script else ''}</body></html>
""").encode()


def esc(text) -> str:
    return html.escape(str(text if text is not None else ""))


class Review:
    """the app's state: its paths and secret, a lock for the notes file, the render's progress"""

    def __init__(self, secret: str, notes: Path, build: Path = BUILD):
        self.secret = secret
        self.notes = notes
        self.build = build
        self.lock = threading.Lock()
        self.rendering = None
        self.rendered = ""

    def counts(self) -> dict:
        found = {}
        for note in menu_notes.read_notes(self.notes):
            if note.get("status") in menu_notes.ACTIVE:
                found[note.get("screen")] = found.get(note.get("screen"), 0) + 1
        return found

    # (each page: its HTML)

    def gallery(self, query: dict) -> bytes:
        runs = find_runs(self.build)
        if not runs:
            return page("Menu review", "<h1>No gallery yet</h1><p>Make one: <code>python tools/menu_gallery.py</code>.</p>")
        name = query.get("run", runs[0][0])
        folder = run_folder(name, self.build) or runs[0][1]
        compared = (folder / "before").is_dir()
        counts = self.counts()
        options = "".join(f'<option value="{esc(run)}"{" selected" if run == name else ""}>{esc(run)}'
                          f'{" (before | after)" if has else ""}</option>' for run, _, has in runs)
        groups = {}
        for screen, variants in run_screens(folder):
            groups.setdefault(menu_of(screen), []).append((screen, variants))
        sections = []
        for menu, screens in groups.items():
            cards = []
            for screen, variants in screens:
                count = counts.get(screen.name, 0)
                badge = f'<span class="badge">{count} open</span>' if count else ""
                thumbs = "".join(
                    f'<a href="screen?run={urllib.parse.quote(name)}&screen={urllib.parse.quote(screen.name)}'
                    f'&variant={variant}"><img loading="lazy" alt="{esc(variant)}" '
                    f'src="file/{urllib.parse.quote(name)}/{self.thumb(folder, variant, screen.slug)}"></a>'
                    for variant in variants)
                cards.append(f'<div class="card"><h3>{esc(screen.label)}{badge}</h3><div class="row">{thumbs}</div>'
                             f'<p class="dim">{esc(screen.file)} &middot; {esc(screen.generator)}</p></div>')
            sections.append(f'<h2>{esc(menu.capitalize())}</h2><div class="grid">{"".join(cards)}</div>')
        top = (f'<form class="tools" method="get"><select name="run" onchange="this.form.submit()">{options}</select>'
               f'<span class="dim">{"Before | after: notes go on the after." if compared else ""}</span></form>')
        return page("Menu review", top + "".join(sections))

    @staticmethod
    def thumb(folder: Path, variant: str, slug: str) -> str:
        for name in (f"{slug}.thumb.jpg", f"{slug}.thumb.png", f"{slug}.jpg", f"{slug}.png"):
            if (folder / "after" / variant / name).is_file():
                return f"after/{variant}/{name}"
        return ""

    def screen(self, query: dict) -> bytes:
        name, screen_name, variant = query.get("run", "latest"), query.get("screen", ""), query.get("variant", "")
        folder = run_folder(name, self.build)
        screen = next((screen for screen in menu_gallery.enumerate_screens(ROOT / menu_gallery.MENUS, True)
                       if screen.name == screen_name), None)
        if folder is None or screen is None:
            return page("Not found", "<h1>No such screen</h1>")
        after = picture_of(folder, "after", variant, screen.slug)
        if after is None:
            return page("Not found", "<h1>No picture of it in that variant</h1>")
        variants = [v for v in menu_gallery.VARIANTS if picture_of(folder, "after", v, screen.slug)]
        tabs = "".join(f'<a href="screen?run={urllib.parse.quote(name)}&screen={urllib.parse.quote(screen_name)}'
                       f'&variant={v}">{"<b>" if v == variant else ""}{esc(v)}{"</b>" if v == variant else ""}</a> '
                       for v in variants)
        notes = [note for note in menu_notes.read_notes(self.notes)
                 if note.get("screen") == screen_name and note.get("status") in menu_notes.ACTIVE and
                 note_variant(note) == variant]
        before = picture_of(folder, "before", variant, screen.slug)
        before_html = (f'<details><summary>Before</summary><img style="max-width:100%" alt="before" '
                       f'src="file/{urllib.parse.quote(name)}/{before.relative_to(folder).as_posix()}"></details>'
                       if before else "")
        listed = "".join(f'<li><span class="status-{esc(note.get("status"))}">'
                         f'{esc(str(note.get("status")).replace("_", " "))}</span> '
                         f'{esc((note.get("widget") or "the screen").rsplit(chr(92), 1)[-1])}: {esc(note.get("note"))}'
                         f' <span class="dim">({esc(note.get("author") or note.get("source") or "studio")})</span></li>'
                         for note in notes)
        notes_json = json.dumps(notes).replace("</", "<\\/")
        body = f"""<h1>{esc(screen.label)}</h1><p class="dim">{esc(screen.name)} &middot; {esc(screen.file)} &middot;
{esc(screen.generator)} &middot; run {esc(name)}</p><p>{tabs}</p>
<div class="tools"><button data-mode="widget" class="primary">Widget</button><button data-mode="area">Area</button>
<button data-mode="point">Point</button><span id="info" class="dim"></span></div>
<div class="stage"><img id="shot" alt="{esc(screen.label)}" data-run="{esc(name)}" data-variant="{esc(variant)}"
data-screen="{esc(screen_name)}" data-side="after" data-layout="file/{urllib.parse.quote(name)}/after/{esc(variant)}/{esc(screen.slug)}.json"
src="file/{urllib.parse.quote(name)}/{after.relative_to(folder).as_posix()}"><canvas id="over"></canvas></div>
<form id="form" class="form" hidden><input id="author" placeholder="Your name" autocomplete="name">
<textarea placeholder="What should change?" maxlength="{menu_notes.NOTE_LENGTH}"></textarea>
<div class="tools"><button class="primary" type="submit">Save note</button><button type="button" id="cancel">Cancel</button></div></form>
{before_html}<h2>Open notes here (<span id="count">{len(notes)}</span>)</h2><ul>{listed}</ul>
<script type="application/json" id="notes">{notes_json}</script>"""
        return page(screen.label, body, SCREEN_SCRIPT)

    def notes_page(self, query: dict) -> bytes:
        notes = menu_notes.read_notes(self.notes)
        status = query.get("status", "active")
        chosen = [note for note in notes if
                  (status == "all" or (status == "active" and note.get("status") in menu_notes.ACTIVE) or
                   note.get("status") == status) and
                  (not query.get("screen") or query["screen"] in note.get("screen", "")) and
                  (not query.get("variant") or note_variant(note) == query["variant"]) and
                  (not query.get("author") or query["author"].lower() in str(note.get("author", "studio")).lower())]
        statuses = "".join(f'<option value="{value}"{" selected" if value == status else ""}>{label}</option>'
                           for value, label in (("active", "Open and in progress"), ("open", "Open"),
                                                ("in_progress", "In progress"), ("done", "Done"),
                                                ("closed", "Closed"), ("all", "All")))
        variants = "".join(f'<option{" selected" if v == query.get("variant") else ""}>{v}</option>'
                           for v in menu_gallery.VARIANTS)
        filters = (f'<form class="tools" method="get"><select name="status">{statuses}</select>'
                   f'<input name="screen" placeholder="Screen" value="{esc(query.get("screen", ""))}">'
                   f'<select name="variant"><option value="">Any variant</option>{variants}</select>'
                   f'<input name="author" placeholder="Author" value="{esc(query.get("author", ""))}">'
                   f'<button>Filter</button></form>')
        render_line = (f'<form class="tools" method="post" action="render"><button>Render the noted screens again</button>'
                       f'<span class="dim">{esc(self.rendering or self.rendered)}</span></form>')
        cards = []
        for note in reversed(chosen):
            picture = ""
            if note.get("picture") and safe_file(Path(note["picture"]).name, [self.notes.parent / "menu_notes"]):
                picture = (f'<a href="notefile/{esc(Path(note["picture"]).name)}"><img style="width:100%" '
                           f'loading="lazy" alt="note" src="notefile/{esc(Path(note["picture"]).name)}"></a>')
            fixed = ""
            if note.get("status") in ("done", "closed"):
                pair = note_before_after(note, self.build)
                if pair:
                    run, before, after = pair
                    fixed = (f'<div class="pair"><figure><img alt="before" src="file/{urllib.parse.quote(run)}/{before}">'
                             f'<figcaption class="dim">before</figcaption></figure><figure><img alt="after" '
                             f'src="file/{urllib.parse.quote(run)}/{after}"><figcaption class="dim">after</figcaption>'
                             f'</figure></div>')
                else:
                    fixed = '<p class="dim">No before | after yet: render the noted screens again.</p>'
            buttons = "".join(
                f'<form class="inline" method="post" action="api/notes/{esc(note.get("id"))}/status">'
                f'<input type="hidden" name="status" value="{value}"><button>{label}</button></form> '
                for value, label in (("in_progress", "Start"), ("open", "Reopen"), ("closed", "Approve"))
                if value != note.get("status") and not (value == "in_progress" and note.get("status") != "open")
                and not (value == "closed" and note.get("status") != "done"))
            where = (note.get("widget") or "").rsplit("\\", 1)[-1] or ("an area" if note.get("area") else "the screen")
            commit = f' &middot; fixed in {esc(str(note.get("commit"))[:9])}' if note.get("commit") else ""
            link = (f'screen?run=latest&screen={urllib.parse.quote(note.get("screen", ""))}'
                    f'&variant={note_variant(note)}')
            cards.append(f'<div class="card note"><div>{picture}</div><div><h3><span class="status-{esc(note.get("status"))}">'
                         f'{esc(str(note.get("status")).replace("_", " "))}</span> &middot; {esc(note.get("note"))}</h3>'
                         f'<p class="dim"><a href="{link}">{esc(note.get("screen"))}</a> &middot; {esc(where)} &middot; '
                         f'{esc(note_variant(note))} &middot; {esc(note.get("author") or "studio")} &middot; '
                         f'{esc(note.get("time"))} &middot; {esc(note.get("id"))}{commit}</p>{fixed}<p>{buttons}</p></div></div>')
        body = (f"<h1>Notes ({len(chosen)})</h1>{filters}{render_line}" +
                ("".join(cards) or '<p class="dim">No notes here.</p>'))
        return page("Notes", body)

    def runs_page(self) -> bytes:
        rows = "".join(f'<li><a href="./?run={urllib.parse.quote(name)}">{esc(name)}</a> '
                       f'<span class="dim">{"before | after" if has else "gallery"} &middot; '
                       f'{time.strftime("%Y-%m-%d %H:%M", time.localtime(folder.stat().st_mtime))}</span></li>'
                       for name, folder, has in find_runs(self.build))
        return page("Runs", f"<h1>Galleries</h1><ul>{rows}</ul><p class='dim'>A pull request's: "
                            "<code>python tools/menu_gallery.py --compare master --out build/menu_review/runs/pr-N</code>.</p>")

    # (the notes' changes)

    def add_note(self, request: dict) -> dict:
        run, variant, screen_name = request.get("run", ""), request.get("variant", ""), request.get("screen", "")
        folder = run_folder(run, self.build)
        screen = next((screen for screen in menu_gallery.enumerate_screens(ROOT / menu_gallery.MENUS, True)
                       if screen.name == screen_name), None)
        if folder is None or screen is None or variant not in menu_gallery.VARIANTS:
            return {"error": "no such screen"}
        if not str(request.get("note", "")).strip():
            return {"error": "an empty note"}
        side = "before" if request.get("side") == "before" else "after"
        layout_path = folder / side / variant / f"{screen.slug}.json"
        picture = picture_of(folder, side, variant, screen.slug)
        if not layout_path.is_file() or picture is None:
            return {"error": "that picture has no layout"}
        layout = json.loads(layout_path.read_text())
        size = picture_size(picture)
        note = make_web_note(layout, size, request, run)
        note["screen"] = screen_name
        out = self.notes.parent / "menu_notes" / f"web-{note['id']}.png"
        note["picture"] = str(outline_picture(picture, out, layout, size, note))
        with self.lock:
            menu_notes.append_note(self.notes, note)
        return note

    def set_status(self, identifier: str, status: str) -> dict:
        with self.lock:
            return menu_notes.set_status(self.notes, identifier, status, changed=time.strftime(
                "%Y-%m-%dT%H:%M:%SZ", time.gmtime()))

    def start_render(self):
        if self.rendering:
            return

        def work():
            out, status, tail = render(self.notes)
            self.rendered = f"Rendered {out.name}" if status == 0 else f"The render failed: {tail[-300:]}"
            self.rendering = None

        self.rendering = "Rendering the noted screens..."
        threading.Thread(target=work, daemon=True).start()


def picture_size(path: Path):
    """a picture's size: PNG's and JPEG's headers read (PIL's where it is there)"""
    try:
        from PIL import Image
        with Image.open(path) as image:
            return image.size
    except ImportError:
        pass
    data = path.read_bytes()
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")
    index = 2
    while index < len(data):
        marker, length = data[index + 1], int.from_bytes(data[index + 2:index + 4], "big")
        if marker in (0xC0, 0xC2):
            return int.from_bytes(data[index + 7:index + 9], "big"), int.from_bytes(data[index + 5:index + 7], "big")
        index += 2 + length
    raise ValueError(f"no size in {path}")


def local_next(where: str) -> str:
    """where a sign-in link goes on to: a page of the app's own (a relative path), else the gallery"""
    if not where or where.startswith(("/", "\\")) or ":" in where.split("?", 1)[0] or "//" in where:
        return "./"
    return where


def make_handler(review: Review):
    class Handler(BaseHTTPRequestHandler):
        server_version = "MenuReview"

        def log_message(self, format, *args):
            pass

        def send(self, status: int, body: bytes, kind: str = "text/html; charset=utf-8", headers=()):
            self.send_response(status)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Frame-Options", "DENY")
            self.send_header("Referrer-Policy", "no-referrer")
            for name, value in headers:
                self.send_header(name, value)
            self.end_headers()
            self.wfile.write(body)

        def redirect(self, where: str, headers=()):
            self.send(303, b"", headers=[("Location", where), *headers])

        def authorized(self) -> bool:
            return signed_in(self.headers.get("Cookie", ""), review.secret)

        def login_page(self, wrong=False):
            message = '<p class="status-open">That is not the secret.</p>' if wrong else ""
            self.send(401 if wrong else 200, page("Sign in", f"""<h1>Menu review</h1>{message}
<form class="form" method="post" action="login"><input type="password" name="secret" placeholder="Secret"
autocomplete="current-password" autofocus><button class="primary">Sign in</button></form>"""))

        def body(self) -> bytes:
            length = int(self.headers.get("Content-Length", "0") or 0)
            return self.rfile.read(min(length, 65536))

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            query = {key: values[-1] for key, values in urllib.parse.parse_qs(url.query).items()}
            path = url.path.lstrip("/")
            if path == "login":
                # (a sign-in link, login?key=<secret>, for a phone: the cookie set, the key gone from the address)
                given = query.get("key", "")
                if given and hmac.compare_digest(given.encode(), review.secret.encode()):
                    cookie = f"{COOKIE}={token_of(review.secret)}; Path=/; HttpOnly; SameSite=Lax; Max-Age=2592000"
                    return self.redirect(local_next(query.get("next", "")), [("Set-Cookie", cookie)])
                return self.login_page()
            if not self.authorized():
                return self.redirect("login")
            if path == "":
                return self.send(200, review.gallery(query))
            if path == "screen":
                return self.send(200, review.screen(query))
            if path == "notes":
                return self.send(200, review.notes_page(query))
            if path == "runs":
                return self.send(200, review.runs_page())
            if path == "api/notes":
                return self.send(200, json.dumps(menu_notes.read_notes(review.notes)).encode(), "application/json")
            if path.startswith("file/"):
                run, _, relative = urllib.parse.unquote(path[5:]).partition("/")
                folder = run_folder(run, review.build)
                found = safe_file(relative, [folder]) if folder else None
                return self.file(found)
            if path.startswith("notefile/"):
                return self.file(safe_file(urllib.parse.unquote(path[9:]), [review.notes.parent / "menu_notes"]))
            self.send(404, page("Not found", "<h1>Not found</h1>"))

        def file(self, found):
            if not found or found.suffix.lower() not in (".jpg", ".png", ".json"):
                return self.send(404, b"not found", "text/plain")
            kind = mimetypes.guess_type(found.name)[0] or "application/octet-stream"
            self.send(200, found.read_bytes(), kind, [("Cache-Control", "private, max-age=60")])

        def do_POST(self):
            path = urllib.parse.urlsplit(self.path).path.lstrip("/")
            if path == "login":
                form = urllib.parse.parse_qs(self.body().decode(errors="replace"))
                given = form.get("secret", [""])[-1]
                if hmac.compare_digest(given.encode(), review.secret.encode()):
                    cookie = f"{COOKIE}={token_of(review.secret)}; Path=/; HttpOnly; SameSite=Lax; Max-Age=2592000"
                    return self.redirect("./", [("Set-Cookie", cookie)])
                time.sleep(1.0)
                return self.login_page(wrong=True)
            if not self.authorized():
                return self.send(401, b'{"error": "sign in"}', "application/json")
            if path == "api/notes":
                try:
                    request = json.loads(self.body() or b"{}")
                    note = review.add_note(request)
                except (ValueError, KeyError, TypeError) as error:
                    note = {"error": f"not saved: {error}"}
                return self.send(200, json.dumps(note).encode(), "application/json")
            match = re.fullmatch(r"api/notes/([0-9a-f]+)/status", path)
            if match:
                form = urllib.parse.parse_qs(self.body().decode(errors="replace"))
                try:
                    review.set_status(match.group(1), form.get("status", [""])[-1])
                except (KeyError, ValueError) as error:
                    return self.send(400, page("Not changed", f"<h1>Not changed</h1><p>{esc(error)}</p>"))
                return self.redirect(self.headers.get("Referer") or "../../../notes")
            if path == "render":
                review.start_render()
                return self.redirect("notes")
            self.send(404, b"not found", "text/plain")

    return Handler


def watch_brief(review: Review, out: Path):
    """when notes arrive, tools/menu_notes.py's brief of the open ones written for an agent"""
    seen = None
    while True:
        try:
            stamp = review.notes.stat().st_mtime_ns if review.notes.exists() else 0
        except OSError:
            stamp = 0
        if stamp != seen:
            seen = stamp
            active = [note for note in menu_notes.read_notes(review.notes) if note.get("status") in menu_notes.ACTIVE]
            text = menu_notes.brief(active) if active else "No open notes.\n"
            part = out.with_suffix(".part")
            part.write_text(text)
            part.replace(out)
        time.sleep(NOTES_WATCH_SECONDS)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="The menus reviewed on the gallery's pictures.", allow_abbrev=False)
    parser.add_argument("action", choices=("serve", "render", "secret"))
    parser.add_argument("--host", default="0.0.0.0", help="the address to listen on (a LAN's, Tailscale's...)")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--secret", help="the login's secret (MENU_REVIEW_SECRET; else build/menu_review/secret)")
    parser.add_argument("--notes", type=Path, default=menu_notes.NOTES, help="the notes file")
    parser.add_argument("--ref", help="render: the ref to compare with (the done notes' earliest commit's parent)")
    parser.add_argument("--watch-brief", nargs="?", const=BUILD / "menu_notes_brief.txt", type=Path, metavar="FILE",
                        help="serve: write the open notes' brief there as they arrive (build/menu_notes_brief.txt)")
    arguments = parser.parse_args(argv)
    secret = secret_of(arguments.secret)
    if arguments.action == "secret":
        print(secret)
        return 0
    if arguments.action == "render":
        out, status, tail = render(arguments.notes, arguments.ref)
        print(tail, file=sys.stderr)
        print(out / "index.html")
        return status
    review = Review(secret, arguments.notes)
    if arguments.watch_brief:
        threading.Thread(target=watch_brief, args=(review, arguments.watch_brief), daemon=True).start()
    server = ThreadingHTTPServer((arguments.host, arguments.port), make_handler(review))
    print(f"menu_review: http://{arguments.host}:{arguments.port}/ (the secret: tools/menu_review.py secret)",
          file=sys.stderr, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
