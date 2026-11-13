# UI bridge — specification, protocol version 1

Status: **schema 1.0**. The engine side lives in `engine/darkplaces/touch_uib*.c`, the
QuakeC builder and screens in `qcsrc/menu/xonotic/touch_uib_*.qc`, the JSON Schema in
[`ui-bridge.schema.json`](ui-bridge.schema.json), fixtures in `tests/uibridge/fixtures/`.

## 1. What this is

The menu VM (QuakeC) describes every screen **once**, as a tree of semantic nodes: what is
on the screen, its current state and what can be done. The engine keeps that tree in a
store and exports it. Each platform renders it in its own way:

| Platform | Renderer | Transport |
| --- | --- | --- |
| Android | Kotlin views (`android/**`, branch `work/androidui`) | in-process C API, called from the JNI bridge (`touch_frontend.c`) |
| Linux | GTK 4 / libadwaita app (`launcher/**`, branch `work/linuxui`) | local stream: socketpair fd or Unix socket (section 9) |
| Web, and the fallback everywhere | QuakeC renderer in the menu VM (`touch_uib_render.qc`) | in-process, reads the store through builtins |

The description says **what**: structure, content, state, actions, and a few hints. It
never says how in pixels: renderers choose widgets, sizes, spacing, typography, icons and
motion (transitions, shimmer placeholders, parallax on preview cards, haptics). Two
renderers showing the same snapshot will look different and must both be correct.

Owner rules that the descriptions follow and renderers must not undo:

- Home holds a hero Play card, Singleplayer, Multiplayer, Settings, Credits and Quit. Nothing else.
- Multiplayer is one Servers screen: a search field, one row of checkboxes (LAN, Empty,
  Full, Laggy) and Refresh, the dense list, an address field and Join. LAN is just one of
  those checkboxes. There is no Internet | LAN | Host switch, no filters sheet and no
  hosting screen.
- Hosting is part of Create: a LAN checkbox; when ticked, Port and an optional Password
  (with its own toggle; open lock when no password applies, closed lock when one is set)
  appear inline. While hosting, the pause menu shows one compact card with the labelled
  join addresses and the lock state.
- Mods is a top-level section of Settings.
- The user-visible word is "Mods", never "plugins".
- Data lists (the server list) are dense; action controls are large (48 dp minimum target).
- Maps are chosen in a visual picker with preview pictures and a preview page.
- Touch scrolling drags the content and never selects on scroll; a tap activates.
- Everything must feel light and instant: render optimistically, never block on the engine.

## 2. Versioning and compatibility

- Every message carries `"uib": 1`, the **major** protocol version. A peer receiving a
  different major replies `error` with code `version` and closes.
- The schema has a **minor** version (`"schema": "1.0"` in `hello`). Minor versions only
  add node types, props, hints, events or message types. Nothing is ever renamed or
  removed within a major.
- **Unknown node type**: render it as a generic row if it has a `title` or `label`
  (title, subtitle, its `actions` as buttons, its children below), otherwise render its
  children in order, otherwise skip it. Never fail the screen.
- **Unknown prop or hint**: ignore it.
- **Unknown message type** (`t`): ignore it. **Unknown event** (`ev`) at the engine:
  `error` code `unknown`.
- **Unknown icon name**: draw no icon (or the renderer's generic dot); never show the name.
- Renderers list what they support in `hello.caps.nodes` and `hello.caps.hints`; the menu
  VM may simplify a screen for a renderer that lacks a type.

## 3. Wire format

- **Encoding**: JSON (RFC 8259), UTF-8, no BOM. One message is one JSON object.
- **Framing on streams**: one message per line (`\n`-terminated, NDJSON). Encoders must
  escape every control character inside strings, so a message never contains a raw
  newline. In-process APIs pass one message per call, without the newline.
- **Size limits**: a message is at most 4 MiB; a snapshot of the core screens is normally
  under 64 KiB. A string value is at most 4096 bytes (8192 for `text` node `body`); a node
  id at most 128 bytes from `[A-Za-z0-9_.:/-]`; a node has at most 1024 children; the tree
  at most 8192 nodes and depth 32. Senders truncate strings at a UTF-8 boundary; receivers
  reject (error `too_large`) a message over 4 MiB.
- **Text**: plain UTF-8 for display. The engine removes Quake colour codes (`^1`,
  `^xRGB`) and maps the Quake private-use glyphs to their nearest characters before a
  string leaves the store. All user-visible text is already translated by the menu VM
  (`locale` in the handshake); renderers do not translate content, only their own chrome
  (e.g. an accessibility "Back" label).
- **Numbers**: JSON numbers; values bound to cvars travel as numbers when the value type
  is `number` or `bool` (`0`/`1` become `false`/`true`), as strings otherwise.

### 3.0 Typing rules (for generated clients, e.g. Rust `serde`)

- **No nulls on the wire.** An optional field is either present with a value of its one
  declared type, or absent. Absent means the documented default. Senders never write
  `null`; receivers treat a `null` like an absent field.
- **One type per field.** A field never changes JSON type. The only field whose type
  depends on context is a control's `value`, and it is fixed by the node's `type`
  (`switch`: bool, `slider`/`stepper`: number, `choice`/`field`/`colour`/`tabs`: string,
  `keybind`: array of strings). The `value` of a `value` event has the same type as the
  node it targets.
- **Tagged unions.** Messages are tagged by `t`, events by `ev`, nodes by `type` with their
  content in `props` (adjacent tagging: `#[serde(tag = "type", content = "props")]`).
  Every node always has `id`, `type`, `props` (possibly `{}`) and `children` (possibly
  `[]`). Recommended client decoding: read a node as `{id, type: String, props: Value,
  children, origin}` and decode `props` by `type`, so an unknown type is kept, not an error.
- **Enums** are lowercase ASCII strings (`camelCase` never appears in enum values).
  Unknown enum values mean the documented default.
- **Integers** (`id`, `re`, `rev`, `base`, `total`, `offset`, `limit`, `w`, `h`, dp sizes,
  `timeout`) are JSON integers in `0 ..= 2^53 - 1`, so they fit `i64`/`u64` and a double.
  Values of sliders and steppers and fractions (`progress`) are JSON numbers (`f64`).
- **Unknown fields** are ignored by every receiver (`#[serde(default)]`, no
  `deny_unknown_fields`). Field names are `camelCase` ASCII.
- **Table cells** are scalars: string, number or bool. Never objects or arrays.
- **Node ids** are stable: the same thing on screen keeps the same id across diffs and
  across engine restarts.

### 3.1 Envelope

```json
{"uib": 1, "t": "<message type>", "id": 17, "re": 12, ...}
```

| Field | Meaning |
| --- | --- |
| `uib` | protocol major, always `1` |
| `t` | message type |
| `id` | sender's message number, strictly increasing per connection, starting at 1 |
| `re` | on a reply: the `id` of the request it answers |

### 3.2 Messages, engine to renderer

| `t` | When | Body |
| --- | --- | --- |
| `hello` | reply to the renderer's `hello` | `schema`, `engine` (name/version string), `session` (random per engine process), `limits` (object, section 3), `features` (array of strings) |
| `snapshot` | after `hello`, on `sync`, and whenever a diff cannot be produced | `rev`, `root` (always `"app"`), `nodes` (array of nodes, parents before children) |
| `diff` | after the store changes, at most once per engine frame | `base` (the rev it applies to), `rev`, `set` (nodes, whole, parents before children), `del` (node ids) |
| `rows` | reply to `window` | `source`, `rev` (source revision), `total`, `offset`, `rows` |
| `source` | a data source changed (new servers, a ping, a filter) | `source`, `rev`, `total` |
| `image` | reply to `image` | `ref`, `mime`, `w`, `h`, and `path` or `data` (base64) |
| `ack` | reply to `sync`, `caps`, `ping` (as `pong`) when nothing else answers | — |
| `error` | a request or event was refused | `code`, `msg`, optional `re` |
| `bye` | engine is shutting down or closing the connection | `reason` |

### 3.3 Messages, renderer to engine

| `t` | Body | Reply |
| --- | --- | --- |
| `hello` | `schema`, `renderer` `{name, version}`, `caps` (section 7) | `hello` then `snapshot` |
| `caps` | `caps` (all fields again; size class changed, battery saver on ...) | none, or a diff if the menu VM adapts content |
| `sync` | optional `have` (the rev the renderer holds) | `snapshot` |
| `ev` | an event (section 6) | none, or `error`; effects arrive as diffs |
| `window` | `source`, `offset`, `limit` (≤ 200), optional `sort`, `desc`, `filter` | `rows` |
| `image` | `ref`, optional `w`, `h` (box to fit, px), `as` (`path` default or `bytes`) | `image` or `error` |
| `ping` | — | `pong` |
| `bye` | `reason` | none; the engine closes |

### 3.4 Snapshot and diff

The store is a **flat map of keyed nodes**. A snapshot lists every node; a diff lists every
node that changed since `base` (whole, not patched field by field) and every id removed.

Applying a diff:

1. If `diff.base` is not the rev the renderer holds, discard the diff and send `sync`.
2. Remove every id in `del`.
3. Insert or replace every node in `set`.
4. Set the held rev to `diff.rev`.

After step 3 every id in any `children` list exists. A node's identity is its `id`: a
renderer keeps the widget for an id across diffs and animates changes of its props, so a
progress bar slides and a value flips instead of the screen being rebuilt. Applying every
diff in order to the first snapshot gives exactly the latest snapshot (tested).

### 3.5 Ordering and threading

- The engine produces every message on its main (engine) thread, in order. Diffs are
  coalesced: at most one per frame, none in a frame where nothing changed.
- Replies may interleave with diffs. A `rows` reply carries the source's `rev`; a renderer
  that got a `source` notice with a higher rev re-requests its visible window.
- In-process consumers (the Rust core on Android) use the thread-safe queue functions of
  section 10 from any thread; the engine consumes their messages on its own thread.
- Renderers are optimistic: a switch flips when tapped and the engine's diff confirms it.
  If the diff carries a different value, the renderer shows the engine's value.

### 3.6 Connection lifecycle, reconnect and resync

1. Renderer connects, sends `hello`.
2. Engine replies `hello` and a `snapshot`, then diffs.
3. Lost connection: the renderer reconnects and starts again at 1. A `session` different
   from the last one means the engine restarted: drop every cached image path and row.
4. Any inconsistency (unknown child id, base mismatch, parse failure): send `sync`.

There is no per-renderer state in the engine besides the rev last sent and the open
image/window requests, so a reconnect is cheap.

## 4. Nodes

```json
{"id": "mp/servers", "type": "list", "children": [], "props": {"source": "servers"}, "origin": "core"}
```

| Field | Meaning |
| --- | --- |
| `id` | unique in the store; built from the parent's id and a key (`settings/video/fov`) |
| `type` | node type (4.2 to 4.6) |
| `children` | ordered ids; omitted when empty |
| `props` | type-specific content, state and hints; omitted when empty |
| `origin` | `"core"` (omitted) or `"mod:<mod id>"` on every node a mod described (section 8) |

### 4.1 Common props (any node)

| Prop | Type | Meaning |
| --- | --- | --- |
| `title` / `label` | string | main text |
| `subtitle` | string | secondary text |
| `icon` | icon name | section 5.1 |
| `hidden` | bool | not shown (kept in the tree so it can appear without a rebuild) |
| `enabled` | bool | default `true`; disabled controls are shown but do nothing |
| `help` | string | longer explanation (tooltip, info button, subtitle on touch) |
| `a11y` | string | accessibility label when it differs from the visible text |
| `actions` | action[] | secondary actions (section 4.7) |
| hints | — | section 5 |

### 4.2 Application and navigation

**`app`** (exactly one, id `app`, the root). Props:

| Prop | Type | Meaning |
| --- | --- | --- |
| `stack` | id[] | screens, bottom to top; the top one is shown |
| `modal` | id[] | sheets and dialogs over the stack, bottom to top |
| `busy` | object | absent when not busy. `{label, detail, progress (0..1), indeterminate (bool), cancel (action)}`: a global blocking state (connecting, loading) |
| `canBack` | bool | whether the `back` event does something (hardware back, Escape, swipe) |
| `title` | string | application title |
| `context` | `"menu"` / `"game"` / `"loading"` | menu at start, pause menu over a running match, a loading screen |

Children: every screen in `stack` and `modal`, then the `toast` nodes. Screens that leave the
stack are deleted from the store; renderers may keep their widgets for a back animation.

**`screen`** — one page. Props: `title`, `subtitle`, `presentation`
(`page` default, `sheet`, `dialog`, `fullscreen`), `kind` (`home`, `page`, `detail`,
`game`, `loading`, `status`), `primary` (action, the main thing to do), `actions`
(toolbar), `background` (image ref, renderers may blur/dim it), `dismissable` (sheets and
dialogs: tapping outside or back closes them), `header` (`{collapsible, compact, hidden}`:
the sticky header, section 4.10). Children: content and containers.

**`toast`** — `text`, `kind` (`info`, `success`, `warning`, `error`), `icon`, `action`
(one, e.g. Undo), `timeout` (seconds; renderer dismisses and sends `dismiss`).

### 4.3 Layout containers

| Type | Props | Children |
| --- | --- | --- |
| `section` | `title`, `subtitle`, `footer`, `collapsible`, `collapsed`, `link` (action: "See all"), `sticky` (bool: its header sticks, section 4.10) | anything |
| `toolbar` | — | sticky strip under the header: a search `field`, `tabs`, filter buttons. At most one, the screen's first child |
| `actionbar` | `align` | sticky bar at the bottom: buttons (Play, Join, Apply). At most one, the screen's last child |
| `row` | `wrap` (bool), `align` (`start`, `center`, `end`, `spread`) | small items side by side (buttons, chips, a control and its label) |
| `grid` | `minItem` (dp, a hint for the narrowest card, default 160), `maxColumns`, `scroll` (`vertical` default, `horizontal` for a carousel) | cards |
| `tabs` | `style` (`tabs`, `segmented`), `value` (selected pane's key), `bind` | `pane` nodes |
| `pane` | `key`, `label`, `icon`, `badge` | content of one tab |
| `split` | `detailOpen` (bool) | two children: list, then detail. Wide screens show both; narrow ones show the detail as a pushed page when `detailOpen` |
| `form` | `submit` (action), `error` (form-level message) | controls; each control may carry `error` |

### 4.4 Content

**`card`** — `title`, `subtitle`, `image` (image ref), `aspect`, `badges` (badge[]),
`meta` (chip[]), `progress` (0..1; absent: no bar), `primary` (action: tapping the card),
`actions` (secondary), `selected` (bool), `featured`.
A badge is `{text, icon, tone}`, a chip `{icon, text}`; `tone` is one of `neutral`,
`accent`, `positive`, `warning`, `danger`.

**`hero`** — the big Play/continue card: `title`, `subtitle`, `image`, `badges`, `meta`,
`primary`, `actions`. At most one per screen, first.

**`list`** — a virtualised list or data table over a data source.

| Prop | Type | Meaning |
| --- | --- | --- |
| `source` | string | data source name (section 4.8) |
| `columns` | column[] | `{key, label, type, sortable, width, align, priority}` |
| `sort`, `desc` | string, bool | current sort column and direction |
| `filter` | string | current search text (also sent in `window`) |
| `searchable` | bool | show a search field |
| `selection` | `none`, `single`, `multi` | |
| `selected` | string[] | selected row keys |
| `rowPrimary` | action | what tapping a row does (event `row`, kind `activate`) |
| `rowActions` | action[] | per-row actions (swipe, context menu, trailing buttons) |
| `state` | `ready`, `loading`, `error` | |
| `empty` | `{title, text, icon, action}` | shown when `total` is 0 and `state` is ready |
| `error` | `{title, text, action}` | shown when `state` is error |
| `count` | int | row count at the last `source` notice (also `total` in `rows`) |
| `display` | `table`, `rows`, `cards` | `table` is dense, `cards` is a grid of preview cards using each row's `image` |
| `rowHeight` | `compact`, `comfortable`, `card`, `estimated` | fixed row height class (section 4.10); `estimated`: variable rows, `estimate` gives a dp guess |
| `estimate` | int (dp) | estimated row height for `estimated` |
| `minItem` | int (dp) | `display: cards`: narrowest card |
| `aspectItem` | aspect | `display: cards`: the cards' image aspect |

Column `type`: `text`, `number`, `ping` (ms, renderers colour by quality), `players`
(cell is a number of players; the row's `max` and `bots` cells complete it as "7/16"), `bool`, `icon` (cell is an icon name),
`lock` (bool, padlock), `map` (map name; renderers may show its preview), `mode` (game
type id), `size`, `date`. `width`: `xs`, `s`, `m`, `l`, `fill`. `priority`: 1 (always
shown), 2, 3 (dropped first on narrow screens). `align`: `start`, `end`.

Rows (in `rows` messages): `{key, cells: {<column key>: value}, image, badges, tone,
disabled}`. Cells may also carry keys that are not columns (e.g. `address`, `modeName`);
renderers ignore them unless they know them.

**`preview`** — a detail page header for a map, a mode or a mod: `title`, `subtitle`,
`image`, `aspect`, `body` (description), `attrs` (`[{label, value, icon}]`), `badges`,
`primary`, `actions`.

**`text`** — `body`, `style` (`body`, `caption`, `title`, `mono`, `note`, `warning`),
`selectable`.
**`image`** — `ref`, `aspect`, `fit` (`cover`, `contain`), `alt`.
**`divider`** — nothing.
**`status`** — a whole-page state: `icon`, `title`, `body`, `progress` (0..1),
`indeterminate`, `primary`, `actions`, `tone` (empty pages, errors, "connecting", "nothing here yet").
**`progress`** — `label`, `value` (0..1), `indeterminate` (bool; `value` is then
ignored), `detail` ("12.3 MB of 40 MB · 2.1 MB/s · 13 s left"), `cancel` (action).

### 4.5 Controls

Every control has `label`, optional `help`, `bind` (section 4.9), `value` (current value),
`error` (validation message), `apply` and `enabled`.

`apply`: `immediate` (default), `video` (applies after a renderer restart, which the
engine does when the screen is left or on `apply` action), `sound` (sound restart),
`reconnect` (applies to the next match), `restart` (needs a game restart). Renderers show a
small note for anything but `immediate`.

| Type | Value | Props |
| --- | --- | --- |
| `switch` | bool | `style` (`switch` default, `checkbox` for filter rows and form options) |
| `slider` | number | `min`, `max`, `step`, `format` (`int`, `float1`, `float2`, `percent` (value 0..1 shown ×100), `ms`, `dp`, `db`, `x` (multiplier), `deg`), `unit` (text after the number), `marks` (`[{value, label}]`) |
| `stepper` | number | `min`, `max`, `step`, `format` |
| `choice` | string | `options` (`[{value, label, icon, image, help}]`), `style` (`auto`, `segmented`, `dropdown`, `radio`, `cards`) |
| `field` | string | `kind` (`text`, `name`, `number`, `password`, `url`, `search`, `address`), `placeholder`, `maxLength`, `private`, `hasValue`, `submit` (action) |
| `button` | — | `action` (action), `style` (`primary`, `secondary`, `tonal`, `destructive`, `link`) |
| `colour` | string `#rrggbb` | `palette` (`["#rrggbb", ...]`; when present only these may be picked), `preview` (image ref) |
| `keybind` | string[] (key names) | `command`, `max` (keys per command, default 2) |
| `progress` | see 4.4 | |

Private values (`field` with `private: true`, e.g. passwords) are write-only: the
description never carries the value, only `hasValue`; the renderer sends `value` or
`submit` and clears its own copy.

### 4.6 Value events and validation

A control change sends `ev: "value"` with the new value. The engine checks it against the
bind's type and range (and `options` / `palette`), stores it, and the control's next diff
carries the stored value; a refused value comes back as the old value with `error` set.

### 4.7 Actions

```json
{"id": "join", "label": "Join", "icon": "play", "style": "primary", "enabled": true,
 "confirm": {"title": "Leave the match?", "body": "...", "ok": "Leave", "destructive": true}}
```

| Field | Meaning |
| --- | --- |
| `id` | sent back in the `action` event |
| `label`, `icon`, `style`, `enabled` | as for `button` |
| `kind` | `normal` (default), `back` (same as the back event), `link`, `file`, `share`, `copy` |
| `confirm` | the renderer asks first, natively, and sends the event only on OK |
| `url` | `kind: link` only: an `https:` (or `http:`) URL. The renderer shows the host and asks before opening it in the system browser |
| `accept` | `kind: file` only: file kinds (`"pk3"`, `"zip"`, `"godnotic-mod"`) for the system picker; the chosen file comes back as `payload.path` (a readable path or a content URI) |
| `text` | `kind: share` / `copy`: the text the renderer shares or copies itself (no event needed; it may still send one) |

Nothing else in a description can open a URL or a file.

### 4.8 Data sources

| Source | Rows | Notes |
| --- | --- | --- |
| `servers` | every server the engine knows: Internet, LAN, VPN | cells: `name`, `map`, `mode`, `players`, `max`, `bots`, `ping`, `lock`, `fav`, `address`, `net` (`internet`, `lan`, `vpn`), `version` |
| `servers.lan` | LAN and VPN rows only | as above (the Servers screen keeps source `servers` and its LAN checkbox filters it engine-side) |
| `servers.fav` | favourites | as above |
| `players:<address>` | players on one server (from its status reply) | `name`, `score`, `ping`, `team` |
| `maps` | installed maps | `name`, `title`, `author`, `modes` (space-separated mode ids), `players` (recommended, string), `size` (`small`, `medium`, `large`); row `image` is the preview ref |
| `maps:<mode>` | maps that support a game type | as above |
| `modes` | game types | `id`, `name`, `help`, `team` (bool); row `image` is the mode artwork |
| `mods.client`, `mods.server` | installed mods | `id`, `name`, `version`, `enabled`, `state` (`ok`, `error`, `stopped`), `tier` (permission tier as the engine reports it) |
| `recent` | recent matches | `map`, `mode`, `when`, `result`, `address` (empty for local) |

Row `key`s are stable for a row across revisions (a server's address, a map's name).

### 4.9 Bindings

Every control binds to one named, typed value: `"bind": "<scheme>:<name>"`.

| Scheme | Lives in | Example |
| --- | --- | --- |
| `cvar:` | an engine cvar; the engine reads and writes it directly | `cvar:sensitivity` |
| `vm:` | a view-model value owned by the menu VM (filters, form fields, selections) | `vm:mp.filter.mode` |
| `mod:<id>.` | a value of a mod page, owned by that mod | `mod:radar.range` |

The engine accepts a `value` event only for a bind that is present in the current tree
(a renderer cannot set arbitrary cvars), and only of the declared type and range.
When a bound value changes from elsewhere (console, config, another renderer), the engine
pushes a diff of the controls bound to it.

### 4.10 Scrolling, virtualisation and sticky elements

All three renderers must behave the same way here.

**Screen anatomy.** A page is, top to bottom: the **header** (title, back, the screen's
`actions`), then the screen's `toolbar` child if any, or the tab strip of a `tabs` node that
is a direct child of the screen; then **one scrolling region** with everything else; then
the **action bar**: the `actionbar` child, or the screen's `primary` action when there is
no `actionbar`. Header, toolbar/tab strip and action bar are **sticky**: they never scroll
away, sit on their own surface (or a subtle scrim) so content passing under them stays
readable, and stay inside the safe insets. `header.collapsible`: after the content scrolls
by about one header height, the header shrinks to a compact single line (large title to
small title); it never disappears. `header.compact`: always compact. On very short screens
(height class `compact`) a collapsible header starts compact. The scroll indicator is drawn
inside the scrolling region, between the sticky top and bottom parts, never over them.

A `list` with `display: table` keeps its column header row sticky at the top of the
scrolling region while its rows scroll.

**Sticky section headers.** A `section` with `sticky: true` inside the scrolling region:
its header stays at the top of the region while its content scrolls under it, and the next
sticky section's header pushes it up and away.

**Virtual scroll.** A `list` is always virtualised; so is the scrolling region of any
screen: a renderer lays out and draws only what is visible plus a small overscan (about
half a screen), recycling row and card widgets.

- Content height: `count` × the row height for fixed classes (`compact`, `comfortable`,
  `card`), or for `estimated` rows `count` × `estimate`, corrected as rows are measured.
  Corrections above the viewport are compensated in the scroll offset so nothing visible
  jumps.
- Data: rows come from `window` requests. Request the visible range plus look-ahead
  before it is needed (when the viewport gets within one screen of an unloaded range);
  draw **placeholder rows** (shimmer or skeleton, same height) until the rows arrive. Keep
  a bounded row cache (e.g. 1,000 rows) keyed by row key.
- **Anchor on keys, not offsets.** Remember the first visible row's key and its offset
  from the top of the viewport. After a `source` change, a re-sort, a filter change or a
  diff that inserts or removes nodes above the viewport, scroll so that key stays at the
  same place (if it is gone, keep the offset). Returning to a screen restores its anchor
  (renderers keep it per screen id and list id for the session).
- Sorting and filtering re-window: send `window` with the new sort/filter for the
  visible range; keep the old rows visible (dimmed is fine) until the reply comes.
- Images in rows and cards are requested only when within the overscan, and released
  (cache eviction allowed) when far away.
- **Touch**: dragging scrolls the content and never selects; a fling continues with
  friction; a tap activates. A touch that stops a fling (a press during a fling) only
  stops it and never activates a row. A press becomes a tap only if the finger moves less
  than the platform slop (about 8-10 dp) and the content was not moving.
- Target: 60 fps flinging through 2,000 servers and a 200-card map grid.

| | Android | GTK 4 / libadwaita | QuakeC |
| --- | --- | --- | --- |
| scrolling region | `RecyclerView` (one per screen, `ConcatAdapter` for mixed sections) | `GtkScrolledWindow` + `GtkListView` | own (touch_uib_render.qc) |
| `list` table / rows | `RecyclerView` + `ListAdapter`, `DiffUtil` on row keys | `GtkColumnView` / `GtkListView` over a `GListModel` that fetches windows | own, recycled row slots |
| `list` cards, `grid` | `GridLayoutManager` with span from `minItem` | `GtkGridView` | own grid |
| sticky section headers | item decoration or sticky-header layout manager | `GtkListHeaderFactory` sections | own |
| header, toolbar, tab strip | `CoordinatorLayout` + `AppBarLayout` (collapsing) | `AdwToolbarView` top bars (`AdwHeaderBar`, `AdwViewSwitcher`) | own |
| action bar | bottom bar in the `CoordinatorLayout` | `AdwToolbarView` bottom bar | own |

## 5. Hints

Hints are props that suggest presentation. A renderer that does not understand one ignores it.

| Hint | Values | Meaning |
| --- | --- | --- |
| `density` | `comfortable` (default), `compact` | compact rows for dense data (server list, mods list); never applies to action controls, which stay ≥ 48 dp |
| `emphasis` | `low`, `normal`, `high` | visual weight |
| `accent` | `blue`, `ember`, `green`, `red`, `neutral` | tint (Godnotic palette: blue for information/focus, ember for primary action) |
| `featured` | bool | larger card, first in a grid, parallax/shine allowed |
| `aspect` | `"16:9"`, `"4:3"`, `"1:1"`, `"3:4"`, `"21:9"` | image aspect ratio |
| `importance` | 1, 2, 3 | adaptive layouts drop 3 first, then 2 |
| `span` | `full` | in a grid, take the whole row |
| `tone` | `neutral`, `accent`, `positive`, `warning`, `danger` | badges, status, rows |
| `live` | bool | value changes often (ping, progress): no announcement for accessibility, no layout animation |

### 5.1 Icons

Icon names are semantic. Each renderer maps them to its own set; the table gives the
reference mapping.

| Name | Material Symbols | Adwaita symbolic |
| --- | --- | --- |
| `play` | `play_arrow` | `media-playback-start-symbolic` |
| `resume` | `play_circle` | `media-playback-start-symbolic` |
| `pause` | `pause` | `media-playback-pause-symbolic` |
| `singleplayer` | `person` | `avatar-default-symbolic` |
| `multiplayer` | `groups` | `system-users-symbolic` |
| `settings` | `settings` | `emblem-system-symbolic` |
| `credits` | `info` | `help-about-symbolic` |
| `quit` | `power_settings_new` | `system-shutdown-symbolic` |
| `back` | `arrow_back` | `go-previous-symbolic` |
| `close` | `close` | `window-close-symbolic` |
| `search` | `search` | `system-search-symbolic` |
| `filter` | `filter_list` | `funnel-symbolic` |
| `sort` | `sort` | `view-sort-descending-symbolic` |
| `refresh` | `refresh` | `view-refresh-symbolic` |
| `lock` | `lock` | `changes-prevent-symbolic` |
| `unlock` | `lock_open` | `changes-allow-symbolic` |
| `star` / `star_filled` | `star` / `star` (filled) | `non-starred-symbolic` / `starred-symbolic` |
| `wifi` | `wifi` | `network-wireless-symbolic` |
| `vpn` | `vpn_lock` | `network-vpn-symbolic` |
| `lan` | `lan` | `network-wired-symbolic` |
| `internet` | `public` | `web-browser-symbolic` |
| `host` | `cell_tower` | `network-server-symbolic` |
| `server` | `dns` | `network-server-symbolic` |
| `map` | `map` | `mark-location-symbolic` |
| `mode` | `sports_esports` | `input-gaming-symbolic` |
| `bot` | `smart_toy` | `computer-symbolic` |
| `player` | `person` | `avatar-default-symbolic` |
| `players` | `group` | `system-users-symbolic` |
| `ping` | `network_ping` | `network-cellular-signal-good-symbolic` |
| `add` / `remove` | `add` / `remove` | `list-add-symbolic` / `list-remove-symbolic` |
| `edit` | `edit` | `document-edit-symbolic` |
| `delete` | `delete` | `user-trash-symbolic` |
| `download` / `upload` | `download` / `upload` | `folder-download-symbolic` / `send-to-symbolic` |
| `update` | `system_update` | `software-update-available-symbolic` |
| `info` / `warning` / `error` / `check` | `info` / `warning` / `error` / `check` | `dialog-information-symbolic` / `dialog-warning-symbolic` / `dialog-error-symbolic` / `object-select-symbolic` |
| `link` | `link` | `insert-link-symbolic` |
| `share` | `share` | `send-to-symbolic` |
| `copy` | `content_copy` | `edit-copy-symbolic` |
| `mods` | `extension` | `application-x-addon-symbolic` |
| `video` | `display_settings` | `video-display-symbolic` |
| `audio` | `volume_up` | `audio-volume-high-symbolic` |
| `game` | `sports_esports` | `input-gaming-symbolic` |
| `input` | `keyboard` | `input-keyboard-symbolic` |
| `touch` | `touch_app` | `input-touchscreen-symbolic` |
| `esp` | `visibility` | `view-reveal-symbolic` |
| `privacy` | `shield` | `security-high-symbolic` |
| `data` | `storage` | `drive-harddisk-symbolic` |
| `menu` | `menu` | `open-menu-symbolic` |
| `mouse` / `gamepad` | `mouse` / `stadia_controller` | `input-mouse-symbolic` / `input-gaming-symbolic` |
| `colour` | `palette` | `color-select-symbolic` |
| `crosshair` | `my_location` | `find-location-symbolic` |
| `aim` | `center_focus_strong` | `find-location-symbolic` |
| `hud` | `dashboard` | `view-grid-symbolic` |
| `timer` | `timer` | `alarm-symbolic` |
| `trophy` | `emoji_events` | `starred-symbolic` |
| `flag` | `flag` | `flag-outline-thin-symbolic` |
| `chevron` | `chevron_right` | `go-next-symbolic` |
| `more` | `more_vert` | `view-more-symbolic` |
| `folder` / `file` | `folder` / `draft` | `folder-symbolic` / `text-x-generic-symbolic` |
| `help` | `help` | `help-browser-symbolic` |
| `disconnect` | `logout` | `system-log-out-symbolic` |
| `keyboard` | `keyboard` | `input-keyboard-symbolic` |
| `language` | `translate` | `preferences-desktop-locale-symbolic` |

### 5.2 Image refs

| Ref | Means |
| --- | --- |
| `pk3:<path>` | a file in the engine's virtual filesystem (pk3, pk3dir, dpk, zstd entries). The extension may be omitted: the engine tries `.jpg`, `.png`, `.tga`, `.webp` (e.g. `pk3:maps/stormkeep`) |
| `asset:<name>` | a named built-in asset: `banner`, `logo`, `background`, `mode:<id>`, `placeholder:map` |
| `mod:<id>/<path>` | a file inside a mod package |

Renderers never resolve refs themselves except the QuakeC renderer (which draws `pk3:`
paths directly). They send `image` with the box they will draw (`w`, `h` in physical
pixels); the engine returns a PNG thumbnail no larger than that box (aspect kept), cached in
the user directory (`uib-cache/`), as a `path` (default; the file stays valid for the
session) or as base64 `data`. A missing image is an `error` with code `not_found`;
renderers then show their placeholder.

## 6. Events (renderer to engine)

All events are messages `{"uib":1, "t":"ev", "id":n, "ev":"<kind>", ...}`.

| `ev` | Fields | Meaning |
| --- | --- | --- |
| `action` | `node`, `action` (action id), optional `payload` (object) | an action was invoked: a button, a card's primary, a toolbar action, a confirmed confirm |
| `value` | `node`, `value` | a control's value changed (switch, slider on release or throttled to 10 Hz while dragging, choice, colour, keybind, field on each edit if the field has no `submit`) |
| `submit` | `node`, `value` | a field was submitted (Enter, Done, the field's `submit` action) |
| `row` | `node` (the list), `row` (row key), `kind` (`activate`, `select`, `action`), `action` (for kind action) | a list row was tapped, selected, or had a row action invoked |
| `shown` / `hidden` | `node` (screen id) | the screen became visible / was covered or left; the engine pauses work for hidden screens (server pings, previews) |
| `back` | — | system back, Escape, swipe back |
| `dismiss` | `node` (sheet, dialog or toast) | closed by tapping outside, swiping down, or a toast timing out |
| `visible` | `node` (the list), `offset`, `limit` | the visible window of a list (lets the engine prioritise pings for those rows) |

## 7. Capability handshake

`hello` from the renderer:

```json
{"uib": 1, "t": "hello", "id": 1, "schema": "1.0",
 "renderer": {"name": "godnotic-android", "version": "0.3.0"},
 "caps": {
   "nodes": ["app","screen","toast","section","row","grid","tabs","pane","split","form",
             "card","hero","list","preview","text","image","divider","status","progress",
             "switch","slider","stepper","choice","field","button","colour","keybind"],
   "hints": ["density","emphasis","accent","featured","aspect","importance","span","tone","live"],
   "width": 844, "height": 390, "dpi": 420, "widthClass": "expanded", "heightClass": "compact",
   "input": ["touch"], "locale": "en", "reducedMotion": false,
   "powerSave": false, "thermal": "nominal",
   "insets": {"top": 0, "right": 32, "bottom": 0, "left": 32}
 }}
```

| Field | Meaning |
| --- | --- |
| `width`, `height` | window size in dp |
| `widthClass` | `compact` (< 600 dp), `medium` (600 to 839), `expanded` (≥ 840) |
| `heightClass` | `compact` (< 480 dp), `medium` (480 to 899), `expanded` (≥ 900) |
| `input` | any of `touch`, `mouse`, `keyboard`, `gamepad` |
| `locale` | BCP 47; the menu VM switches `prvm_language` only if the user has not chosen one |
| `reducedMotion` | the system asks for less motion |
| `powerSave`, `thermal` | the engine lowers background work (server pings, previews, warm-up) |
| `insets` | safe-area insets in dp |

The engine exposes the latest caps to the menu VM, which may adapt content (fewer columns
on a compact width, no parallax hint with reduced motion). Send `caps` again whenever any
field changes.

## 8. Mods

Mods add settings and pages through the same schema (Lua API, `touch_lua*`):

- A mod page is a subtree registered under `mods/<mod id>/<page key>`; every node in it
  carries `"origin": "mod:<mod id>"`. Renderers label such pages ("From the mod
  <name>") and must not let them look like core settings.
- Settings has a top-level **Mods** section listing installed client and server mods and,
  under each mod, the pages it contributed.
- Bounds per mod: 256 nodes, depth 8, strings of 512 bytes, 16 image refs (`mod:<id>/`
  only), 32 KiB of JSON. Allowed node types: `section`, `row`, `text`, `divider`, `image`,
  `card`, `switch`, `slider`, `stepper`, `choice`, `field`, `button`, `colour`, `keybind`,
  `progress`, `status`. Allowed binds: `mod:<own id>.*` only. Allowed actions: `normal`
  and `link` (https only; the renderer always confirms). Anything else is dropped and
  logged; a description over a bound is refused whole.
- Events on mod nodes go to that mod only.

## 9. Transports

### 9.1 In process (Android JNI, QuakeC renderer)

C API, section 10. One connection per renderer; messages are the same JSON.

### 9.2 Linux local stream

The engine runs as the UI back end when started with `-uibridge <spec>`:

| Spec | Meaning |
| --- | --- |
| `fd:<n>` | use inherited file descriptor `n`, a bidirectional `AF_UNIX` `SOCK_STREAM` socket (one end of `socketpair`). The usual case: the app creates the pair and spawns the engine |
| `stdio` | read stdin, write stdout (console output then goes to stderr only) |
| `listen[:<path>]` | listen on a Unix socket, default `$XDG_RUNTIME_DIR/godnotic/uibridge.sock`, created with mode 0600 in a 0700 directory; one client at a time |

Framing is NDJSON (section 3). In back-end mode:

- the menu VM runs and keeps the description current, but the menu is not drawn;
- the game window stays hidden until a match starts (`app.context` becomes `game`) and
  hides again when the player returns to the menu;
- with `fd:` or `stdio`, the engine quits cleanly (`bye`, config saved) when the front end
  closes its end; with `listen`, it waits 10 s for a new client, then quits;
- the engine answers the first `hello` as soon as the menu VM has built Home (measured
  start time in `docs/UI_BRIDGE.md` section 12).

### 9.3 Fixture player

`scripts/uib-replay.py` plays fixtures over the same transport without an engine, so a
renderer can be developed and tested alone:

```sh
scripts/uib-replay.py --listen /tmp/uib.sock tests/uibridge/fixtures/seq-server-list.json
scripts/uib-replay.py --stdio tests/uibridge/fixtures/home.json   # spawned by the app
scripts/uib-replay.py --fd 3 ...
```

It answers `hello` with a `hello` and the fixture's snapshot, then sends the sequence's
diffs (with their delays), answers `window` from the fixture's rows and `image` with
the path of a generated placeholder PNG of the requested size, applies `value` events like
the engine (a confirming diff), and prints every message it receives.

## 10. Engine C ABI (`touch_uib.h`)

A plain C ABI with opaque handles and explicit lengths, so the Rust core (`loader/crates/core`)
can bind it with `bindgen` or by hand. Strings are UTF-8 with an explicit length and are
not required to be NUL-terminated on input; buffers returned by the engine are
NUL-terminated as a convenience.

```c
typedef struct touchuib_conn_s touchuib_conn_t;   /* opaque */

/* --- any thread, once the engine has started (TouchUIB_Available() == 1) --- */
int  TouchUIB_Available(void);                     /* 1 when the bridge is up */
touchuib_conn_t *TouchUIB_Open(const char *name, size_t name_len);   /* NULL on failure */
/* Queue one renderer message (hello, ev, window, image, sync, caps, ping, bye).
   The engine copies it. Returns 0, or -1 if the connection is closed,
   -2 if the inbound queue is full (256 messages or 8 MiB; retry later), -3 if too large. */
int  TouchUIB_Post(touchuib_conn_t *c, const char *json, size_t len);
/* Take the next engine message, or NULL if none. The buffer is owned by the caller
   and must be released with TouchUIB_Free (from any thread). *len excludes the NUL. */
char *TouchUIB_Take(touchuib_conn_t *c, size_t *len);
void  TouchUIB_Free(char *buf);
/* Wake-up without polling:
   - a file descriptor (eventfd on Linux/Android, -1 elsewhere) that becomes readable
     when messages are waiting; read 8 bytes to reset it, then Take until NULL;
   - or a callback run on the engine thread right after messages were queued. It must
     not block and may only call TouchUIB_Take/Free or signal its own thread. */
int  TouchUIB_NotifyFd(touchuib_conn_t *c);
void TouchUIB_SetNotify(touchuib_conn_t *c, void (*fn)(void *ctx), void *ctx);
/* Close and free the handle. After this the handle is invalid; queued messages are dropped. */
void TouchUIB_Close(touchuib_conn_t *c);
```

Threading: `Open`, `Post`, `Take`, `Free`, `NotifyFd`, `SetNotify` and `Close` are
thread-safe and never block on the engine (they take a short internal lock). The engine
processes posted messages at the start of its next frame and queues its replies and diffs
at the end of that frame. A connection whose outbound queue exceeds 16 MiB (the renderer
stopped taking) is reset: the queue is dropped and the next message is a fresh `snapshot`.
Outbound order is the order of section 3.5. When the engine shuts down, every connection
gets `bye` and then `Post` returns -1.

The JNI bridge (`touch_frontend.c`) and the Linux stream transport are both clients of
this same queue API; the stream transport runs on the engine thread with non-blocking I/O.

Engine-thread-only helpers (tests, the QuakeC renderer, debug commands):

```c
char *TouchUIB_SnapshotJSON(size_t *len);              /* free with TouchUIB_Free */
char *TouchUIB_DiffJSON(int64_t since_rev, size_t *len); /* NULL if since_rev is too old */
int64_t TouchUIB_Rev(void);
```

Mods (the Lua API calls these on the engine thread):

```c
/* Register or replace a page; 0 ok, < 0 refused (err gets a NUL-terminated reason). */
int  TouchUIB_ModSetPage(const char *mod_id, const char *page_key,
                         const char *json, size_t len, char *err, size_t errlen);
void TouchUIB_ModRemove(const char *mod_id, const char *page_key /* NULL: all pages */);
/* Update one bound value of a mod (JSON scalar or string array). */
int  TouchUIB_ModSetValue(const char *mod_id, const char *key, const char *json_value, size_t len);
/* Events on mod nodes: the full `ev` message as JSON, origin already checked. */
typedef void (*touchuib_mod_event_fn)(const char *mod_id, const char *ev_json, size_t len, void *ctx);
void TouchUIB_ModSetEventHandler(touchuib_mod_event_fn fn, void *ctx);
```

A mod page JSON is `{"title": ..., "nodes": [ ... ]}` with nodes in the wire shape whose
ids are relative to the page (`"range"`, `"general/range"`); the store prefixes them.

Engine-side cost: with no connection and `menu_touch_uib 0`, nothing runs. With a
connection, the engine does work only in frames where a message arrived, the menu VM
rebuilt a screen, a bound cvar changed (bound cvars are compared at most 10 times a
second) or a data source changed.

Console commands (tests, debugging):

| Command | Does |
| --- | --- |
| `uib_dump [file]` | writes the current snapshot (to the console or `data/<file>`) |
| `uib_event <json>` | delivers an `ev` message as if a renderer sent it |
| `uib_send <json>` | delivers any renderer message on a loopback client and prints the replies |
| `uib_stats` | node count, rev, snapshot size, last diff size, clients |

## 10a. Writing a client

For the Rust core (and anyone else):

1. **Connect** (stream: connect or inherit the fd; Android: `TouchUIB_Open`) and send
   `hello` with your caps. Wait for `hello` (check `uib`, note `session`) and the
   `snapshot`. Until then show your own skeleton; the engine answers in well under a second.
2. **Hold a store**: `HashMap<String, Node>` plus `rev`. Build widgets from `app`
   (`props.stack` top, then `props.modal`), keyed by node id.
3. **Apply diffs** exactly as 3.4. On any mismatch send `sync` and replace the store with
   the next `snapshot`, reusing widgets whose ids survive.
4. **Lists**: for a `list` node, request `window` for the rows you are about to show
   (first screenful plus one page of look-ahead, `limit` ≤ 200), keyed by `source` plus
   sort, desc and filter. Cache rows by `key`. On a `source` message with a higher rev,
   re-request the visible window only. Send `visible` when the visible range settles, and
   change `sort`/`filter` by sending `window` with the new values (the engine also echoes
   them into the list's props through the bound values).
5. **Images**: send `image` with the ref and the pixel box you draw; cache the reply by
   `(session, ref, w, h)`. Show a placeholder (shimmer) until it arrives; on `error`
   keep the placeholder.
6. **Input**: send `ev` messages. Be optimistic (flip the switch now), then let the
   engine's diff win. Throttle slider `value` events to 10 Hz while dragging and send the
   final value on release.
7. **Navigation**: back gesture/button sends `back`; never pop screens locally. Screen
   visibility changes send `shown`/`hidden`.
8. **Threading**: one reader (stream: a reader thread or the main loop's fd source;
   Android: wake on `TouchUIB_NotifyFd` or the notify callback, then `Take` until NULL),
   apply on the UI thread. Never call `Post` while holding a UI lock the notify path needs.
9. **Reconnect**: on EOF or `bye`, reconnect (stream) or reopen (Android) with backoff
   (100 ms, doubling to 2 s) and start again at 1; drop cached images if `session` changed.
10. **Test** against `scripts/uib-replay.py` and the fixtures, which cover every screen
    and the diff sequences in section 13.

## 11. QuakeC side

- `touch_uib_api.qh`: the builtins (store writes, reads for the QuakeC renderer).
- `touch_uib_build.qc`: the builder (`UIB_Screen`, `UIB_Begin`/`UIB_End`, `UIB_Card`,
  `UIB_Switch` ...). A screen is a function that describes its nodes; it is called again
  when anything it reads changes, and the engine keeps only the differences.
- `touch_uib_screens*.qc`: the screens (Home, Play, Multiplayer, Settings, pause,
  loading, Credits, dialogs).
- `touch_uib_render.qc`: the QuakeC renderer (web, and the fallback everywhere).
- `menu_touch_uib 0/1`: 1 draws the menu from these descriptions with the QuakeC
  renderer; 0 keeps the current touch menu. A native renderer attached hides both.

## 12. Measurements

Filled in from the engine as it lands (backend start time, snapshot and diff sizes).

## 13. Fixtures

`tests/uibridge/fixtures/`:

- `<screen>.json`: a snapshot sequence file for one screen (`{"snapshot": ..., "rows": {...}}`).
- `seq-*.json`: a snapshot followed by diffs (`{"snapshot": ..., "steps": [{"delay": s, "msg": diff}], "rows": {...}}`).
- `rows` maps a source name to the full row array; the replay script windows and sorts it.

Every fixture validates against `docs/ui-bridge.schema.json`. Fixtures marked
`"generated": "seed"` were written by hand before the engine produced them; `scripts/test-uibridge.sh --check` regenerates the
others from the engine and fails on drift (ignoring `session`, `id`, pings and timestamps).
