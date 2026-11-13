# The hand-written screens

The menu engine (`display.menu_engine = "new"`, [menu_engine.md](menu_engine.md))
draws the generated screens from the generators' model. That takes three
layers for one screen: Custom Edition's tags, the Python generators that
write `port/assets/menus/ce` and the model, and the C that reaches widgets
by Custom Edition's names. The hand-written screens replace those layers, one
screen at a time, with a file of our own that says what the screen is, and C
that says what its actions do. The main menu is the first.

The classic engine (`display.menu_engine = "classic"`) is not touched: it keeps
the PC version's screens, generated as before, and its parity check.

## Where things are

| Path | What it is |
| --- | --- |
| `port/assets/menus/screens/<name>.xml` | One screen a file: its building blocks, words and actions |
| `port/assets/menus/screens/blocks.xml` | The building blocks' widgets, written by hand in the menus' own format (`port/assets/menus/README.md`) |
| `port/linux/src/menu_screens.c`, `port/linux/include/menu_screens.h` | The reader: a file read with Expat into blocks and checked; hot reload |
| `port/linux/game/menu_screen.c` | The screens made into widgets, and the registry: actions, bindings, and which screen a hand-written one replaces |
| `tools/harness/tests/test_menu_screens.py` | The reader's tests, asset-free |

Release builds embed the files with the other menu files (`menus.json`,
`tools/embed_assets.py`). A file of the same name in a menus folder
(`debug.menu_files`, or `menus` beside `config.toml`) replaces the embedded
one, as for every menu file.

## The format

```xml
<screen name="main" back="quit">
	<title-art/>
	<list focus="campaign">
		<item id="training" label="VR TUTORIAL" when="vr" hint="Learn to move, aim and fight in the headset" do="open training"/>
		<item id="campaign" label="CAMPAIGN" do="open campaign">
			<hint>Continue · {campaign.level} · {campaign.difficulty}</hint>
			<hint>The story · solo, or with friends online</hint>
		</item>
		<item id="versus" label="VERSUS" hint="Slayer, CTF, 2v2, Infection · find or host a game" do="open versus"/>
		<item id="settings" label="SETTINGS" hint="Video, sound, controls and your profile" do="open settings"/>
		<item id="quit" label="QUIT" do="quit"/>
	</list>
	<space/>
	<buttons>
		<button key="accept" label="SELECT"/>
		<button key="back" label="QUIT"/>
	</buttons>
</screen>
```

A file describes what is on the screen and what it does, not where each
thing is in pixels. The blocks are laid out from the top of the screen down,
in the menus' 480-unit-tall canvas (the same canvas the tall layout uses). A
`<space/>` takes the height left over, so a `<space/>` before `<buttons>` puts
the bar at the foot of the screen at every aspect ratio. Centred blocks stay
centred, and the bar's wide layout reaches the screen's edges up to 16:9.

| Element | Attributes | Meaning |
| --- | --- | --- |
| `<screen>` | `name` (required), `back` | The screen. `back` is the action for B and Back; without it, B goes back to the screen before |
| `<title-art/>` | | Halo's title art, where the main menu has it. If the screen's blocks need more height than the canvas has, this block gives up some of its room |
| `<list>` | `id`, `focus` | A list of items. The d-pad, the stick and the mouse wheel move through it, wrapping from the last item to the first, as the classic lists do. `focus` is the id of the item focused when the screen first opens. After that the screen remembers the last focused item |
| `<item>` | `label` (required), `do` (required), `id`, `hint`, `when` | One item. A, Start, a click or a tap runs `do`. `hint` is the line shown under the item while it is focused. Write `<hint>` children instead to give alternatives: the first whose bindings all have values is shown. `when` lists platforms (`desktop android vr hdr touch pointer computer`, as for `platform=` in the menu files) and the item is made only on one of them |
| `<space/>` | | Takes the height left over (several spaces share it) |
| `<buttons>` | | The button bar: each button a prompt, as Choose Mission's footer has them (its key's picture and word, lit while it has the focus; A, a click or a tap presses its key, accept running the chosen item's action), BACK at the left and the rest from the right, past 16:9 at the screen's edges, and on a touchscreen raised above a phone's safe area with the tall layout's footer (`foot`) |
| `<button>` | `key` (`accept`, `back`, `x`, `y`), `label` | One hint in the bar: the key's picture (on the keyboard, the key the menus use for it) and its word |
| `<colors>` | `id` | The player profile's colors, one item each, under one another in its name and its color (white while focused). It is the screen's list (a screen has one `<list>`, `<cards>` or `<colors>`). The profile's own color is focused first. A chooses the color for the profile being edited, as the PC version's color picker did, and goes back |
| `<spartan>` | `x`, `y`, `width`, `height`, `color` (`focus`, `profile`), `players` (1 to 4) | The Spartan, which the game draws in the menus' scene (port/linux/game/menu_spartan.c) standing in this box of the 640 by 480 canvas, its feet at the box's foot, as tall as the box. It is placed before the menus' camera each frame, so it stands beside the menu at every aspect ratio. It takes no room in the layout. `color="focus"` follows the focused `<colors>` item; `profile` (the default) is the color of the profile being edited, else the player's own, live as it changes. `players` puts that many side by side, each its local player's profile's color. Where there is no Spartan (no multiplayer map exported one, a refused file, `display.menu_pictures` off, a random color, no profile), the box shows the original picture of the color (ui.map's `player_color_marine_large`, frame 18 for random). It is gone as soon as its screen closes |

The reader checks each file whole. A file is refused if it has any of these
problems: bad XML; an element where it may not be; an attribute an element
does not have; an item with no label or no `do`; two items with one id; a
list's `focus` that names no item; a `when` that names no platform; a `key`
other than the four; an action or binding the game does not have. The
problem is logged with its file and line, for example `menus:
screens/main.xml:6: <list> is not one of what its element may hold; the
screen stays as it was`. A refused file never crashes the game:

- an embedded or menus-folder file that is refused at start leaves no
  hand-written screen, and the screen the file would have replaced is drawn
  from the model as before;
- a file that is refused when it is read again (hot reload) keeps the last
  good version of its screen.

## Bindings

Text in a `label`, `hint` or `<hint>` can contain `{name}`, which is filled
from C when the screen is made. If any binding in a line has no value, the
whole line is left out: an item whose label is left out is not made, and an
item uses the next of its `<hint>`s. The bindings are listed in
`menu_screen.c`'s `screen_binding_names`, and their values come from
`screen_value`:

| Binding | Value |
| --- | --- |
| `{campaign.level}` | The saved game's mission (player 1's profile), for example `The Silent Cartographer`; none without a saved game, or in co-op |
| `{campaign.difficulty}` | The saved game's difficulty, for example `Legendary` |

## Actions

An action is a name, such as `do="open versus"` or `back="quit"`, which C
resolves in `menu_screen.c`'s `screen_actions`. The files never name the
game's widgets. Each action is what the old screen's item did: the function
it ran, the screen opened if that succeeded, and the screen opened if it
failed (a new profile's name is asked for first). Each screen is opened
through the menus' own handler code, so the history (B comes back), the
sounds and the fades are the game's.

| Action | Does |
| --- | --- |
| `open campaign` | The Campaign screen, as the PC version's CAMPAIGN opens it (a profile made first if there is none) |
| `open versus` | The server browser (the full-height one), as Multiplayer's SERVER BROWSER opens it |
| `open settings` | Settings, as the main menu's SETTINGS opens it |
| `open training` | The VR tutorial (Basic Training's chapters) |
| `quit` | The quit question, as the main menu's QUIT and B open it |

A file is checked against `screen_action_names` as it is read, and
`tools/harness/tests/test_menu_screens.py` checks that every name there has an
entry in `screen_actions`.

## How a screen is made

`screen_places` in `menu_screen.c` names the screen a hand-written one
replaces (`main` replaces `main_menu/main_menu`). The game opens that screen as
before: `ui_widget.c` makes its stand-in widget and keeps it in the history.
The engine (`menu_engine_screen_opened`) then asks `menu_screen_opened`, which
deletes the stand-in's own children and makes the screen's blocks in their
place. Each block is a widget of `screens/blocks.xml` (`screens/title_art`,
`screens/list`, `screens/item`, `screens/hint`, `screens/prompt`). It gets
its words (`parameters.text_box.text`) and its place (its offsets) from the
file. From then on the engine draws the widgets and takes their events
exactly as it does for any screen's widgets. Focus, the controller, the
keyboard, the mouse and touch (their pointer targets come from the widgets
drawn), the menus' sounds and the high-res text are therefore the game's own.
The blocks are drawn from their own definitions, not from the model's looks
(`menu_tags.c` gives them no look). Before each frame is drawn
(`menu_screen_frame`), the screen shows the focused item's hint, and the main
menu notes that it is shown, which the update and crash questions and the
Party screen open over (`pc_menu_main_menu_shown`).

Because the items share one widget tag, the engine gives the focus to the
item widget itself, not to the first widget of its tag
(`ui_widget_port_give_focus`).

## Hot reload

`debug.menu_screens` (`HALO_MENU_SCREENS`) names a folder of screen files
(normally a source tree's `port/assets/menus/screens`). If it is empty,
`debug.menu_files`' `screens` folder is used when that setting is set. The
game looks at the folder twice a second, on the menus' clock
(`menu_screens_poll`). A file that changed is read again, and an open screen
whose file was read again is made again, with the focus kept on the item of
the same id. A file with a problem is logged and its screen stays as it was.
Hot reload works in any build, release or not, but only while the setting is
set, and only on desktops.

```sh
HALO_MENU_SCREENS=$PWD/port/assets/menus/screens build/linux_x86_64/halo
# edit port/assets/menus/screens/main.xml: the main menu changes within half a second
```

The blocks' widgets (`blocks.xml`) are menu files like any other: the menu
studio's reload (`debug.menu_studio`, F7) reads them again.

## Adding a screen

1. Write `port/assets/menus/screens/<name>.xml`, and add it to
   `port/assets/menus/menus.json` so that releases embed it.
2. In `port/linux/game/menu_screen.c`:
   - add each new action to `screen_action_names` and `screen_actions`;
   - add each new binding to `screen_binding_names` and give it a value in
     `screen_value`;
   - if the screen takes the place of a generated one, add it to
     `screen_places` (a screen opened only by an action needs no place yet:
     step 2 opens hand-written screens directly).
3. A block that does not exist yet: add its widget to `blocks.xml`, its
   element to the reader (`menu_screens.c`, `menu_screens.h`) and its layout
   to `screen_build`.
4. Tests: add a case to `tools/harness/tests/test_menu_screens.py` for what
   the reader must accept or refuse. Take screenshots of the screen at 16:9,
   16:10 and 4:3, and make a scripted run (`debug.test_input "menu:ready
   ..."` with `debug.menu_open`) that presses through it.
5. If it replaces a screen that `tools/menu_parity.py` compares, add the
   screen to `HAND_WRITTEN` there. The classic engine still draws the old
   screen; the new engine's side must have drawn the hand-written blocks.

## The profile color screen

`screens/profile_color.xml` takes the place of Settings' COLOR screen
(`color_edit/color_select_screen`): PROFILE COLOR in the band at the top,
the 18 colors at the left (`<colors>`), and the Spartan beside them in the
focused color (`<spartan color="focus">`), with its rifle and its idle. A
chooses the color and goes back; B goes back without one. The classic engine
keeps its own screen and the original pictures.

## The main menu: four doors

`screens/main.xml`: CAMPAIGN, VERSUS, SETTINGS and QUIT under the title art
(and VR TUTORIAL first in the headset), each focused item's hint under it,
and the button-hint bar at the foot of the screen.

The main menu no longer offers the PC version's MULTIPLAYER and PRO 2V2
items. Under the new engine, these are unreachable from the menus until later
steps rebuild them:

- the Multiplayer hub itself, and its ONLINE NOW and BEST FOR YOU panel;
- LOCAL NETWORK (the local network's games) and JOIN BY LINK. A link opened
  from outside the game still joins;
- CREATE GAME: hosting on the Internet, on the local network, or offline (a
  game of this machine alone, with bots);
- EDIT GAMETYPES (also reached from Server Setup, which hosting opens, so
  that is unreachable too);
- PARTY and FRIENDS from the hub. The Party screen still opens over the main
  menu when a party's game is followed;
- PRO 2V2's own screen (matchmaking a 2v2, hosting one). Its games stay
  findable through the server browser's `pro_2v2` game type filter.

Co-op stays reachable: CAMPAIGN, then CO-OP, opens the co-op page (online
co-op hosting and split-screen co-op).

There are two temporary ways in. `display.menu_engine = "classic"` has the old
main menu. `debug.menu_open =
main_menu/multiplayer_type_select/multiplayer_type_select_screen` starts on
the Multiplayer hub, from which everything above is reached.
