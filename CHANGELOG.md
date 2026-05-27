# Changelog

All notable changes to Mongui will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] — 2026-05-28

Initial public release by RD-Freelance.

### Added
- Three operating modes — **Query**, **Aggregation**, **Shell** — toggled with `^E` / `^A` / `^S`.
- **Server-side pagination** in both Query and Aggregation modes (`^N` / `^P`,
  20 docs per page). Query shows total via `countDocuments`; Aggregation shows
  total via a parallel `{ $count: "n" }` pipeline.
- **Responsive layout** — three-pane wide layout when terminal ≥ 110 cols,
  stacked single-column layout below that. Adapts on resize.
- **Document wrapping** — long BSON lines wrap onto continuation rows with a
  scrollbar in the doc pane. No content is hidden.
- **Type-aware display** — `NumberLong("…")`, `NumberDecimal("…")`,
  `ObjectId("…")`, `ISODate("…")` so the BSON type is always visible and the
  output round-trips through the relaxed-BSON parser.
- **Syntax highlighting** for keys, strings, numbers, booleans, `$operators`,
  and helper calls.
- **Smart aggregation editor** — auto-paired `{ } [ ] ( ) "`, smart Enter (copy
  indent, split brackets onto dedented closer), smart backspace, autocomplete
  on `$operators`, `^L` reformat by depth, per-line horizontal scroll on long
  lines.
- **Mongo shell** with relaxed-BSON parsing: `find`, `findOne`, `aggregate`,
  `countDocuments`, `insertOne`/`Many`, `updateOne`/`Many`, `replaceOne`,
  `deleteOne`/`Many`, plus full index ops (`createIndex`/`createIndexes`,
  `dropIndex`/`dropIndexes`, `getIndexes`).
- **Export** to JSON or CSV (`^O`), capped at 100 000 docs per file.
- **Pooled libmongoc** client sized to `hardware_concurrency()`.
- **Single-write rendering** — every frame goes out as one atomic `fwrite`,
  no flicker even over SSH.
- **Connect screen** — centered rounded card with examples and key hints.
- **CLI** — `--version`, `--help`.
- **Clear-on-exit** — leaves the terminal on a fresh prompt.

### Documentation
- README with build instructions, mode docs, key reference, pagination notes.
- MIT LICENSE.

[Unreleased]: https://github.com/RD-Freelance/mongui/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/RD-Freelance/mongui/releases/tag/v0.1.0
