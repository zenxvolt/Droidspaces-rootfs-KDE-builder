#!/usr/bin/env bash
# End-to-end test of phoc-anland on a phone (needs a working GPU render node).
#
# Runs the real Anland display daemon on a private socket, a stand-in for the
# Anland app that offers GPU buffers, and phoc-anland with a small Wayland
# client that draws a gradient and logs its input. It checks that the picture
# reaches the "app" with the right colours, that touch and keys reach the
# client, and that phoc shuts down cleanly. The Anland app's real socket is
# never touched, so a running Anland session is not disturbed.
#
# Usage: run-phoc-anland-test.sh PHOC_ANLAND ANLAND_SOURCE_DIR
#   PHOC_ANLAND        the phoc-anland binary (e.g. /usr/libexec/phoc-anland)
#   ANLAND_SOURCE_DIR  an Anland checkout (build-phoc-anland.sh's WORK_DIR/anland)
#
# Needs: gcc, pkg-config, wayland-scanner, wayland-protocols, the GBM and
# wayland-client headers, the phoc GSettings schema, and a render node
# (ANLAND_DRM_DEVICE, default /dev/dri/renderD128).
# KEEP_LOGS=1 keeps the logs and pictures.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
PHOC="$(realpath "${1:?usage: run-phoc-anland-test.sh PHOC_ANLAND ANLAND_SOURCE_DIR}")"
ANLAND_SRC="$(cd "${2:?usage: run-phoc-anland-test.sh PHOC_ANLAND ANLAND_SOURCE_DIR}" && pwd)"
NODE="${ANLAND_DRM_DEVICE:-/dev/dri/renderD128}"
WIDTH=720 HEIGHT=1280

# Socket paths must stay short (108 bytes), so do not nest this deeply.
T="$(mktemp -d "${TMPDIR:-/tmp}/phoc-anland-test.XXXXXX")"
PIDS=()
cleanup() {
  for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
  if [[ -n "${KEEP_LOGS:-}" ]]; then echo "logs kept in $T"; else rm -rf "$T"; fi
}
trap cleanup EXIT

[[ -x "$PHOC" ]] || { echo "not executable: $PHOC" >&2; exit 2; }
[[ -r "$NODE" && -w "$NODE" ]] || { echo "render node unavailable: $NODE" >&2; exit 2; }

echo "== building the test programs"
XDG_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"
wayland-scanner client-header "$XDG_XML" "$T/xdg-shell-client-protocol.h"
wayland-scanner private-code "$XDG_XML" "$T/xdg-shell-protocol.c"
"${CC:-cc}" ${CFLAGS:-} -O1 -o "$T/display_daemon" "$ANLAND_SRC/daemon/daemon.c" \
  "$ANLAND_SRC/libdisplay_daemon/display_daemon.c" "$ANLAND_SRC/common/socket_utils.c" \
  -I"$ANLAND_SRC/common" -I"$ANLAND_SRC/libdisplay_daemon" -lpthread
"${CC:-cc}" ${CFLAGS:-} -O1 -Wno-address-of-packed-member -I"$ANLAND_SRC/libdisplay_consumer" -I"$ANLAND_SRC/common" \
  -o "$T/fake-app" "$SCRIPT_DIR/fake-anland-gbm-app.c" \
  "$ANLAND_SRC/libdisplay_consumer/display_consumer.c" "$ANLAND_SRC/common/socket_utils.c" \
  $(pkg-config --cflags --libs gbm) -lpthread
"${CC:-cc}" ${CFLAGS:-} -O1 -I"$T" -o "$T/client" "$REPO_DIR/waydroid/tests/fake-waydroid-hwc.c" \
  "$T/xdg-shell-protocol.c" $(pkg-config --cflags --libs wayland-client)

FAILED=0
check() { # description, command...
  local what="$1"; shift
  if "$@"; then echo "  ok    $what"; else echo "  FAIL  $what"; FAILED=1; fi
}

# check_picture FILE : the gradient the client draws, scaled to the screen
check_picture() {
  python3 - "$1" "$WIDTH" "$HEIGHT" <<'PY'
import sys
path, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
header, rest = open(path, 'rb').read().split(b'\n255\n', 1)
assert tuple(map(int, header.split(b'\n')[1].split())) == (w, h), "wrong picture size"
for fx, fy in ((0.1, 0.1), (0.5, 0.25), (0.9, 0.8), (0.33, 0.66)):
    x, y = int(w * fx), int(h * fy)
    o = (y * w + x) * 3
    r, g, b = rest[o], rest[o + 1], rest[o + 2]
    er, eg = x * 255 // w, y * 255 // h
    assert abs(r - er) <= 3 and abs(g - eg) <= 3 and abs(b - 200) <= 3, \
        f"pixel {x},{y} is {(r, g, b)}, expected about {(er, eg, 200)}"
PY
}

# Same rule as the phosh-anland launcher: the kgsl driver reports itself as
# a software EGL device even on the GPU.
if [[ ${MESA_LOADER_DRIVER_OVERRIDE:-} == kgsl || ${GALLIUM_DRIVER:-} == kgsl ]]; then
  export WLR_RENDERER_ALLOW_SOFTWARE="${WLR_RENDERER_ALLOW_SOFTWARE:-1}"
fi

mkdir -p "$T/xdg" && chmod 700 "$T/xdg"
printf '[core]\nxwayland=false\n' > "$T/phoc.ini"

echo "== phoc-anland shows a client in the Anland app"
"$T/display_daemon" "$T/display.sock" > "$T/daemon.log" 2>&1 & DPID=$!; PIDS+=("$DPID")
sleep 0.5
# The "app" is open first, as on a phone; it waits up to 600 frames for a picture.
"$T/fake-app" "$NODE" "$T/display.sock" "$WIDTH" "$HEIGHT" 600 "$T/frame" > "$T/app.log" 2>&1 & APID=$!
PIDS+=("$APID")
sleep 1
env -u WAYLAND_DISPLAY -u DISPLAY XDG_RUNTIME_DIR="$T/xdg" ANLAND_SOCKET="$T/display.sock" \
  ANLAND_DRM_DEVICE="$NODE" PHOC_ANLAND_SCALE= GSETTINGS_BACKEND=memory \
  "$PHOC" -C "$T/phoc.ini" -E "sh -c 'sleep 2; exec $T/client 4000'" > "$T/phoc.log" 2>&1 & PPID_=$!
PIDS+=("$PPID_")

app_rc=0
wait "$APID" || app_rc=$?
check "the stand-in app received frames with a picture" test "$app_rc" -eq 0
check "phoc picked the Anland backend" grep -q "Using the Anland backend" "$T/phoc.log"
check "the client got the app's screen size" grep -q "hwc: output mode ${WIDTH}x${HEIGHT}" "$T/phoc.log"
check "the scale fits Phosh's 360 px width" grep -q "hwc: configure 360x640" "$T/phoc.log"
check "Anland touch and keyboard reach the seat" grep -q "hwc: seat capabilities touch=1 keyboard=1" "$T/phoc.log"
picture="$(ls "$T"/frame-*.ppm 2>/dev/null | sort -t- -k2 -n | tail -n1 || true)"
check "the picture arrives with the right colours" check_picture "${picture:-/nonexistent}"
check "touch lands in the middle of the client" grep -q "hwc: touch down id=0 x=180 y=320 surface=ok" "$T/phoc.log"
check "touch release reaches the client" grep -q "hwc: touch up id=0" "$T/phoc.log"
check "a key press reaches the client" grep -q "hwc: key 30 down" "$T/phoc.log"

kill -TERM "$PPID_" 2>/dev/null || true
phoc_rc=0
wait "$PPID_" || phoc_rc=$?
check "phoc shuts down cleanly" test "$phoc_rc" -eq 0
kill "$DPID" 2>/dev/null || true

if ((FAILED)); then
  echo "SOME CHECKS FAILED (KEEP_LOGS=1 keeps $T)"
  exit 1
fi
echo "all checks passed"
