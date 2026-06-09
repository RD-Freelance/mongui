#!/usr/bin/env bash
# =============================================================================
# mongui uninstaller — Linux & macOS
#
# One command:
#     ./scripts/uninstall.sh
#
# Removes the installed `mongui` binary from the standard locations. It does NOT
# remove the build tools or the MongoDB C driver that install.sh pulled in —
# those are general-purpose and may be used by other software.
#
#   PREFIX=$HOME/.local ./scripts/uninstall.sh   # if you installed there
#   ./scripts/uninstall.sh --build               # also delete ./build
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"

info() { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m==>\033[0m %s\n' "$*"; }

have() { command -v "$1" >/dev/null 2>&1; }
SUDO=""
if [ "$(id -u)" -ne 0 ] && have sudo; then SUDO="sudo"; fi

REMOVE_BUILD=0
[ "${1:-}" = "--build" ] && REMOVE_BUILD=1

# Candidate install locations: an explicit PREFIX, the system prefix, and the
# per-user fallback that install.sh uses when /usr/local isn't writable.
candidates=()
[ -n "${PREFIX:-}" ] && candidates+=("$PREFIX/bin/mongui")
candidates+=("/usr/local/bin/mongui" "$HOME/.local/bin/mongui")

# Also catch anything else named mongui on PATH (e.g. a custom prefix).
if have command; then
    if path_hit="$(command -v mongui 2>/dev/null)"; then candidates+=("$path_hit"); fi
fi

removed=0
seen=" "   # space-delimited list of already-handled paths (bash 3.2 friendly)
for f in "${candidates[@]}"; do
    case "$seen" in *" $f "*) continue ;; esac
    seen="$seen$f "
    if [ -e "$f" ] || [ -L "$f" ]; then
        info "Removing $f"
        if [ -w "$(dirname "$f")" ]; then
            rm -f "$f" && removed=$((removed + 1)) || warn "could not remove $f"
        elif [ -n "$SUDO" ]; then
            $SUDO rm -f "$f" && removed=$((removed + 1)) \
                || warn "could not remove $f (run again in a terminal that can sudo)"
        else
            warn "no permission to remove $f (try: sudo rm -f '$f')"
        fi
    fi
done

if [ "$REMOVE_BUILD" = "1" ] && [ -d "$BUILD_DIR" ]; then
    info "Removing build directory $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

if [ "$removed" -eq 0 ]; then
    warn "No installed mongui binary found in the standard locations."
    warn "If you used a custom PREFIX, run: PREFIX=/your/prefix ./scripts/uninstall.sh"
else
    info "Done — removed $removed binary file(s)."
fi
