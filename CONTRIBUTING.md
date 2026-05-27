# Contributing to Mongui

Thanks for taking the time to contribute! Mongui is a small, focused project
— bug reports, polish, and feature PRs are all welcome.

## Quick start

```bash
# Clone
git clone https://github.com/RD-Freelance/mongui.git
cd mongui

# Install deps
brew install mongo-c-driver cmake pkg-config        # macOS
sudo apt-get install libmongoc-dev libbson-dev cmake pkg-config build-essential   # Ubuntu/Debian

# Build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Smoke test
./build/mongui --version
./build/mongui
```

## Project layout

```
src/
  main.cpp      — entry, alt-screen, sigint, --version / --help
  app.hpp/cpp   — State, render(), input loop, editor helpers, layouts
  term.hpp/cpp  — frame buffer, ANSI, raw mode, key reader, syntax highlighter
  mongo.hpp/cpp — pooled libmongoc, relaxed-BSON parser,
                  find / aggregate / shell / indexes / export
.github/        — CI workflow + issue and PR templates
CMakeLists.txt
LICENSE         — MIT
README.md
CHANGELOG.md
```

## Reporting bugs

Open an issue using the **Bug report** template. Please include:

- Mongui version (`./mongui --version`)
- Operating system + terminal emulator (iTerm2, Alacritty, gnome-terminal, …)
- libmongoc / mongo-c-driver version
- Steps to reproduce
- What you expected vs. what happened
- A screenshot if it's a rendering issue

## Suggesting features

Open an issue using the **Feature request** template. Lead with the
problem you're trying to solve; the implementation idea is secondary.

## Pull requests

1. **Discuss first** for non-trivial changes — open an issue describing
   the approach. Avoids you doing work that won't land.
2. **One change per PR.** Mixed PRs get bounced back.
3. **Keep the diff focused.** Don't reformat unrelated files; don't
   sneak in dependency bumps.
4. **Test on both platforms** if you can (macOS + Linux). CI will catch
   what you miss.
5. **Update the CHANGELOG** under `[Unreleased]` if your change is user-visible.
6. **No new dependencies** without discussion. Mongui's only runtime dep is
   libmongoc.

## Style

C++17, no exceptions, no RTTI required.

- **Naming**: `lower_snake_case` for functions and variables;
  `UpperCamelCase` for types; `kCONSTANT` not used — prefer `constexpr`.
- **Comments**: only when the *why* is non-obvious. Don't restate the code.
- **Headers**: include what you use; group `<std…>` then `"local.hpp"`.
- **Match the surrounding style.** No reformat-only PRs.

## Commit messages

Short imperative subject line (≤ 70 chars), blank line, optional body. Prefix
with the affected area when useful:

```
agg: clamp cursor to pipeline pane on long lines
shell: add createIndex / dropIndex / getIndexes
readme: add pagination section
```

## Release process (maintainers)

1. Bump version in `CMakeLists.txt` (`project(... VERSION x.y.z)`) and in
   `src/main.cpp`'s `MONGUI_VERSION` fallback.
2. Move `[Unreleased]` entries to a new `[x.y.z]` section in `CHANGELOG.md`.
3. Tag `vx.y.z`, push tag.
4. Create a GitHub Release; CI attaches the built binaries.

## License

By contributing, you agree that your contributions will be licensed under
the [MIT License](LICENSE).
