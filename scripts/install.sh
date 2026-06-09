#!/usr/bin/env bash
# =============================================================================
# mongui installer — Linux & macOS
#
# One command:
#     ./scripts/install.sh
#
# It will:
#   1. Install the build tools + MongoDB C driver using your system's package
#      manager (building the driver from source if no package is available).
#   2. Build mongui with CMake (Release).
#   3. Install the `mongui` binary onto your PATH.
#
# Override the install location:   PREFIX=$HOME/.local ./scripts/install.sh
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# Where to install. Default to /usr/local (system-wide); fall back to ~/.local
# automatically if /usr/local is not writable and sudo is unavailable.
PREFIX="${PREFIX:-/usr/local}"

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
info() { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

have() { command -v "$1" >/dev/null 2>&1; }

# Run a command with sudo when we are not already root and sudo exists.
SUDO=""
if [ "$(id -u)" -ne 0 ] && have sudo; then SUDO="sudo"; fi
maybe_sudo() { if [ -n "$SUDO" ]; then $SUDO "$@"; else "$@"; fi; }

# ---------------------------------------------------------------------------
# 1. Install dependencies
# ---------------------------------------------------------------------------
install_deps() {
    local os; os="$(uname -s)"

    if [ "$os" = "Darwin" ]; then
        have brew || die "Homebrew not found. Install it from https://brew.sh and re-run."
        info "Installing dependencies via Homebrew…"
        brew install cmake mongo-c-driver pkg-config || true
        return
    fi

    # ----- Linux: detect the package manager -----
    if   have apt-get; then
        info "Installing dependencies via apt…"
        maybe_sudo apt-get update -y
        maybe_sudo apt-get install -y build-essential cmake pkg-config \
            libmongoc-dev libbson-dev xclip || DRIVER_FROM_SOURCE=1
    elif have dnf; then
        info "Installing dependencies via dnf…"
        maybe_sudo dnf install -y gcc-c++ cmake pkgconf-pkg-config \
            mongo-c-driver-devel xclip || DRIVER_FROM_SOURCE=1
    elif have yum; then
        info "Installing dependencies via yum…"
        maybe_sudo yum install -y gcc-c++ cmake pkgconfig \
            mongo-c-driver-devel xclip || DRIVER_FROM_SOURCE=1
    elif have pacman; then
        info "Installing dependencies via pacman…"
        maybe_sudo pacman -Sy --needed --noconfirm base-devel cmake \
            mongo-c-driver xclip || DRIVER_FROM_SOURCE=1
    elif have zypper; then
        info "Installing dependencies via zypper…"
        maybe_sudo zypper install -y gcc-c++ cmake pkg-config \
            mongo-c-driver-devel xclip || DRIVER_FROM_SOURCE=1
    elif have apk; then
        info "Installing dependencies via apk…"
        maybe_sudo apk add --no-cache build-base cmake pkgconf \
            mongo-c-driver-dev xclip || DRIVER_FROM_SOURCE=1
    else
        die "No supported package manager found (apt/dnf/yum/pacman/zypper/apk).
Install cmake, a C++17 compiler, and mongo-c-driver manually, then run:
    cmake -S \"$REPO_ROOT\" -B \"$BUILD_DIR\" && cmake --build \"$BUILD_DIR\" -j"
    fi
}

# Build mongo-c-driver from source if the distro had no package for it.
build_driver_from_source() {
    info "No mongo-c-driver package — building it from source…"
    have cmake || die "cmake is required to build the driver from source."
    local ver="1.27.5"
    local tmp; tmp="$(mktemp -d)"
    local url="https://github.com/mongodb/mongo-c-driver/releases/download/${ver}/mongo-c-driver-${ver}.tar.gz"
    info "Downloading mongo-c-driver ${ver}…"
    if have curl; then curl -fsSL "$url" -o "$tmp/driver.tgz"
    elif have wget; then wget -qO "$tmp/driver.tgz" "$url"
    else die "need curl or wget to download the driver"; fi
    tar -xzf "$tmp/driver.tgz" -C "$tmp"
    cmake -S "$tmp/mongo-c-driver-${ver}" -B "$tmp/build" \
          -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF
    cmake --build "$tmp/build" -j "$JOBS"
    maybe_sudo cmake --install "$tmp/build"
    # Refresh the linker cache so the freshly installed .so is found.
    have ldconfig && maybe_sudo ldconfig || true
    rm -rf "$tmp"
}

# ---------------------------------------------------------------------------
# 2. Build mongui
# ---------------------------------------------------------------------------
build_mongui() {
    # Start from a clean cache so a driver upgrade can't leave stale paths.
    rm -rf "$BUILD_DIR"
    info "Configuring (CMake, Release)…"
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
    info "Building (-j $JOBS)…"
    cmake --build "$BUILD_DIR" -j "$JOBS"
}

# ---------------------------------------------------------------------------
# 3. Install onto PATH
# ---------------------------------------------------------------------------
install_binary() {
    local bindir="$PREFIX/bin"
    # Prefer no elevation: if the prefix is user-writable, install directly.
    # Else use sudo if available. Else fall back to ~/.local/bin.
    if mkdir -p "$bindir" 2>/dev/null && [ -w "$bindir" ]; then
        SUDO=""
    elif [ -n "$SUDO" ] && $SUDO mkdir -p "$bindir" 2>/dev/null; then
        :
    else
        warn "$bindir not writable — installing to \$HOME/.local/bin instead."
        PREFIX="$HOME/.local"; bindir="$PREFIX/bin"; SUDO=""
        mkdir -p "$bindir"
    fi
    info "Installing binary to $bindir/mongui…"
    maybe_sudo install -m 0755 "$BUILD_DIR/mongui" "$bindir/mongui"

    if ! printf '%s' ":$PATH:" | grep -q ":$bindir:"; then
        warn "$bindir is not on your PATH. Add this line to your shell rc:"
        printf '    export PATH="%s:$PATH"\n' "$bindir"
    fi
}

main() {
    bold "Installing mongui…"
    DRIVER_FROM_SOURCE=0
    install_deps
    if [ "${DRIVER_FROM_SOURCE:-0}" = "1" ]; then build_driver_from_source; fi
    build_mongui
    install_binary
    echo
    info "Done. Run it with:  mongui"
}

main "$@"
