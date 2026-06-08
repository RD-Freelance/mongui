# Mongui


> A keyboard-driven MongoDB terminal client written in C++17.
> By **RD-Freelance** · MIT licensed · [LICENSE](LICENSE)

Browse databases, run queries and aggregations, manage indexes, paginate
through results, and drive a mongosh-style shell — all in a single binary
that adapts to whatever terminal size you give it.

```
 mongui  ·  admin › users                                            QUERY

┌ databases ────────┐┌ collections ─────────────┐┌ documents ────────────────
│ › admin           ││ search ▸ user            ││ filter  ▸ { age: { $gt: 21 } }
│   config          ││ › users                  ││ project ▸ (optional)
│   local           ││   user_events            ││ sort    ▸ { _id: -1 }
│   myapp           ││   user_sessions          ││ skip    ▸    limit ▸ 50
└───────────────────┘└──────────────────────────┘│ ─────────────────────────
                                                  │ │ {
                                                  │ │   "_id": ObjectId("…"),
                                                  │ │   "name": "alice",
                                                  │ │   "age": 30
                                                  │ │ }

 ^e query  ^a agg  ^s shell    tab field  ^f search  ↵ find    ^o export  ^q quit
                                                                         3 / 1,204 docs
```

---

## Highlights

- **Three modes** — Query, Aggregation, Shell — toggled with `^E` / `^A` / `^S`
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
libmongoc-1.0).

### CMake

```bash
brew install mongo-c-driver cmake          # macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/mongui
```

### Direct (no CMake)

```bash
clang++ -std=c++17 -O2 -DMONGUI_VERSION='"0.1.0"' \
  $(pkg-config --cflags mongoc2) \
  -o mongui src/main.cpp src/term.cpp src/mongo.cpp src/app.cpp \
  $(pkg-config --libs mongoc2)
./mongui
```

On Apple Silicon, the CMake build auto-points pkg-config at
`/opt/homebrew/lib/pkgconfig`.

### CLI flags

```
mongui                    launch interactive TUI
mongui -v, --version      print version, libmongoc version, license
mongui -h, --help         print usage
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
five fields: filter, projection, sort, skip, limit.

| Key | Action |
|-----|--------|
| `↑ / ↓` | Navigate databases |
| `← / →` | Navigate collections |
| `^F` | Toggle fuzzy collection search |
| `TAB` | Cycle filter / project / sort / skip / limit |
| `↵` | Run `find` (resets to page 1) |
| `^N / ^P` | Next / previous page |
| `^J / ^K` | Scroll the document pane |

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
| `^O` | Open export picker |
| `^J / ^K` | Scroll documents down / up |
| `^Q` or `^C` × 2 | Quit (clears the terminal) |

### Query
| Key | Action |
|-----|--------|
| `↑ / ↓` | Navigate databases |
| `← / →` | Navigate collections |
| `^F` | Toggle collection search |
| `TAB` | Next query field |
| `↵` | Run `find` |

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
  term.hpp/cpp  — frame buffer, ANSI, raw mode, key reader, syntax highlighter
  mongo.hpp/cpp — pooled libmongoc, relaxed-BSON parser,
                  find / aggregate / shell / indexes / export
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

**Cursor.** The aggregation block-cursor uses ANSI reverse video
(`\x1b[7m`), not a 256-color background — visible on every terminal
regardless of theme.

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
