# Demos

A demo is a recording of a game, tick by tick, that plays back exactly: the
same game state after every tick as the game that was recorded, as a
Counter-Strike demo is. Every game this computer plays or hosts is recorded
as one (`demo.record`): the campaign, split screen, games with bots, a
network game it hosts, a dedicated server's. They are the base for clips,
bug reports' reproductions, kill cams and the render inspector.

The code is `port/linux/game/demo.c` (what a demo holds, recording and
playback) and `port/linux/src/demo_files.c` (the files, their writer and
their retention; `demo_unpack.c` reads their blocks). `tools/demo_info.py`
prints what a demo holds.

## Where they are

Demos are kept in the data root's `demos/` folder (the folder with `maps/`),
one file a map: `20261108-014642-a10.demo`, the time it began and the map.
As a new demo begins, the oldest are deleted until no more than
`demo.keep_count` (20) are kept and they take no more than
`demo.keep_megabytes` (2048 MB). A demo moved into `demos/kept/` is pinned:
it is never deleted, nor counted.

`halo --demo <file>` (or `demo.play`) plays one: its map loads at its
difficulty and its game plays as it was recorded. Nothing of the player's
own saved games is read or written meanwhile (their checkpoint, their last
level). With `debug.demo_quit` the game quits when the demo ends, for tests.

## What a demo holds, and why

The game's simulation, its ticks, is a function of the game state (one block
of memory: `source/saved games/game_state.c`), the two random seeds the ticks
take (`source/math/random_math.c`) and the players' input of each tick
("Determinism" in `port/linux/README.md`). So a demo holds:

- **The start.** A snapshot of the game state (each of its allocations' part
  in use: a data array to its count, a memory pool to its last block) and
  the seeds, at the map's first tick (a network game's first: its lobby is
  not recorded).
- **Each tick's input.** Every player's action as the tick takes it
  (`players.c`'s `players_update_before_game`): this machine's players', the
  bots' and the other machines' players' that the netcode gave it, and the
  seeds where the bots took them. At `game.sim_rate` 60 and 120, each later
  sub-step's controls of the players' units (`player_control.c`'s slot of the
  fast phase): their buttons, sticks, trigger and aim, and the presses seen
  in it.
- **The frames' input the ticks read.** The player control globals, which
  the frames write from the controllers and which the scripts test (Pillar of
  Autumn's tutorial asks the player to look up and down), whole at each
  tick's start that they changed by.
- **What reached the game state from outside the ticks.** At each tick's
  start, and each sub-step's when the tick was open across frames, the
  allocations the ticks keep are compared with what the tick before left,
  and the bytes that changed are recorded: a player who joined or left, a
  host's decision, a console command, an effect a particle made as it
  struck. When the game state was rewritten as a whole (a checkpoint
  reverted to, a saved game or core loaded, the map started over), or the
  game's code made or deleted an object outside the ticks, the whole game
  state is recorded instead, as at the start.
- **What the netcode put in inside a tick.** A host takes its clients'
  predictions of their own units at the tick's start
  (`network_distributed.c`): what that changed is recorded, and the
  choices it makes by which machines are there (a client's hits are its
  report's to deal: `network_damage.c`).
- **A hash of the game state once a second**, and of each allocation, which
  the playback compares with its own.
- **A snapshot every `demo.snapshot_seconds`** (10), for seeking.
- **Bookmarks** (the clip key) and the game's variant.

The frames' own allocations are neither compared nor hashed: the particles,
decals, contrails, lights (with their cluster references), the motion
sensor, the first-person weapon posed at the camera, the sound's state, the
game time globals (`tools/determinism_compare.py`'s `FRAME_ALLOCATIONS`).
Nor are the objects' fields that the frames write as well as the ticks: an
object's magic number (the collision tests' mark), its render state's
index, its attachments that are the frames' datums.

## Playback

A demo's playback loads its map as a game of this machine (a multiplayer
game's with its variant), and at its first tick takes the start's snapshot.
Then each tick runs with the recorded input in place of the players': this
machine's controllers, the bots and the other machines play no part. At each
tick's start, what the playback's own frames wrote into the allocations the
ticks keep is put back as the tick before left it, and what the recording
had from outside the ticks is written in; a whole game state recorded is
taken as a saved game is (`game_state.c`: the loading's procedures before
and after, the game state checked between them, then written again over
what the procedures changed).

While a demo plays, the keyboard's own controls drive the playback, not a
player: strafing left and right (A, D) seeks 10 seconds back and on, to the
snapshot nearest before that point; moving forward and back (W, S) plays
twice as fast or half as fast (from a quarter to eight times); jumping
(space) pauses. The console's line shows where the playback is.

The playback checks its hash against the recording's once a second. If they
differ, it says where in the log ("parted from the recording by tick 1680:
effect"), names the allocations that differ, and plays on as its own game.

## The file

A header of 512 bytes (`OH1DEMO`, the format's version, the build, the
network version, the game state's allocations' checksum, the connection,
the rate, the difficulty, the map), then blocks, each packed with zlib on a
thread of its own as the game plays (`demo_writer_*`): a second of records
(`R`), a snapshot the playback takes (`K`: the start, a reload) or one only
a seek takes (`k`). A record is its type (a byte), its length (4 bytes) and
its bytes:

| Record | Holds |
| --- | --- |
| `T` | a tick begins: its number, its rate |
| `K` | the game state: each allocation's part in use, and the seeds |
| `P` | what changed in the game state since the tick before ended |
| `M` | the same, before a sub-step of a tick left open across frames |
| `C` | the player control globals, whole |
| `S` | the seeds |
| `A` | the tick's players' actions (`struct player_action`) |
| `U` | a later sub-step's units' controls |
| `F` | what the netcode put in inside the tick |
| `D` | a choice the netcode made |
| `H` | the game state's hash, and each allocation's |
| `V` | the game's variant and its options |
| `B` | a bookmark: its tick and how far back it reaches |
| `E` | the end |

A block cut short (the game crashed or was killed) ends the demo there; the
blocks before it play. A demo is a file anyone may send: its playback checks
every range it writes into the game state (each wholly in one allocation; a
snapshot's each an allocation from its start), the game state after each
as a saved game's is checked, every input as the netcode checks another
machine's, and unpacks with the port's zlib (`port/third_party/zlib`).

A demo of another build is refused with the reason: its build (the release's
number and commit; a local build's `local`), its platform (Linux x86-64 or
arm64, Windows, macOS, iOS, Android, VR or not), its network version and its
game state's layout must be this build's. The simulation is the code, and a
snapshot holds the guest's code's addresses (the caches' procedures, as a
saved game does), each build's own.

## Clips

The clip key (`controls.clip`, F9) marks the demo being recorded where it
is pressed: a bookmark of its last `demo.clip_seconds` (30), which
`tools/demo_info.py` lists. (Not yet: a controller's button for it, its row
in Controls Setup, and the export of a bookmark to video.)

## Sizes and cost

Measured on the Linux x86-64 build (`tools/demo_info.py`):

| Game | MB a minute |
| --- | --- |
| Pillar of Autumn (a10), the campaign's start, 60 Hz | 1.0 |
| The Silent Cartographer (b30), fighting, a death every 20 s, 60 Hz | 2.2 |
| Blood Gulch Slayer, a player and 3 bots, 60 Hz | 0.5 |
| Blood Gulch Slayer hosted, a client, 120 Hz | 0.3 to 2 |

Most of it is the snapshots for seeking (about 150 KB packed each in the
campaign) and the whole game states of reloads (a revert's). Recording
takes about 0.1 ms of the game's thread a tick (the comparison of the
allocations the ticks keep, about 1 MB, with what the tick before left).

## What is checked

`debug.determinism_log` hashes the game state after every tick;
`tools/determinism_compare.py` compares a recording's log with its
playback's (`--simulation`: not the frames' allocations; `--builds`: not
the seeds' takers' sites; `--game`: from a network game's first tick, after
its lobby):

```sh
HALO_DETERMINISM_LOG=rec.txt HALO_TEST_INPUT=bot:3 HALO_SKIP_CINEMATICS=1 ./halo   # init.txt: map_name levels\a10\a10
HALO_DETERMINISM_LOG=play.txt HALO_DEMO_PLAY=demos/<demo> HALO_DEMO_QUIT=1 HALO_DEMO_RECORD=0 ./halo
python tools/determinism_compare.py --simulation --builds \
    --ignore "object list header" --ignore "list object reference" rec.txt play.txt
```

Alike at every tick: a10 at 30, 60 and 120 (with `debug.frame_time`, and in
real time); played back at 144 frames a second from a recording at 60; The
Silent Cartographer (b30) fighting, 4 minutes with 12 deaths and their
reverts; Blood Gulch Slayer with three bots at 60, in real time; a hosted
game at 120 alone; on macOS with the Metal renderer, in real time. Every
recorded hash matched in those playbacks.
The object lists' free elements are left out: the playback's loading leaves
other bytes in them, which no tick reads. `tools/test_demo_files.py` checks
the files without the game: a demo written and read back, a damaged block
refused, the retention.

## Not yet

- **A game joined on another machine** is not recorded: its client decides
  nothing, and much of what it has comes from the host inside its ticks.
- **A hosted game with clients** plays back exactly until the netcode's
  work outside the recorded points reaches the game: a client's reported hit
  dealt between two sub-steps of an open tick, whose effects (made by the
  game's code) the bytes recorded do not bring with them. The playback's
  hash says where.
- **Effects the frames make** (a particle striking) are recorded as data,
  and the ticks update them: their particles in the playback are the
  playback's own.
- **A demo recorded with `debug.frame_time`** plays back exactly with it
  (any frame time), not in real time: the sound manager's clock reaches the
  game state ("Determinism" in `port/linux/README.md`), and with the frames'
  time fixed it is the frames' time. A demo recorded in real time plays back
  in real time.
- **Slayer with bots**: of three runs, two played back every hash as
  recorded, the third two hashes apart (the objects, six seconds in) and as
  recorded again after.
- **The problem report** does not attach the last 30 seconds yet: a demo
  holds the players' names (the game state's), which a public issue must not.
- **A seek back** can part from the recording (the AI's actors, the
  objects): what the game keeps outside its game state (the checkpoints'
  timers, the main loop's) is where the playback was, not where it goes.
  A seek on plays on as recorded (checked: every hash after one).
- The free camera and the other players' views, the export to video and
  the Demos screen come next.
