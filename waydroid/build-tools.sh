#!/usr/bin/env bash
# Build the two static helper programs that go into the Waydroid rootfs:
#
#   ds-waydroid-init        /sbin/init of the rootfs
#   anland-waydroid-bridge  shows Android's screen in the Anland app
#
# Both are linked fully static so they run inside an Android image, which has
# no usable C library before Android's own init has started.
#
# Usage: build-tools.sh [-o OUTPUT_DIR] [-t TARGET_TRIPLET] [-w WORK_DIR]
#   -t  cross-compile, e.g. aarch64-linux-gnu (needs gcc-<triplet>)
#
# Needs: gcc, meson, ninja, pkg-config, wayland-scanner (libwayland-bin),
#        wayland-protocols, git, and "deb-src" entries for apt-get source.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/out/tools"
WORK_DIR="$SCRIPT_DIR/out/tools-work"
TARGET=""

# Anland provides the producer library the bridge talks to the app with.
: "${ANLAND_REPOSITORY:=https://github.com/SuperTurtleDev/anland}"
: "${ANLAND_REVISION:=ea10fdb542e14c2756269e0a42c708fb87de32e7}"

while getopts "o:t:w:h" opt; do
  case "$opt" in
    o) OUT_DIR="$OPTARG" ;;
    t) TARGET="$OPTARG" ;;
    w) WORK_DIR="$OPTARG" ;;
    *) sed -n '2,16p' "$0"; exit 1 ;;
  esac
done

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

if [[ -n "$TARGET" ]]; then
  CC="${TARGET}-gcc"; AR="${TARGET}-ar"; STRIP="${TARGET}-strip"
else
  CC="${CC:-gcc}"; AR="${AR:-ar}"; STRIP="${STRIP:-strip}"
fi
for tool in "$CC" "$AR" meson ninja pkg-config wayland-scanner git; do
  command -v "$tool" >/dev/null || die "missing build tool: $tool"
done

PROTOCOLS_DIR="$(pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null || true)"
XDG_SHELL_XML="$PROTOCOLS_DIR/stable/xdg-shell/xdg-shell.xml"
DMABUF_XML="$PROTOCOLS_DIR/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml"
[[ -f "$XDG_SHELL_XML" ]] || die "wayland-protocols is not installed (xdg-shell.xml not found)"
[[ -f "$DMABUF_XML" ]] || die "wayland-protocols has no linux-dmabuf-unstable-v1.xml"

mkdir -p "$OUT_DIR" "$WORK_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
WORK_DIR="$(cd "$WORK_DIR" && pwd)"
PREFIX="$WORK_DIR/prefix"
SRC_DIR="$WORK_DIR/src"
mkdir -p "$PREFIX" "$SRC_DIR"

# --- sources ---------------------------------------------------------------
fetch_source() { # name
  local name="$1"
  if ! compgen -G "$SRC_DIR/${name}-*/" >/dev/null; then
    log "Fetching $name source" >&2
    (cd "$SRC_DIR" && apt-get source -qq "$name") >&2 \
      || die "apt-get source $name failed (enable deb-src in the apt sources, install dpkg-dev)"
  fi
  compgen -G "$SRC_DIR/${name}-*/" | head -n1
}

WAYLAND_SRC="$(fetch_source wayland)"

if [[ ! -d "$SRC_DIR/anland/.git" ]]; then
  log "Fetching Anland $ANLAND_REVISION"
  git init -q "$SRC_DIR/anland"
  git -C "$SRC_DIR/anland" remote add origin "$ANLAND_REPOSITORY"
fi
git -C "$SRC_DIR/anland" fetch -q --depth 1 origin "$ANLAND_REVISION"
git -C "$SRC_DIR/anland" checkout -q FETCH_HEAD
ANLAND_SRC="$SRC_DIR/anland"

# --- libffi ----------------------------------------------------------------
# A native build can use the distribution's static libffi (libffi-dev). Only a
# cross build has to compile its own.
FFI_LIB_DIR=""
if [[ -z "$TARGET" ]]; then
  system_ffi="$("$CC" -print-file-name=libffi.a)"
  [[ "$system_ffi" == /* && -f "$system_ffi" ]] && FFI_LIB_DIR="$(dirname "$system_ffi")"
fi
if [[ -z "$FFI_LIB_DIR" && ! -f "$PREFIX/lib/libffi.a" ]]; then
  FFI_SRC="$(fetch_source libffi)"
  log "Building static libffi"
  command -v autoreconf >/dev/null || die "autoconf, automake, libtool and libltdl-dev are needed to build libffi"
  [[ -x "$FFI_SRC/configure" ]] || (cd "$FFI_SRC" && autoreconf -fi >/dev/null 2>&1)
  rm -rf "$WORK_DIR/build-ffi"; mkdir -p "$WORK_DIR/build-ffi"
  (cd "$WORK_DIR/build-ffi" && \
    "$FFI_SRC/configure" --prefix="$PREFIX" --libdir="$PREFIX/lib" \
      ${TARGET:+--host="$TARGET"} CC="$CC" CFLAGS="-O2" \
      --disable-shared --enable-static --disable-docs --disable-multi-os-directory >/dev/null && \
    make -j"$(nproc)" >/dev/null 2>&1 && make install >/dev/null 2>&1)
fi

# --- libwayland-server -----------------------------------------------------
if [[ ! -f "$PREFIX/lib/libwayland-server.a" ]]; then
  log "Building static libwayland-server"
  rm -rf "$WORK_DIR/build-wayland"
  cross_args=()
  if [[ -n "$TARGET" ]]; then
    cpu="${TARGET%%-*}"
    cat > "$WORK_DIR/cross.ini" <<CROSS
[binaries]
c = '$CC'
ar = '$AR'
strip = '$STRIP'
pkg-config = 'pkg-config'
[properties]
pkg_config_libdir = '$PREFIX/lib/pkgconfig'
[host_machine]
system = 'linux'
cpu_family = '$cpu'
cpu = '$cpu'
endian = 'little'
CROSS
    cross_args=(--cross-file "$WORK_DIR/cross.ini")
  fi
  PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
  CC="$CC" meson setup "$WORK_DIR/build-wayland" "$WAYLAND_SRC" "${cross_args[@]}" \
    --prefix="$PREFIX" --libdir=lib --buildtype=release --default-library=static \
    -Dscanner=false -Dtests=false -Ddocumentation=false -Ddtd_validation=false >/dev/null
  ninja -C "$WORK_DIR/build-wayland" install >/dev/null
fi

# --- protocol glue ---------------------------------------------------------
GEN_DIR="$WORK_DIR/gen"
mkdir -p "$GEN_DIR"
wayland-scanner server-header "$XDG_SHELL_XML" "$GEN_DIR/xdg-shell-server-protocol.h"
wayland-scanner private-code "$XDG_SHELL_XML" "$GEN_DIR/xdg-shell-protocol.c"
wayland-scanner server-header "$DMABUF_XML" "$GEN_DIR/linux-dmabuf-unstable-v1-server-protocol.h"
wayland-scanner private-code "$DMABUF_XML" "$GEN_DIR/linux-dmabuf-unstable-v1-protocol.c"

# --- the two programs ------------------------------------------------------
CFLAGS_COMMON=(-O2 -Wall -Wextra -Wno-format-truncation -static)

log "Building ds-waydroid-init"
"$CC" "${CFLAGS_COMMON[@]}" -o "$OUT_DIR/ds-waydroid-init" "$SCRIPT_DIR/src/ds-waydroid-init.c"

log "Building anland-waydroid-bridge"
"$CC" "${CFLAGS_COMMON[@]}" -O3 \
  -I"$GEN_DIR" -I"$PREFIX/include" \
  -I"$SCRIPT_DIR/src" -I"$ANLAND_SRC/libdisplay_producer" -I"$ANLAND_SRC/common" \
  -o "$OUT_DIR/anland-waydroid-bridge" \
  "$SCRIPT_DIR/src/anland-waydroid-bridge.c" \
  "$SCRIPT_DIR/src/anland-clipboard.c" \
  "$GEN_DIR/xdg-shell-protocol.c" \
  "$GEN_DIR/linux-dmabuf-unstable-v1-protocol.c" \
  "$ANLAND_SRC/libdisplay_producer/display_producer.c" \
  "$ANLAND_SRC/common/socket_utils.c" \
  -L"$PREFIX/lib" ${FFI_LIB_DIR:+-L"$FFI_LIB_DIR"} -lwayland-server -lffi -lm -lrt -lpthread

"$STRIP" "$OUT_DIR/ds-waydroid-init" "$OUT_DIR/anland-waydroid-bridge"
printf '%s\n' "$ANLAND_REVISION" > "$OUT_DIR/anland-revision.txt"

log "Done"
ls -l "$OUT_DIR"
