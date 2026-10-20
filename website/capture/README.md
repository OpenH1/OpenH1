# Capturing the site's clips

The scripts that capture, survey and encode the comparison clips for
openh1.com (`VIDEOS-TODO.md` lists the clips). They run on a Linux machine
with a built `linux_x86_64` tree (`python configure.py --release`, `ninja
linux_x86_64`) and the retail maps, drawn offscreen (SDL's offscreen driver),
a BMP a frame.

| Script | What it does |
| --- | --- |
| `capture.sh` | One side of a pair: the Original (the Classic preset, one frame a tick, the Xbox's 640 × 480) or OpenH1 (Enhanced, frames between ticks, at `SIZE`); the HUD hidden through the telnet console once the map draws |
| `survey.py` | Candidate spots for a clip, both sides each, ten games at a time: scored from the sim trace and the frames, the frames then deleted, a few kept as PNGs |
| `rank.py` | The survey's usable candidates, best first, and a contact sheet of them |
| `watch.sh` | Prints each survey result as it lands |
| `encode.sh` | A pair's MP4, WebM and poster, frame for frame, the Original scaled by nearest neighbour |

Settings are environment variables: `TREE` (the built tree), `MAPS`, `RUNS`
(where runs go), `SIZE` (OpenH1's window), `PORT` (the console's; distinct
for runs side by side), `FT` (OpenH1's frame time: 1/60 for the clips, 1/30
in a survey), `SEED`, `TRACE_SECONDS`; for `encode.sh`, `WIDTH`, `HEIGHT`,
`MP4_CRF`, `WEBM_CRF`, `FADE`.

## How a pair lines up

Both sides play the same script (`debug.test_input`, timed on the
simulation's clock) with fixed frame times, so the simulation is the same
tick for tick. The Original draws a frame a tick; OpenH1 at 1/60 draws two.
A frame's tick is found from the sim trace: the run's frames are its
loading frames and then one a tick, so the frame of tick `k` is the frame
count less the trace's ticks plus `k`. (An `aim=` in the script snaps the
view at once, which also makes a clear mark in both.)

For 4K: OpenH1 at `SIZE=3840x2880` (4:3, the Xbox's field of view), the
Original's 640 × 480 scaled six times (`WIDTH=3840 HEIGHT=2880`); a 16:9
crop of both (3840 × 2160) keeps the six.

## Loops without a fade

A clip loops by cutting from its last frame to its first, so the two must
match. For the grenade: one grenade thrown before the clip starts, so the
scorch mark is already there, and a second at the same spot in the clip,
which ends when its smoke has gone and the frame most like the first comes.
A spot is no good if the blast moves or hurts the player (the view jumps at
the cut): the survey measures both from the trace.

## Running them

On the capture machine, the scripts in a folder of their own (here
`~/halo-capture-runs/tools`), the runs beside it:

```sh
# a survey: candidates scored, the best drawn on a sheet
python tools/survey.py ratrace,wizard --out survey-lights            # grenades (the default script)
python tools/rank.py survey-lights 8                                 # survey-lights/sheet.png
# a final pair at 4K, its loop chosen, masters encoded into final/out
python tools/final.py lights "levels\test\ratrace\ratrace" "0 aim=90,-5;1 throw;5 throw" 4.5 7.6 9.8 --seed 7
```

`survey.py` takes `--yaws`, `--pitch` (lists), `--script` (with `{yaw}` and
`{pitch}`), `--event boom|fire`, `--console` (commands sent once the map
draws), `--seconds` (of simulation, then it quits), `--seed`. Its games run
with `nohup` so that a dropped connection does not stop them; `follow.sh
<log> <pattern>` prints a log as it grows.

Things learnt doing it:

- Do not copy a script over one that is running: bash reads a script as it
  runs it. Copy it beside, then `mv`.
- `debug.trace` must be a relative path (it lands in the data root); the
  game cannot open an absolute one.
- `debug.trace_seconds` quits the game at a time of simulation: much
  sooner than `debug.exit_after`'s wall clock.
- The trace has more steps than ticks (a tick can have several): count
  frames by the steps' ticks.
- A multiplayer spawn did not change with the seed on the maps tried.
- `show_hud 0` in `init.txt` comes too early (and prints its result): send
  it through the console once the map draws, as `capture.sh` does.
- A grenade thrown at a wall close by comes back and kills the player
  (35 of the 80 lights candidates): the survey drops those.

### A weapon the player does not start with

`cheat_all_weapons` (sent with `--console`) drops the globals' weapon list
in a ring 1.5 world units around the player, 22.5° apart, in the list's
order (`game_engine.c`: assault rifle, flamethrower, gravity rifle, needler,
pistol, plasma pistol, plasma rifle, rocket launcher, shotgun, sniper rifle,
ball, flag, frag and plasma grenades; `cheats.c`, `cheat_objects`). Walk to
one and hold action to swap it in; the trace's `e new` lines name what the
weapon fires, which says what was picked up. On Rat Race (seed 7): aim
yaw 26 to 41 and pitch -25, walk forward 0.5 s at 3 s, action at 4.5 s:
the plasma rifle, every time (yaw 45 to 65 gives the rocket launcher, 75
to 105 the sniper rifle).

## Surveys

`surveys/<clip>-<date>/`: `results.jsonl` (a line a candidate), `sheet.webp`
(the best eight: the Original and OpenH1 at the blast, then the loop's first
and last frames) and `picks.webp` (the ones chosen to look at).

### lights, 2026-10-20

Ten multiplayer maps (Derelict, Rat Race, Prisoner, Damnation, Chill Out,
Boarding Action, Hang 'Em High, Longest, Wizard, Chiron TL-34), the seed 7
spawn, eight aims (yaw 0 to 315, pitch -5), a grenade at 1 s and another at
5 s. 80 candidates: 35 killed the player (the first grenade bounced back),
31 usable (no knockback or damage, the same spot twice, a loop of at least
33 dB). The best by eye:

1. Rat Race, yaw 90: a side room; the Original lights the whole room and its
   ceiling yellow, OpenH1 a pool around the blast. Loop 42 dB.
2. Rat Race, yaw 0: a dark pillared corridor, lit far down by the Original.
   Loop 39 dB.
3. Wizard, yaw 45: the courtyard's back wall, lit evenly by the Original.
   Loop 43 dB.
4. Hang 'Em High, yaw 180: milder, mostly the left wall.

The two that scored highest (Hang 'Em High yaw 135, Damnation yaw 45) have
the blast at the frame's edge or in a bright room. The `light_diff` score
is the mean difference between the light each side adds at the blast (the
blast frame less one before it), so it counts the sprite's edges too: look
at the sheet.

## Machines

- The 5900x (RTX 4090): no hangs; about 3 s a candidate in a survey, ten
  games at a time.
- omarchy (Radeon 8060S, RADV): the game stops on the loading screen when
  drawn offscreen there (RADV has headless surfaces, so it makes a
  swapchain; with vsync on or off). Not looked into.
- macOS: see `VIDEOS-TODO.md` ("Two problems on macOS").
