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
| `port/linux/game/menu_screen.c` | The screens made into widgets, and the registry: actions, bindings, row sources, and which screen a hand-written one replaces |
| `port/linux/game/menu_screen_tall.inc` | The tall layout's blocks (a screen with a `<table>`): tabs, the table, its panel, searches and the spread bar, made and updated each frame |
| `port/linux/game/menu_table.h` | A table's row source: what C gives a `<table>` each frame, and the menus' text fields and fitted text the blocks use (menu_functions.c's) |
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
| `<button>` | `key` (`accept`, `back`, `x`, `y`, `lb`, `rb`, `lt`, `rt`, `r3`, `view`), `label`, `side`, `do`, `width`, `if`, `when` | One hint in the bar: the key's picture (on the keyboard, the key the menus use for it) and its word. `do` is the action the key (anywhere on the screen) and a click run; without it, the key is pressed. In a tall screen's bar (below), `width` is its least width, `if` a binding that must have a value for it to be shown, and two buttons of one key share one place, shown as their `if`s say |
| `<colors>` | `id` | The player profile's colors, one item each, under one another in its name and its color (white while focused). It is the screen's list (a screen has one `<list>`, `<cards>` or `<colors>`). The profile's own color is focused first. A chooses the color for the profile being edited, as the PC version's color picker did, and goes back |
| `<spartan>` | `x`, `y`, `width`, `height`, `color` (`focus`, `profile`), `players` (1 to 4) | The Spartan, which the game draws (port/linux/game/menu_spartan.c) standing in this box of the 640 by 480 canvas, its feet at the box's foot, as tall as the box. It is drawn in a view of its own where the block is in the screen's order: over the screen's background and the blocks before it, under those after, with its own fixed camera, depth and light, so nothing of the menus' scene (the ring, the sky) cuts it and the scene's camera never moves or tilts it. It takes no room in the layout. `color="focus"` follows the focused `<colors>` item; `profile` (the default) is the color of the profile being edited, else the player's own, live as it changes. `players` puts that many side by side, each its local player's profile's color. Where there is no Spartan (no multiplayer map exported one, a refused file, `display.menu_pictures` off, a random color, no profile), the box shows the original picture of the color (ui.map's `player_color_marine_large`, frame 18 for random). It is gone as soon as its screen closes |

### The tall layout: tabs, tables, panels and searches

A screen with a `<table>` is laid out as Settings' and the server browser's
generated screens are (the tall layout, `tools/port_settings.py`'s
`BROWSER_TALL`): the whole height of the screen, the `<header>`'s title in
the band at the top, the `<tabs>` under it, the table at the left with its
`<panel>` beside it at the right, and the `<buttons>` at the foot. The
blocks' places are the tall layout's (`menu_screen_tall.inc`), not a flow from
the top, and a `<space/>` there does nothing. On a 16:9 screen the table, its
columns and the panel are as much wider as their layout says, and past 16:9
they reach the screen's edges; the tabs and the bar keep 16:9's places. On a
touchscreen (`debug.touch_menus`, the phones' builds) the tabs are chips, a
row is two lines of its cells (each column's `touch` place), the columns have
no titles, and the bar's buttons are a touchscreen's, BACK at the right.

| Element | Attributes | Meaning |
| --- | --- | --- |
| `<header>` | `title`, `picture`, `detail` | (in a tall screen) its title's words in the band, `detail` at the band's right (on a touchscreen, under the title), and a `<button>` in the band |
| `<tabs>` | `current`, `if`, `when` | A strip of tabs: `current` names the binding whose value is the current tab's id (framed, its words white). LB and RB (Q and E) run the `do` of the tab either side, the keys' prompts beside the strip; the d-pad steps the strip, A or a click runs a tab's `do` |
| `<tab>` | `id` (required), `label` (required), `do` (required), `when` | One tab |
| `<table>` | `source` (required), `sorted`, `do`, `empty` | A live table: its rows come from the row source C names `source` (below), as many as fit over the panel's foot, scrolling at their ends. `do` is the action A, a click or a double click on a row runs (the row given to it); `sorted` names the binding whose value is the column sorted by and its way (`players down`); `empty` the words in the rows' place while there are none |
| `<column>` | `id` (required), `width` (required), `label`, `kind`, `grow`, `gap`, `align`, `sort`, `do`, `title-width`, `touch`, `when` | One column, from the left: `kind` its cells, `text` (the default), `strong` (white words), `ping` (the ping's bars and its number) or `star` (a favorite's star). `width` and `gap` (the room before it, 4) in the menus' units; `grow`, how much wider it is at 16:9; `align` its words at its `right`. Its title (`label`) runs `sort` when chosen, its arrow after the words when `sorted` names it; `title-width` its title's width, if not its words'. `do`: a click on a cell of it runs that, not the table's. `touch`: its place in a touchscreen's row, `x y width move grow` |
| `<search>` | `value` (required), `label`, `if`, `when` | A search: in a `<column>`, its magnifying glass after the column's title, the field typed into in the title's place and its X; at the screen's level, a field of its own at the top's right (a touchscreen's). A, a click or a tap types into it; each letter typed sets the binding `value` (the list filtered at once); Enter, A again or the focus taken off it keeps the words; B and Esc clear them; the X clears them |
| `<button>` | (in a `<table>`) | A button under the table's words for none (CLEAR SEARCH), shown as its `if` says, the focus on it as it comes |
| `<panel>` | `if` | The details beside the table: its lines one after another from its top, and a `<button>` at its foot |
| `<heading>` | `text` (required), `lines`, `touch-lines`, `when` | The panel's heading, large and white, wrapped to its lines |
| `<text>` | `text` (required), `lines`, `touch-lines`, `when` | A line of the panel's, wrapped to its lines (`touch-lines` on a touchscreen) |
| `<pills>`, `<pill>` | `text` (required), `width` (required), `tone` | A row of pills (as many to a line as fit): each its words, its frame `tone` (a number, or a binding giving one) |
| `<pair>` | `label` (required), `value` (required), `when` | A label at the left and its value, large, at the right |
| `<button>` | (in a `<panel>`) | The panel's button at its foot (JOIN GAME), shown as its `if` says |

A panel's line whose text has a binding of no value (or none at all) is not
shown, and its place stays. Each text of a tall screen (the header's detail,
the panel's lines, the buttons' labels, the words for none) is filled again
every frame, and the `if`s read, so a binding's value can change while the
screen is open.

## Row sources

A table's rows are C's: `source` names a `struct menu_table_source`
(`menu_table.h`) in `menu_screen.c`'s `screen_sources`, by its name in
`screen_source_names`. Each frame the table (the view) tells the source what
it shows: its rows, the row focused, whether the pointer resting on it put the
focus there, whether the titles are shown. The source (the model) answers how
many rows there are, which is first on the table, which is chosen (the panel's
row, its picture brighter), and which row of the table to give the focus (a
row moved, a heading passed over, a scroll at the table's ends), and then
gives each row: a heading across it, or its cells by their columns' ids, each
its words, their colour, a picture's frame (a ping's bars, a star) and a pill
or a lock after the words. The table cuts words too long for their column
short with "...", puts the pill and the lock after them, and leaves a ping's
bars out where they would meet its number. The actions a row runs get the row
(0 up, on the table).

The reader checks each file whole. A file is refused if it has any of these
problems: bad XML; an element where it may not be; an attribute an element
does not have; an item with no label or no `do`; two items with one id; a
list's `focus` that names no item; a `when` that names no platform; a `key`
other than the four; an action or binding the game does not have. The
problem is logged with its file and line, for example `menus:
screens/main.xml:6: <list> is not one of what its element may hold; the
screen stays as it was`. A refused file never crashes the game:

A tall screen's file is refused, too, for: a `<tabs>` with no `<tab>`, a tab
with no id, label or `do`, two tabs or columns of one id; a `<table>` with no
source, a source the game has not got, no `<column>`, a column with no id or
width, a `kind`, `align` or `touch` it may not have; a `<search>` with no
value; a `<panel>` with nothing in it, a `<pill>` with no width, a `<pair>`
with no value; a binding named alone (`current`, `sorted`, `value`, `if`,
`tone`) that the game has not got; a second `<tabs>`, `<table>` or
`<panel>`.

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
| `{browser.title}`, `{browser.counts}`, `{browser.empty}`, `{browser.host}` | The server browser's title (SERVER BROWSER, LOCAL NETWORK, JOIN BY LINK), its counts (`20 GAMES    24 PLAYERS`, `2 OF 18 GAMES ...` while some are hidden), its words while it lists none, its HOST button's (HOST & MORE, HOST CO-OP) |
| `{browser.tab}`, `{browser.sort}`, `{browser.sort.title}` | The tab shown (`all`, `coop`...), the sort (`players down`), its column's title (PLAYERS) |
| `{browser.search.server}`, `{browser.search.map}`, `{browser.search.any}` | The searches' words (set as they are typed) |
| `{browser.panel.title}` ... `{browser.panel.status}` | The chosen game's name (or the list's title), where (map and mode), kind (OPENH1, LEGACY) and its `tone`, tick rate, OPENCE CAN JOIN, players, ping, and its status line |
| `{browser.searched}`, `{browser.joinable}`, `{browser.server}`, `{browser.link}`, `{browser.filtered}` | (for `if`s) the searches hide every game; a game to join is chosen; the server browser's list; Join by Link's; a list filtered and searched (not Join by Link's) |

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
| `open multiplayer`, `open coop` | The server browser, as Multiplayer's SERVER BROWSER opens it (`open coop`: on its CO-OP tab, this visit) |
| `open host`, `open host coop` | Multiplayer's hub (the server browser's HOST & MORE), its co-op page (HOST CO-OP) |
| `open settings` | Settings, as the main menu's SETTINGS opens it |
| `open training` | The VR tutorial (Basic Training's chapters) |
| `quit` | The quit question, as the main menu's QUIT and B open it |
| `search next` | The next of the screen's searches typed into (the first, if none is) |
| `search clear` | Every search's words cleared, as a table's CLEAR SEARCH |
| `browser back` | The browser's back (its own: `gamespy back handler`) |
| `browser host` | HOST & MORE, or HOST CO-OP on the co-op visit |
| `browser tab <tab>` | The tab shown (`all`, `dedicated`, `openh1`, `legacy`, `coop`, `favorites`) |
| `browser sort <column>`, `browser sort next` | Sorted by the column (`players`, `ping`, `server`, `map`, `mode`; again the other way), or the next (X) |
| `browser favorite` | The row's game (or the chosen one) made a favorite, or no longer one |
| `browser join` | The row's game chosen, or joined if it is chosen (a password asked for first, the connecting box opened); JOIN GAME's |
| `browser refresh` | The public games asked for again; Join by Link's PASTE LINK |
| `browser filters` | The Filters page |

A file is checked against `screen_action_names` as it is read, and
`tools/harness/tests/test_menu_screens.py` checks that every name there has an
entry in `screen_actions`.

## The server browser

`screens/browser.xml` is the server browser of the new engine: it takes the
place of Join Game's screen (`main_menu/multiplayer_type_select/join_game/
join_game_screen`) in each of its modes, by `{browser.*}` bindings and `if`s:

- the server browser (the main menu's MULTIPLAYER), with its HOST & MORE;
- on its CO-OP tab (Campaign's CO-OP, `open coop`: the tab this visit, not
  the player's own kept), with HOST CO-OP;
- LOCAL NETWORK (Multiplayer's hub, which HOST & MORE opens): the games on
  this network, its favorites offline after them; no REFRESH nor HOST;
- JOIN BY LINK (the hub): the invites taken; no tabs, searches, FILTERS nor
  SEARCH, PASTE LINK in REFRESH's place, and an invite link copied joined
  as it opens.

The classic engine, PRO 2V2's browser and a refused or missing file keep the
generated screen (and its HOST button, `screen_browser_opened`).

What it lists and does is menu_functions.c's, as for the generated screen: the
games found, their order, the groups and their headings, the filters, the
searches, the favorites, the tab and the sort kept in `config.toml`, joining
(the password screen, the connecting box, the A press posted once the host is
reached) and the panel's words. Its model (`lobby_browser_model`,
`local_browser_model`) works out each frame what is listed, which row is
first and which chosen, and the panel's and the counts' words
(`browser_display`), for its two views: the generated screen's list
(`lobby_browser_update`, `browser_update`) and the hand-written screen's table
(`pc_menu_browser_source`, the `browser.*` bindings and actions). The screen's
stand-in (Join Game's own widget) still opens and closes the browser
(`gamespy screen init`, `gamespy screen dispose`).

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
   to `screen_build` (a tall screen's to `menu_screen_tall.inc`). A table's
   rows: a row source in `screen_sources`.
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
