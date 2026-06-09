# Mongui


> A keyboard-driven MongoDB terminal client written in C++17.
> By **RD-Freelance** · MIT licensed · [LICENSE](LICENSE)

Browse databases, run queries and aggregations, manage indexes, paginate
through results, and drive a mongosh-style shell — all in a single binary
that adapts to whatever terminal size you give it.

```
 mongui   1 users   2 orders   3 events                              QUERY

┌ databases ────────┐┌ collections ─────────────┐┌ documents ────────────────
│ › admin           ││ search ▸ user            ││ filter  ▸ { age: { $gt: 21 } }
│   config          ││ › users                  ││ project ▸ (optional)
│   local           ││   user_events            ││ sort    ▸ { _id: -1 }
│   myapp           ││   user_sessions          ││ skip    ▸    limit ▸ 50
└───────────────────┘└──────────────────────────┘│ ─────────────────────────
                                                  │ ┃ {
                                                  │ ┃   "_id": ObjectId("…"),
                                                  │ ┃   "name": "alice",   ← selected
                                                  │ ┃   "age": 30
                                                  │ ┃ }

 ^e query  ^a agg  ^s shell   tab field  y copy  c clone  e edit  d delete   ^t/^b/^w tabs  ^q quit
                                                                         3 / 1,204 docs
```

---

## Highlights

- **Three modes** — Query, Aggregation, Shell — toggled with `^E` / `^A` / `^S`
- **Workspace tabs** — up to 5 independent collection + documents views, each with its own query, pipeline, shell, and mode (`^T` new · `^B` next · `^W` close)
- **Per-document actions** — select a document in the pane and **copy** (`y`), **clone** (`c`, drops `_id`), **edit inline** (`e`), or **delete** (`d`)
- **Inline document editor** — edit a document in place as JSON and save with `^S` (`replaceOne` by `_id`); no separate window
- **Non-interactive import** — `mongui --import --db … --collection … --filepath …` loads a JSON array, NDJSON, or a single document straight into a collection
- **Server-side pagination** — `^N` / `^P` through find results *and* aggregation previews
- **Responsive layout** — three-pane on wide terminals, vertically stacked on narrow ones
- **Full BSON output** — long values wrap onto continuation rows, nothing is hidden
- **Type-aware display** — `NumberLong("…")`, `NumberDecimal("…")`, `ObjectId("…")`, `ISODate("…")`
- **Syntax highlighting** — keys, strings, numbers, booleans, `$operators`, helpers
- **Pooled libmongoc** client, ready for future parallel work
- **Single-write rendering** — atomic frames, no flicker even over SSH
- **Index management** — `createIndex`, `dropIndex`, `getIndexes` in the shell
- **Smart pipeline editor** — auto-paired brackets, smart indent, autocomplete on `$ops`, `^L` reformat
- **JSON / CSV export** to `~/Downloads`, capped at 100 000 docs per file

---

## Build

Requires a C++17 compiler and the MongoDB C driver (mongo-c-driver 2.x or
libmongoc-1.0). The provided installers handle every dependency for you.

### One-command install

**Linux / macOS** — installs the toolchain + driver, builds, and puts `mongui`
on your PATH:

```bash
git clone https://github.com/RD-Freelance/mongui && cd mongui
./scripts/install.sh
mongui
```

The script auto-detects your package manager (apt, dnf/yum, pacman, zypper,
apk, or Homebrew) and builds the MongoDB C driver from source if your distro
has no package for it. Override the location with
`PREFIX=$HOME/.local ./scripts/install.sh`.

**Windows 10/11** (PowerShell, in the repo root) — installs Git, CMake, the
MSVC C++ build tools (via winget when missing), fetches the driver with vcpkg,
builds, and adds `mongui.exe` to your PATH:

```powershell
git clone https://github.com/RD-Freelance/mongui; cd mongui
./scripts/install.bat        # or:  powershell -ExecutionPolicy Bypass -File scripts/install.ps1
```

> **Windows note:** mongui renders with ANSI/VT escape sequences, so it needs
> Windows 10 1903+ and works best in **Windows Terminal**. The classic
> `conhost` console may not render the box-drawing characters cleanly.

### Manual CMake build

```bash
# macOS:  brew install mongo-c-driver cmake
# Debian/Ubuntu:  sudo apt install build-essential cmake pkg-config libmongoc-dev libbson-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/mongui
```

The build finds the driver via a CMake package config (vcpkg / Windows) or
pkg-config (Homebrew / Linux), whichever is present. On Apple Silicon it
auto-points pkg-config at `/opt/homebrew/lib/pkgconfig`.

### Clipboard support

The documents-pane **copy** action shells out to a clipboard tool: `pbcopy`
(macOS), `clip` (Windows), or `wl-copy` / `xclip` / `xsel` (Linux — the
installer adds `xclip` where available).

### Uninstall

The quickest way is to let mongui remove itself:

```bash
mongui --uninstall        # asks to confirm; deletes its own binary
mongui --uninstall -y     # skip the confirmation prompt
```

It deletes only the running binary. If it was installed somewhere you need
root for (e.g. `/usr/local/bin`), re-run with `sudo mongui --uninstall`. On
Windows the binary can't delete itself while running, so mongui schedules a
detached command to remove it a moment after it exits (then drop the install
dir from your PATH).

Or use the scripts (these also know the standard install locations):

```bash
# Linux / macOS
./scripts/uninstall.sh            # removes the mongui binary
./scripts/uninstall.sh --build    # also delete ./build
# installed to a custom prefix?  PREFIX=$HOME/.local ./scripts/uninstall.sh
```

```powershell
# Windows
./scripts/uninstall.bat                       # removes mongui.exe + PATH entry
# or:  powershell -ExecutionPolicy Bypass -File scripts/uninstall.ps1 -RemoveBuild
```

The uninstallers remove only mongui itself — they deliberately leave the
shared build tools and the MongoDB C driver in place, since other software may
depend on them. Remove those with your package manager / vcpkg if you want.

### CLI flags

```
mongui                    launch interactive TUI
mongui -v, --version      print version, libmongoc version, license
mongui -h, --help         print usage (also lists every in-TUI keybinding)
mongui --uninstall [-y]   remove the installed mongui binary
mongui --import …         bulk-import a file, then exit (see below)
```

---

## Import (non-interactive)

Load a file straight into a collection without opening the TUI:

```bash
mongui --import \
  --uri mongodb://localhost:27017 \
  --database shop \
  --collection orders \
  --filepath orders.json
```

- `--import` is implied whenever `--filepath` is given.
- `--uri` defaults to `mongodb://localhost:27017` if omitted.
- `--database` / `--db`, `--collection` / `--col`, and `--filepath` / `--file`
  are interchangeable aliases. Every flag accepts `--flag value` or `--flag=value`.

The file may be any of:

| Format | Example |
|--------|---------|
| **JSON array** | `[ { "name": "a" }, { "name": "b" } ]` |
| **NDJSON** (one doc per line) | `{"name":"a"}`<br>`{"name":"b"}` — mongoexport's default |
| **Single document** | `{ "name": "a" }` (may span multiple lines) |

Relaxed / mongosh JSON is accepted in all three (`ObjectId("…")`, unquoted keys,
single quotes, trailing commas — see [Relaxed BSON](#relaxed-bson)). Documents
are inserted in batches; the command prints a summary and exits non-zero on
failure:

```
Importing orders.json → shop.orders …
✔ imported 1284 document(s)
```

---

## Connecting

Launch and you'll see a centered card:

```
   ╭─────────────────────────────────────────────────────────────────╮
   │                                                                 │
   │  MONGUI    the mongodb terminal client                          │
   │                                                                 │
   ├─────────────────────────────────────────────────────────────────┤
   │                                                                 │
   │  Connection   press ↵ for default, ^c to quit                   │
   │                                                                 │
   │  URI        mongodb://localhost:27017   (default)               │
   │                                                                 │
   │  examples                                                       │
   │    mongodb://localhost:27017                                    │
   │    mongodb+srv://user:pass@cluster.mongodb.net                  │
   │    mongodb://user:pass@host:27017/db?authSource=admin           │
   │                                                                 │
   ╰─────────────────────────────────────────────────────────────────╯

   ▸
```

Press `↵` to accept the default URI, or paste your own. The default database
is parsed from the URI path (`mongodb://host/myapp` → `myapp`) and shown in
the next card — `↵` accepts, type a name to override.

---

## Modes

### Query — `^E`

Pick a database and collection, type a filter, run `find`. Query bar has
five fields: filter (defaults to `{}`), projection, sort, skip, limit. `TAB`
cycles through the five fields and then the **documents** pane.

| Key | Action |
|-----|--------|
| `↑ / ↓` | Navigate databases (in nav focus) |
| `← / →` | Navigate collections (in nav focus) |
| `^F` | Toggle fuzzy collection search |
| `TAB` | Cycle nav → filter / project / sort / skip / limit → documents |
| `↵` | Run `find` (resets to page 1) |
| `^N / ^P` | Next / previous page |
| `^J / ^K` | Scroll the document pane |

### Documents pane — per-document actions

`TAB` until the **documents** pane is focused (it gains a green title and a
selection bar). The selected document is marked with a `┃` gutter.

| Key | Action |
|-----|--------|
| `j / k` or `↑ / ↓` | Move the selection between documents |
| `y` | Copy the document JSON to the system clipboard (`pbcopy`) |
| `c` | Clone the document — re-inserts a copy **without `_id`** so the server mints a new one |
| `e` or `↵` | Edit the document **inline** as JSON (see below) |
| `d` | Delete the document — opens a `y` / `esc` confirm popup |

**Inline editor.** `e` turns the selected document into an editable JSON
buffer right where it sits (yellow gutter + block cursor). Type to edit;
`^S` saves via `replaceOne` on its `_id`; `esc` cancels. Clone/edit accept
the same relaxed BSON as the shell, so `ObjectId(...)`, `ISODate(...)`, etc.
are fine. Documents without an `_id` (e.g. some projections) can't be
edited or deleted.

### Aggregation — `^A`

Multi-line pipeline editor with autocomplete and smart editing.

| Key | Action |
|-----|--------|
| `^G` | Run pipeline (resets to page 1, refreshes total) |
| `^L` | Reformat / reindent the whole pipeline by depth |
| `^N / ^P` | Cycle autocomplete (when popup open) · paginate preview (when closed) |
| `TAB` | Accept suggestion, or insert 2 spaces |
| `↵` | Smart newline — copies indent; splits `{}` / `[]` / `()` onto a dedented closer |
| `← / →` | Move cursor (crosses line boundaries) |
| `Backspace` | Smart delete — removes paired brackets together |
| `Home / End` | Line start / end |

Auto-pair fires on `{`, `[`, `(`, `"`. Typing the closer skips past an
already-present one. Autocomplete activates after `$` and matches against
the full list of aggregation and query operators.

### Shell — `^S`

mongosh-style commands. The shell parses relaxed BSON (unquoted keys,
single-quoted strings, helper calls — see below).

```js
db.users.find({ email: 'a@b.c' })
db.users.findOne({ _id: ObjectId("…") })
db.users.countDocuments({ active: true })
db.users.aggregate([
  { $match: { age: { $gt: 21 } } },
  { $count: 'n' }
])

db.users.insertOne({ name: 'alice', age: 30 })
db.users.insertMany([{ … }, { … }])

db.users.updateOne ({ _id: 1 }, { $set: { age: 31 } })
db.users.updateMany({ active: false }, { $set: { archived: true } })
db.users.replaceOne({ _id: 1 }, { name: 'a2', age: 31 })

db.users.deleteOne ({ _id: 1 })
db.users.deleteMany({ archived: true })
```

#### Indexes

```js
db.users.createIndex({ email: 1 })
db.users.createIndex({ email: 1 }, { unique: true })
db.users.createIndex({ email: 1, created: -1 }, { name: 'idx_recent' })
db.users.createIndex({ loc: '2dsphere' })
db.users.createIndex({ body: 'text' })

db.users.createIndexes([
  { key: { email: 1 }, unique: true },
  { key: { loc: '2dsphere' } }
])

db.users.getIndexes()        // also listIndexes() / indexes()

db.users.dropIndex('email_1')
db.users.dropIndex({ email: 1 })
db.users.dropIndexes()        // drops all except _id
```

`createIndex` auto-names indexes from the keys when you don't pass one
(`{ email: 1 }` → `email_1`, `{ loc: '2dsphere' }` → `loc_2dsphere`).

---

## Tabs

Open up to **5 workspace tabs**, each a fully independent collection +
documents view with its own query bar, aggregation pipeline, shell buffer,
view mode, selection, and pagination. The tab bar across the top shows one
chip per tab; only the active tab fetches and renders data, so switching is
instant. A new tab opens on the currently selected database.

| Key | Action |
|-----|--------|
| `^T` | New tab (opens on the current db; max 5) |
| `^B` | Switch to the next tab (wraps around) |
| `^W` | Close the current tab (last one stays open) |

---

## Relaxed BSON

The shell and the query/pipeline editors accept mongosh-flavored input and
rewrite it to strict Extended JSON before handing it to libbson.

| Feature | Example |
|---------|---------|
| Unquoted keys | `{ name: 'alice' }` |
| Single-quoted strings | `'hello'` |
| ObjectId | `ObjectId("507f1f77bcf86cd799439011")` |
| Date helpers | `ISODate("2026-01-01")`, `Date(...)`, `new Date(...)` |
| Long / Int / Decimal | `NumberLong("…")`, `NumberInt("…")`, `NumberDecimal("3.14")` |
| Regex | `RegExp("pattern", "i")` |
| Trailing commas | `{ a: 1, b: 2, }` |

---

## Export — `^O`

`^O` opens a small picker:

- `j` — **JSON**: pretty-printed array via `bson_as_relaxed_extended_json`
- `c` — **CSV**: top-level fields only; nested docs/arrays compacted into one
  cell. Header row is the union of keys across all rows.

Files land in `~/Downloads/<db>.<col>.<find|agg>.<UTC-timestamp>.<json|csv>`,
capped at 100 000 documents (the cap is reported in the status line).

---

## Pagination

Both Query and Aggregation paginate server-side at 20 docs per page.

- **Query**: page X is fetched with `skip = user_skip + page * 20`. A parallel
  `countDocuments` gives the total, so the status line shows
  `page 2/63 · 21–40 of 1,247`.
- **Aggregation**: page X is fetched by appending
  `{ $skip: page * 20 }, { $limit: 20 }` to your pipeline. On a fresh `^G`
  Mongui also runs the pipeline with `{ $count: "n" }` appended once to get
  the total, so the status reads `page 2/8 · 21–40 of 158`. If the pipeline
  ends in `$out` or `$merge`, the count is skipped and only the current
  range is shown.

`^N` advances, `^P` goes back. Re-running with `^G` (agg) or `↵` (find)
resets to page 1.

## Layout

Mongui picks one of two layouts based on terminal width:

| Width | Layout | Description |
|-------|--------|-------------|
| ≥ 110 cols | **Wide** | Three columns: databases · collections · documents |
| < 110 cols | **Stack** | Horizontal db/collection pill selectors, vertical query fields, document pane fills the rest |

Both adapt as the terminal resizes — there is no minimum width below which
the app refuses to draw. The document pane always wraps its content; scroll
wrapped rows with `^J` / `^K`.

---

## Key reference

### Global
| Key | Action |
|-----|--------|
| `^E / ^A / ^S` | Switch to Query / Aggregation / Shell |
| `^T / ^B / ^W` | New / next / close tab |
| `^O` | Open export picker |
| `^J / ^K` | Scroll documents down / up |
| `^Q` or `^C` × 2 | Quit (clears the terminal) |

### Query
| Key | Action |
|-----|--------|
| `↑ / ↓` | Navigate databases |
| `← / →` | Navigate collections |
| `^F` | Toggle collection search |
| `TAB` | Next query field, then the documents pane |
| `↵` | Run `find` |

### Documents pane (after `TAB` into it)
| Key | Action |
|-----|--------|
| `j / k` or `↑ / ↓` | Select document |
| `y` | Copy to clipboard |
| `c` | Clone (without `_id`) |
| `e` or `↵` | Edit inline |
| `d` | Delete (with confirm) |
| `^S` / `esc` | Save / cancel while editing |

### Aggregation
| Key | Action |
|-----|--------|
| `^G` | Run pipeline |
| `^L` | Reformat pipeline |
| `^N / ^P` | Cycle autocomplete |
| `TAB` | Accept / insert 2 spaces |
| `↵` | Smart newline |
| `Backspace` | Smart delete |
| `Home / End` | Line start / end |

### Shell
| Key | Action |
|-----|--------|
| `↵` | Run command |
| `← / →` | Move cursor |
| `Backspace` | Delete |

---

## Project layout

```
src/
  main.cpp      — entry, alt-screen, sigint, clean-exit screen clear
  app.hpp/cpp   — State, render(), input loop, editor helpers, layouts
  term.hpp/cpp  — frame buffer, ANSI, raw mode (POSIX termios / Win32 console),
                  key reader, syntax highlighter
  mongo.hpp/cpp — pooled libmongoc, relaxed-BSON parser,
                  find / aggregate / shell / indexes / writes / import / export
scripts/
  install.sh    — one-command install for Linux & macOS
  uninstall.sh  — remove the binary (Linux & macOS)
  install.ps1   — one-command install for Windows (+ install.bat wrapper)
  uninstall.ps1 — remove the binary + PATH entry (+ uninstall.bat wrapper)
CMakeLists.txt
README.md
```

---

## Implementation notes

**Single-write rendering.** Every draw call appends to one `std::string`;
`flush_out()` emits the whole frame in a single `fwrite`. Atomic to the
terminal — no mid-frame tearing, fewer syscalls, noticeably smoother over
SSH or slow ttys.

**Pooled client.** `mongoc_client_pool_t` sized to
`std::thread::hardware_concurrency()`. Each call pops a client with RAII
and pushes it back on scope exit. The current main loop is single-threaded,
but the plumbing is in place for parallel work.

**Bounds-safety.** BSON nesting depth is capped at 64 to prevent stack
blow-ups on pathological inputs. Empty db/col are caught at the public
query boundary. `term::line()` silently drops writes that fall outside the
screen, so a mid-render resize can't corrupt the display.

**Cursor.** One block-cursor style — ANSI reverse video (`\x1b[7m`), not a
256-color background — is shared across the shell, the aggregation editor,
the query fields, and the inline document editor, so it's visible on every
terminal regardless of theme.

**Tabs.** The active tab's working set lives directly on `State`; inactive
tabs are snapshotted into a `Tab` vector and swapped in/out on switch, so
existing render and input code is unchanged — it always operates on the
current tab.

---

## Troubleshooting

- **`Could not connect / list databases`** — usually auth or unreachable
  host. The URI is parsed lazily, so syntax errors surface here.
- **Garbled UTF-8 after editing** — `read_key()` reads one byte at a time;
  multi-byte characters typed mid-line will land as separate bytes and
  look broken. Stick to ASCII in the editor for now.
- **Cursor invisible after a change** — should never happen with reverse
  video. If it does, your terminal is filtering escape sequences.
- **Tiny window** — the layout adapts down to ~40 cols. Below that, text
  may overlap; resize and the next frame will recompute.

---

## License

[MIT](LICENSE) · Copyright © 2026 **RD-Freelance**.

Built with [mongo-c-driver](https://github.com/mongodb/mongo-c-driver) (Apache 2.0).
