#!/usr/bin/env bash
# Build "phoc-anland": Phosh's compositor (phoc) with Anland's wlroots
# backend, so a Phosh session shows up in the Anland Android app.
#
# Phoc is built at the same version as the distribution's phoc package, so
# its GSettings schema and phosh itself stay compatible. wlroots is embedded
# (phoc's own subproject, linked statically) and carries Anland's swapchain
# patch; the Anland producer library is linked statically as well. The result
# is one binary that does not touch the system wlroots or phoc.
#
# Usage: build-phoc-anland.sh [--check]
#   --check  only print the versions that would be built
#
# Environment:
#   DESTDIR          install root (default: /)
#   WORK_DIR         scratch directory (default: /tmp/phoc-anland-build)
#   JOBS             parallel jobs (default: nproc)
#   PHOC_VERSION     phoc release to build (default: installed phoc package)
#   ANLAND_REVISION  Anland commit providing the backend
#
# Needs: git, gcc, meson, ninja, cmake, pkg-config, patch, and the -devel
# packages phoc and wlroots build against (see scripts/desktops/phosh.sh).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHOC_FILES="$SCRIPT_DIR/phoc"

: "${ANLAND_REPOSITORY:=https://github.com/SuperTurtleDev/anland}"
: "${ANLAND_REVISION:=d60a2a5b977f7f66476a8f2ac476eb7a16c8d65f}"
: "${PHOC_REPOSITORY:=https://gitlab.gnome.org/World/Phosh/phoc.git}"
: "${PHOC_VERSION:=}"
: "${WORK_DIR:=/tmp/phoc-anland-build}"
: "${JOBS:=$(nproc)}"
: "${DESTDIR:=}"

# phoc-anland.patch is written against this phoc release. Other releases are
# tried, and the build stops if the patch no longer applies.
readonly PATCHED_PHOC_VERSION=0.57.0
readonly INSTALL_PATH=/usr/libexec/phoc-anland

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'build-phoc-anland: %s\n' "$*" >&2; exit 1; }

installed_phoc_version() {
  if command -v rpm >/dev/null 2>&1 && rpm -q phoc >/dev/null 2>&1; then
    rpm -q --qf '%{VERSION}\n' phoc
  elif command -v dpkg-query >/dev/null 2>&1 && dpkg-query -W -f='${Version}\n' phoc 2>/dev/null | grep -q .; then
    dpkg-query -W -f='${Version}\n' phoc | sed 's/^[0-9]*://; s/[~+-].*//'
  elif command -v pacman >/dev/null 2>&1 && pacman -Q phoc >/dev/null 2>&1; then
    pacman -Q phoc | awk '{print $2}' | sed 's/-[^-]*$//'
  fi
}

[[ $# -le 1 ]] || die "usage: build-phoc-anland.sh [--check]"
case "${1:-}" in ''|--check) ;; *) die "usage: build-phoc-anland.sh [--check]" ;; esac

[[ -n "$PHOC_VERSION" ]] || PHOC_VERSION="$(installed_phoc_version || true)"
[[ -n "$PHOC_VERSION" ]] || PHOC_VERSION="$PATCHED_PHOC_VERSION"
[[ "$PHOC_VERSION" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] || die "invalid phoc version: $PHOC_VERSION"
[[ "$ANLAND_REVISION" =~ ^[0-9a-f]{40}$ ]] || die "ANLAND_REVISION must be a full commit hash"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "JOBS must be a positive integer"

printf 'phoc %s (patch written for %s), Anland %s\n' \
  "$PHOC_VERSION" "$PATCHED_PHOC_VERSION" "$ANLAND_REVISION"
[[ "${1:-}" != --check ]] || exit 0

for tool in git gcc meson ninja cmake pkg-config patch; do
  command -v "$tool" >/dev/null 2>&1 || die "missing build tool: $tool"
done

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
ANLAND_SRC="$WORK_DIR/anland"
PHOC_SRC="$WORK_DIR/phoc"
PRODUCER_BUILD="$WORK_DIR/display-producer"

log "Fetching Anland $ANLAND_REVISION"
git init -q "$ANLAND_SRC"
git -C "$ANLAND_SRC" remote add origin "$ANLAND_REPOSITORY"
git -C "$ANLAND_SRC" fetch -q --depth 1 origin "$ANLAND_REVISION"
git -C "$ANLAND_SRC" checkout -q FETCH_HEAD

BACKEND_SRC="$ANLAND_SRC/producers/mango/anland_backend"
WLROOTS_PATCH="$ANLAND_SRC/producers/mango/Arch_v5/wlroots.patch"
[[ -f "$BACKEND_SRC/anland.c" && -f "$WLROOTS_PATCH" ]] ||
  die "this Anland revision has no Mango wlroots backend"

log "Fetching phoc $PHOC_VERSION"
git clone -q --depth 1 --branch "v$PHOC_VERSION" "$PHOC_REPOSITORY" "$PHOC_SRC"

log "Building the Anland producer library (static)"
mkdir -p "$PRODUCER_BUILD"
cat > "$PRODUCER_BUILD/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.10)
project(anland_display_producer C)
include("${ANLAND_SOURCE_DIR}/libdisplay_producer/producer.cmake")
anland_add_producer_target(display_producer STATIC)
EOF
cmake -S "$PRODUCER_BUILD" -B "$PRODUCER_BUILD/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DANLAND_SOURCE_DIR="$ANLAND_SRC"
cmake --build "$PRODUCER_BUILD/build" --parallel "$JOBS"
mkdir -p "$PRODUCER_BUILD/pkgconfig"
cat > "$PRODUCER_BUILD/pkgconfig/display-producer.pc" <<EOF
Name: display-producer
Description: Anland producer library (static, for phoc-anland)
Version: 5.0.0
Requires.private: libpipewire-0.3
Cflags: -I$ANLAND_SRC/libdisplay_producer -I$ANLAND_SRC/common
Libs: $PRODUCER_BUILD/build/libdisplay_producer.a -lpthread $(pkg-config --libs libpipewire-0.3)
EOF

log "Adding the Anland backend to phoc"
mkdir -p "$PHOC_SRC/src/anland_backend" "$PHOC_SRC/src/anland"
# The backend directory also holds symlinks to the whole producer tree; only
# the backend's own sources are needed.
cp "$BACKEND_SRC"/*.c "$BACKEND_SRC"/*.h "$PHOC_SRC/src/anland_backend/"
cp -R "$PHOC_FILES/glue/." "$PHOC_SRC/src/anland/"
patch --batch --forward -p1 -d "$PHOC_SRC" -i "$PHOC_FILES/phoc-anland.patch" ||
  die "phoc-anland.patch does not apply to phoc $PHOC_VERSION (written for $PATCHED_PHOC_VERSION)"

log "Fetching phoc's subprojects and patching its embedded wlroots"
(cd "$PHOC_SRC" && meson subprojects download wlroots gvdb)
WLROOTS_DIR="$(sed -n 's/^directory *= *//p' "$PHOC_SRC/subprojects/wlroots.wrap")"
[[ -n "$WLROOTS_DIR" && -d "$PHOC_SRC/subprojects/$WLROOTS_DIR" ]] || die "wlroots subproject missing"
patch --batch --forward -p1 -d "$PHOC_SRC/subprojects/$WLROOTS_DIR" -i "$WLROOTS_PATCH" ||
  die "Anland's wlroots patch does not apply to phoc's wlroots"

log "Building phoc-anland"
PKG_CONFIG_PATH="$PRODUCER_BUILD/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
meson setup "$PHOC_SRC/build" "$PHOC_SRC" \
  --buildtype=release --prefix=/usr --wrap-mode=nodownload \
  -Danland=enabled -Dembed-wlroots=enabled -Dxwayland=enabled \
  -Dtests=false -Dman=false -Dgtk_doc=false \
  -Dwlroots:default_library=static -Dwlroots:examples=false -Dwlroots:renderers=gles2 \
  -Dwlroots:backends=drm,libinput -Dwlroots:allocators=gbm
meson compile -C "$PHOC_SRC/build" -j "$JOBS" src/phoc:executable

PHOC_BIN="$PHOC_SRC/build/src/phoc"
[[ -x "$PHOC_BIN" ]] || die "phoc binary was not produced"
if ldd "$PHOC_BIN" | grep -E 'not found|libwlroots|libdisplay_producer'; then
  die "phoc-anland must link wlroots and the producer statically, with every library present"
fi

log "Installing $INSTALL_PATH"
install -Dm0755 "$PHOC_BIN" "$DESTDIR$INSTALL_PATH"
install -Dm0644 "$ANLAND_SRC/LICENSE" "$DESTDIR/usr/share/licenses/phoc-anland/anland-LICENSE"
for license in "$PHOC_SRC"/LICENSES/*; do
  install -Dm0644 "$license" "$DESTDIR/usr/share/licenses/phoc-anland/phoc-${license##*/}"
done
install -Dm0644 "$PHOC_SRC/subprojects/$WLROOTS_DIR/LICENSE" \
  "$DESTDIR/usr/share/licenses/phoc-anland/wlroots-LICENSE"
printf 'phoc=%s\nanland=%s\n' "$PHOC_VERSION" "$ANLAND_REVISION" |
  install -Dm0644 /dev/stdin "$DESTDIR/usr/share/phoc-anland/version"

log "Done: $DESTDIR$INSTALL_PATH"
