# The menu engine

The generated menus (`tools/ce_menus.py`, `tools/port_settings.py`) are
today flattened into Halo's widget engine: each `<widget>` of
`port/assets/menus/ce` becomes a widget definition tag (`menu_tags.c`),
which `ui_widget.c` runs as the Xbox's own. What a screen *means* is lost on
the way and found again at run time: a row's help is the string after its
label's (`settings_row_help`, index + 1), a slider's range is a table copied
by hand into `menu_functions.c` (`settings_sliders`), a row's platform is
decided as the file is read (`menu_files.c`'s `for_this_platform`, so a row
for a capability found later is never there: #853), and which rows show is
a row-name predicate run each frame (`video_rows_show`, `gamma_row_show`,
...).

The menu engine renders the generators' model instead: what the generators
already hold as data, written as data, read by the game, and drawn with
Halo's own primitives so that the look is the same. `display.menu_engine`
chooses it (`"classic"`, the default, or `"new"`). The map-tag menus (the
in-game pause menu, `display.menus = "xbox"`) stay on Halo's engine.

## The model

`tools/ce_menus.py` writes, beside each `ce/<file>.xml`, the screens of that
file as the model: `port/assets/menus/model/<file>.xml`, read by the same
reader as the menus (`menu_files.c`, expat), embedded the same way
(`menus.json`). The XML is unchanged by it; the generators stay the one
source of both. The model is XML rather than JSON because the guest already
reads XML and has no JSON reader.

```xml
<model>
  <!-- a settings tab or page: port_settings.py's PAGES -->
  <screen name="main_menu/settings_select/.../video_settings/video_settings_screen"
          kind="settings" tab="VIDEO" title="main_menu/settings/header_settings">
    <about>The window, the frame rate and the graphics.</about>
    <section title="DISPLAY"/>
    <row key="mode" kind="choice" setting="display.mode" when="computer">
      <label>WINDOW</label>
      <help>How the game takes the screen. ...</help>
      <choice shown="FULLSCREEN" value="fullscreen"/> ...
    </row>
    <row key="fov" kind="slider" setting="display.fov"
         least="80" most="160" step="5" typed_least="20" typed_most="160">
      <label>FIELD OF VIEW</label><help>...</help><note>Click the number ...</note>
      <stop shown="..." value="..."/> ...
    </row>
    <row key="frame_rate_sync" kind="link" open="..."><label>FRAME RATE &amp; SYNC ›</label>...</row>
    <row key="command_1" kind="binding" .../>
    <row key="discord_account" kind="status" run="..."/>
    <row key="remap_player" kind="stepped"/>
    <button kind="defaults"/> <button kind="ok" run="port settings ok"/> <button kind="cancel" .../>
  </screen>
  <!-- every other screen: its tree of nodes, as the XML's widgets -->
  <screen name="main_menu/multiplayer_type_select/lobby/lobby_screen" kind="nodes">
    <node name="..." type="text" box="x0 y0 x1 y1" wide="dx dw" when="touch" .../>
  </screen>
</model>
```

- **Kinds of screen.** `settings` (a Settings tab or page, and the setup
  options' screens of rows), whose rows are their meaning, laid out by the
  engine from the generator's geometry (`SETTINGS_*`, carried in the model's
  header so that the C and the Python never disagree); `nodes`, every other
  screen (the PC version's from its tags, the lobby, the server browser, the
  campaign, the dialogs), a tree of nodes with the XML widgets' attributes.
- **Kinds of row.** `choice` (a spinner: two values are a toggle), `slider`
  (its range, step, typed range and stops: the table `menu_functions.c`
  copies today goes), `link` (opens a page or runs a function), `binding`
  (Key Bindings' slot pair), `status` (a value the game writes), `stepped`
  (left and right step a list `menu_functions.c` keeps), `text` (a field typed
  into). Each has its label, help and note: no index + 1.
- **Conditions** (`when`): an expression of the platforms
  (`desktop android web vr touch pointer computer`), capabilities found at
  run time (`hdr`, `vrr`, `refresh_rates`, ...), and states (`in_game`,
  `profile`, `vr_playing`), with `!`, space for *or*, `+` for *and*. They are
  evaluated as the screen is laid out, each frame, not as the file is read:
  a capability found after start-up shows its rows. The predicates
  `menu_functions.c` runs by row name today move into the model one by one
  as conditions; until each has, it keeps working (below).
- **Actions** are the XML's (`open`, `replace`, `back`, `close`, `run`,
  `branch`/`otherwise`, `sound`), on a row, a button or a node.

## The engine

`port/linux/game/menu_engine.c` (layout, focus, input), `menu_engine_draw.c`
(drawing), `menu_engine_model.c` (the model read and its conditions), in the
guest, compiled as the game's sources are.

- **A screen** opened under `display.menu_engine = "new"` is a tree of
  engine nodes made from the model. Each node has its box in the 640x480
  menu units, laid out each frame: `wide`, `anchor_*`, `stretch` and
  `keep_ends` as `menu_tags.c` and `ui_widget.c` apply them, a settings
  screen's rows packed from the top with their sections (what
  `settings_rows_pack` does after the fact today), hidden rows taking no
  room.
- **Drawing** uses the primitives `ui_widget.c` draws with, called the same
  way: `draw_bitmap_in_rect`, `draw_bitmap_widened_in_rect` (pictures,
  framed panels, a focused row's frame), `rasterizer_draw_unicode_string` and
  `draw_string_and_hack_in_icons` (text, through the high-res text path:
  `display.text_renderer`, MSDF), the key prompts (`hud_key_prompt_*`), the
  carets and rounded pictures (`menu_shapes.c`), the colours
  (`get_ui_argb_white`, the text's focus white, `pc_menu_text_color`) and the
  fade's alpha. The port's helpers in `ui_widget.c` that lay text out (its
  wrap and fit, #523 and #689, the key labels and legends, the caret) are
  made callable (`port:` exports of the static functions, unchanged), not
  copied. Every draw is recorded for `debug.menu_layout` through
  `menu_layout.c`, and every pointer target noted through
  `ui_mouse_note_target`, as `ui_widget.c` records them.
- **Input**: the events `ui_widget.c` makes from the controller, keyboard,
  mouse and touch (`process_ui_widgets`, `ui_widgets_process_mouse`) come to
  the screen's stand-in widget and on to the engine, which moves the focus
  (up and down a screen's rows, left and right a button bar or a spinner,
  its tabs on LB and RB), repeats held directions on `ui_widget.c`'s clock,
  and runs the actions. Sounds are `ui_play_audio_feedback_sound`'s.
- **The game's functions** (`menu_functions.c`, 12,900 lines that read and
  write widget instances by name: `named(list, "op_gamma")->visible`) run
  unchanged on both engines through a small node interface,
  `menu_node.h`: a node's name, parent, children, visibility, disabled,
  offsets, focused child and selected item, picture frame, string index and
  text. `struct widget_instance` and the engine's node both implement it;
  `menu_functions.c` moves to it mechanically (its `->visible` becomes
  `menu_node_visible()`, and so on). Behaviour is then the same code on both
  engines, which is what makes parity of behaviour provable; the row-name
  predicates are then replaced by the model's conditions screen by screen,
  each change a diff the parity harness checks.

## The hooks

Three `port:` hooks in `ui_widget.c`, each a call into the engine when it
owns the widget:

1. `ui_widget_load_by_name_or_tag`: a generated screen opened while
   `display.menu_engine` is `"new"` is made as one stand-in widget (a
   container, the screen's size, with the screen's flags: `pause_game`,
   `no_history`, ...), whose instance the engine keeps its tree on
   (`menu_engine_screen_open`). History, the stack, fades and `close all`
   stay Halo's.
2. `widget_instance_render_recursive`: the stand-in is drawn by the engine
   (`menu_engine_screen_draw`), with the offset, clip and alpha it is given.
3. `widget_instance_process_one_event_recursive`: an event for the
   stand-in goes to the engine (`menu_engine_screen_event`), which answers
   handled or not, as a widget does. (Its deletion, `ui_widget_delete`,
   frees the tree: a fourth, one-line call.)

## Parity, proved by machine

The bar: identical layout, identical pixels and identical behaviour,
allowing only the differences listed below as deliberate.

1. **Layout.** `tools/menu_parity.py --layout` runs `tools/menu_gallery.py`'s
   tour (`debug.menu_tour`) twice, once on each engine, for every screen x
   aspect (4:3, 16:10, 16:9, 21:9) x input (controller, keyboard and mouse,
   touch), `debug.null_renderer`, and diffs the `debug.menu_layout` JSON of
   each pair: every widget's box and clip, every string's text, box, clip and
   extent, every glyph and every pointer target. The stand-in's own record
   is dropped; the engine names its nodes as the widgets were named.
2. **Pixels.** `debug.menu_parity` (the tour's mode): each screen is drawn
   by the classic engine and then by the new one into the same frame size,
   both read back in memory as the Screenshot key reads the frame
   (`d3d8_device.c`), and compared there; the log has each screen's count of differing pixels and their
   bounds. No picture is written.
3. **Behaviour.** `tools/menu_parity.py --behaviour` runs scripts of
   `debug.test_input "menu:ready ..."` on each engine from the same
   `config.toml` and player profile, and compares where the focus was after
   each press, the resulting `config.toml`, the profile's files, the
   settings the log says were written and the last screen's layout. The
   presses are counted from when the screen opened is up, on the frames'
   clock (`debug.frame_time`, which the menus' clock follows), so that a
   press lasts as many of the menus' frames in every run, however many runs
   share the computer; `menu_tour.c` logs the focus at the end of each
   second. The profile is made first by a script (a name typed into the
   profile's field) and copied into each run. The scripts: each kind of row
   (a spinner, a toggle, a slider by keys, by a click, held and by typing, a
   binding slot, by the mouse's button and wheel, a sub-page kept and
   cancelled), the tabs by LB and RB and by a click, the wheel, Defaults, OK
   and Cancel by the pad and by clicks, with the keyboard and mouse, a
   controller and touch. `--sweep` adds the navigation sweep: every generated
   screen opened, the focus moved about it (down, up, left, right, held), with
   no press that leaves it. `--baseline` runs the classic engine on both
   sides, to show the scripts give the same results run to run.

`tools/menu_parity.py` runs all three and fails on any difference not in
its list of deliberate ones.

## Deliberate differences

None yet. Each one added is listed here with its reason.

## Where it ends, and the steps there

The engine is worth having only at its end: one source, the generators'
model, rendered by one engine. Until then, a screen's look comes from its
widgets (the flattened XML, `menu_tags.c`), its meaning from the model, and
its behaviour from `menu_functions.c` on `widget_instance`s: three places,
more than before. Each step below removes one, and each is proved by
`tools/menu_parity.py` (layout, pixels and, from step 2, behaviour) before it
merges, with `display.menu_engine` still `"classic"` by default.

The end:

- the model holds everything a generated screen is: its layout (bounds,
  anchors, sections, rows) and its look (bitmaps, colours, fonts), as well
  as its meaning (labels, help, choices, ranges, conditions, actions);
- the engine draws, lays out, focuses and runs every generated screen from
  the model alone;
- the screens' behaviour is handlers on the model's rows and actions, not
  `menu_functions.c` reading `widget_instance` fields by name;
- `port/assets/menus/ce`'s XML and its widget definitions stay only for the
  menus the maps carry (the in-game pause menu, `display.menus = "xbox"`),
  which Halo's engine keeps;
- `display.menu_engine` is retired, the engine the only one for those
  screens.

The steps, in order:

1. **Drawn by the engine** (done): the model written for the settings
   screens, the engine drawing every generated screen under `"new"`, the
   rows laid out and their help found from the model; 972 of 972 screen
   variants alike in layout and pixels.
2. **Input and behaviour by the engine**: focus, navigation, pad, keys,
   mouse, touch and repeat, the rows' actions, sliders' ranges and stops
   from the model (`settings_sliders` gone), the rows' conditions from the
   model (the row-name predicates gone); the parity tool's behaviour scripts,
   config.toml and the profile compared after each.
3. **The look in the model**: bounds, anchors and the look written by the
   generators into the model for every generated screen; the engine reads
   them there, not from widget definitions.
4. **Behaviour off `widget_instance`**: each screen's handlers in
   `menu_functions.c` moved onto the model, a batch of screens at a time.
5. **The switch**: `"new"` the default, played for a while; then the
   generated screens' widget XML no longer written, `display.menu_engine`
   retired.

Features that need the model (scrolling long strings, #899; one dialog
style, #901; search; live values) come after step 2, on the engine alone.
