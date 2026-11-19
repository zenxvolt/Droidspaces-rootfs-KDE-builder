#!/usr/bin/env bash
# End-to-end test of anland-waydroid-bridge on an ordinary Linux machine.
#
# Runs the real Anland display daemon, a stand-in for the Anland app and a
# stand-in for the Waydroid composer, and checks that pictures and input make
# it through the bridge in both directions.
#
# Usage: run-bridge-test.sh TOOLS_DIR [WORK_DIR]
#   TOOLS_DIR  output of ../build-tools.sh (native build)
#   WORK_DIR   build-tools.sh work directory (default: TOOLS_DIR/../tools-work)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$(cd "${1:?usage: run-bridge-test.sh TOOLS_DIR [WORK_DIR]}" && pwd)"
WORK_DIR="$(cd "${2:-$TOOLS_DIR/../tools-work}" && pwd)"
ANLAND_SRC="$WORK_DIR/src/anland"
BRIDGE="$TOOLS_DIR/anland-waydroid-bridge"
T="$(mktemp -d)"
PIDS=()
cleanup() {
  for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
  if [[ -n "${KEEP_LOGS:-}" ]]; then echo "logs kept in $T"; else rm -rf "$T"; fi
}
trap cleanup EXIT

PROTOCOLS_DIR="$(pkg-config --variable=pkgdatadir wayland-protocols)"
XDG_XML="$PROTOCOLS_DIR/stable/xdg-shell/xdg-shell.xml"
DMABUF_XML="$PROTOCOLS_DIR/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml"
wayland-scanner client-header "$XDG_XML" "$T/xdg-shell-client-protocol.h"
wayland-scanner private-code "$XDG_XML" "$T/xdg-shell-protocol.c"
wayland-scanner client-header "$DMABUF_XML" "$T/linux-dmabuf-unstable-v1-client-protocol.h"
wayland-scanner private-code "$DMABUF_XML" "$T/linux-dmabuf-unstable-v1-protocol.c"

cc -O1 -o "$T/display_daemon" "$ANLAND_SRC/daemon/daemon.c" \
  "$ANLAND_SRC/libdisplay_daemon/display_daemon.c" "$ANLAND_SRC/common/socket_utils.c" -lpthread
cc -O1 -I"$ANLAND_SRC/libdisplay_consumer" -I"$ANLAND_SRC/common" -o "$T/fake-app" \
  "$SCRIPT_DIR/fake-anland-app.c" "$ANLAND_SRC/libdisplay_consumer/display_consumer.c" \
  "$ANLAND_SRC/common/socket_utils.c" -lpthread
cc -O1 -I"$T" -o "$T/fake-hwc" "$SCRIPT_DIR/fake-waydroid-hwc.c" "$T/xdg-shell-protocol.c" \
  "$T/linux-dmabuf-unstable-v1-protocol.c" \
  $(pkg-config --cflags --libs wayland-client)

cc -O1 -I"$SCRIPT_DIR/../src" -o "$T/fake-gpu-copy" "$SCRIPT_DIR/fake-gpu-copy.c"

FAILED=0
check() { # description, command...
  local what="$1"; shift
  if "$@"; then echo "  ok    $what"; else echo "  FAIL  $what"; FAILED=1; fi
}

# check_picture FILE WIDTH HEIGHT : the gradient the fake composer draws
check_picture() {
  python3 - "$1" "$2" "$3" <<'PY'
import sys
path, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
data = open(path, 'rb').read()
header, rest = data.split(b'\n255\n', 1)
pw, ph = map(int, header.split(b'\n')[1].split())
assert (pw, ph) == (w, h), f"picture is {pw}x{ph}, expected {w}x{h}"
def px(x, y):
    o = (y * w + x) * 3
    return rest[o], rest[o + 1], rest[o + 2]
for fx, fy in ((0.1, 0.1), (0.5, 0.25), (0.9, 0.8), (0.33, 0.66)):
    x, y = int(w * fx), int(h * fy)
    r, g, b = px(x, y)
    er, eg = x * 255 // w, y * 255 // h
    assert abs(r - er) <= 3 and abs(g - eg) <= 3 and b == 200, \
        f"pixel {x},{y} is {(r, g, b)}, expected about {(er, eg, 200)}"
PY
}

scenario() { # name, app WxH, bridge args..., then: app_first|hwc_first
  local name="$1" aw="$2" ah="$3" order="$4"; shift 4
  local d="$T/$name"; mkdir -p "$d/xdg"
  echo "== $name"
  "$T/display_daemon" "$d/display.sock" >"$d/daemon.log" 2>&1 & local dpid=$!; PIDS+=("$dpid")
  sleep 0.3
  "$BRIDGE" --anland_socket "$d/display.sock" --runtime_dir "$d/xdg" --log "$d/bridge.log" \
    --verbose 1 "$@" >"$d/bridge.out" 2>&1 & local bpid=$!; PIDS+=("$bpid")
  sleep 0.3
  local apid hpid
  if [[ "$order" == app_first ]]; then
    "$T/fake-app" "$d/display.sock" "$aw" "$ah" 40 "$d/frame" >"$d/app.log" 2>&1 & apid=$!
    sleep 0.5
    XDG_RUNTIME_DIR="$d/xdg" WAYLAND_DISPLAY=wayland-0 "$T/fake-hwc" 150 >"$d/hwc.log" 2>&1 & hpid=$!
  else
    XDG_RUNTIME_DIR="$d/xdg" WAYLAND_DISPLAY=wayland-0 "$T/fake-hwc" 260 >"$d/hwc.log" 2>&1 & hpid=$!
    sleep 2.5
    "$T/fake-app" "$d/display.sock" "$aw" "$ah" 40 "$d/frame" >"$d/app.log" 2>&1 & apid=$!
  fi
  PIDS+=("$apid" "$hpid")
  local app_rc=0 hwc_rc=0
  wait "$apid" || app_rc=$?
  wait "$hpid" || hwc_rc=$?
  check "the stand-in app received all its frames" test "$app_rc" -eq 0
  check "the stand-in composer finished cleanly" test "$hwc_rc" -eq 0
  check "the bridge is still running" kill -0 "$bpid"
  kill "$bpid" "$dpid" 2>/dev/null || true
  wait "$bpid" "$dpid" 2>/dev/null || true
  SCENARIO_DIR="$d"
}

# 1. The app is already open when Android starts.
scenario app-first 720 1280 app_first --render_scale 100
check "Android was told the app's screen size" grep -q "hwc: drawing at 720x1280" "$SCENARIO_DIR/hwc.log"
check "only the single-surface Wayland path is offered" grep -q "subcompositor=0 viewporter=0 dmabuf=1" "$SCENARIO_DIR/hwc.log"
check "picture arrives with correct colours" check_picture "$SCENARIO_DIR/frame-40.ppm" 720 1280
check "touch reaches Android at the right place" grep -q "hwc: touch down id=0 x=360 y=320 surface=ok" "$SCENARIO_DIR/hwc.log"
check "touch release reaches Android" grep -q "hwc: touch up id=0" "$SCENARIO_DIR/hwc.log"
check "key press reaches Android" grep -q "hwc: key 30 down" "$SCENARIO_DIR/hwc.log"
check "input survives an ignored clipboard message" grep -q "hwc: key 48 down" "$SCENARIO_DIR/hwc.log"
check "keyboard focus is given to Android" grep -q "hwc: keyboard enter surface=ok" "$SCENARIO_DIR/hwc.log"

# 2. Android renders at half resolution and the bridge scales it up.
scenario half-scale 720 1280 app_first --render_scale 50
check "Android was told half the screen size" grep -q "hwc: drawing at 360x640" "$SCENARIO_DIR/hwc.log"
check "scaled picture fills the screen correctly" check_picture "$SCENARIO_DIR/frame-40.ppm" 720 1280
check "touch is scaled to Android's coordinates" grep -q "hwc: touch down id=0 x=180 y=160 surface=ok" "$SCENARIO_DIR/hwc.log"

# 2b. A scale that is not a whole number uses the general scaling path.
scenario three-quarter-scale 720 1280 app_first --render_scale 75
check "Android was told three quarters of the screen size" grep -q "hwc: drawing at 540x960" "$SCENARIO_DIR/hwc.log"
check "scaled picture fills the screen correctly" check_picture "$SCENARIO_DIR/frame-40.ppm" 720 1280
check "touch is scaled to Android's coordinates" grep -q "hwc: touch down id=0 x=270 y=240 surface=ok" "$SCENARIO_DIR/hwc.log"

# 2c. GPU mode: frames arrive as linear dma-bufs with padded rows.
FAKE_HWC_DMABUF=1 scenario gpu-dmabuf 720 1280 app_first --render_scale 100
check "the composer used dma-buf buffers" grep -q "hwc: GPU mode" "$SCENARIO_DIR/hwc.log"
check "the bridge saw a GPU frame" grep -q "dmabuf format 0x34324241, drawn by the GPU" "$SCENARIO_DIR/bridge.log"
check "GPU picture arrives with correct colours" check_picture "$SCENARIO_DIR/frame-40.ppm" 720 1280

# 2d. GPU mode with the GPU copy helper (a CPU stand-in here) and render fences.
FAKE_HWC_DMABUF=1 scenario gpu-copy 720 1280 app_first --render_scale 50 --gpu_copy 1 \
  --gpu_copy_helper "$T/fake-gpu-copy"
check "the GPU copy helper started" grep -q "GPU copy: Android's frames are copied to Anland by the GPU" "$SCENARIO_DIR/bridge.log"
check "frames were copied by the helper" grep -q "presented the first Android frame to Anland (copied by the GPU)" "$SCENARIO_DIR/bridge.log"
check "the helper's picture is correct" check_picture "$SCENARIO_DIR/frame-40.ppm" 720 1280
check "render fences reach the Anland app" grep -qE "received 40 frames, [1-9][0-9]* with a render fence" "$SCENARIO_DIR/app.log"
check "buffers Android drops are dropped by the helper too" grep -q "fake-gpu-copy: forgot source" "$SCENARIO_DIR/bridge.out"
check "the helper never fell back" bash -c "! grep -q 'GPU copy turned off' '$SCENARIO_DIR/bridge.log'"

# 2e. The helper fails in the middle; the CPU takes over without a gap.
FAKE_HWC_DMABUF=1 FAKE_GPU_COPY_FAIL=copy scenario gpu-copy-fails 720 1280 app_first \
  --render_scale 100 --gpu_copy 1 --gpu_copy_helper "$T/fake-gpu-copy"
check "the failure is reported" grep -q "GPU copy turned off (pretend the GPU hung)" "$SCENARIO_DIR/bridge.log"
check "the CPU continues with a correct picture" check_picture "$SCENARIO_DIR/frame-40.ppm" 720 1280

# 2f. No helper installed: the CPU copies from the start.
FAKE_HWC_DMABUF=1 scenario gpu-copy-missing 720 1280 app_first --render_scale 100 --gpu_copy 1 \
  --gpu_copy_helper /nonexistent/anland-gpu-copy
check "the missing helper is reported" grep -q "GPU copy turned off (/nonexistent/anland-gpu-copy is missing)" "$SCENARIO_DIR/bridge.log"
check "the CPU copies with a correct picture" check_picture "$SCENARIO_DIR/frame-40.ppm" 720 1280
check "frames were copied by the CPU" grep -q "presented the first Android frame to Anland (copied by the CPU)" "$SCENARIO_DIR/bridge.log"

# 2h. The real anland-gpu-copy, on Mesa's software renderer (llvmpipe imports
# the memfd "dma-bufs" by mapping them). Checks EGL set-up, buffer import,
# scaling, colour order and orientation of the actual helper.
if pkg-config --exists egl glesv2; then
  cc -O1 -I"$SCRIPT_DIR/../src" -o "$T/anland-gpu-copy" "$SCRIPT_DIR/../gpu/anland-gpu-copy.c" \
    $(pkg-config --cflags --libs egl glesv2)
  EGL_PLATFORM=surfaceless FAKE_HWC_DMABUF=1 scenario real-gpu-copy 720 1280 app_first \
    --render_scale 50 --gpu_copy 1 --gpu_copy_helper "$T/anland-gpu-copy"
  check "the real helper started" grep -q "GPU copy: Android's frames are copied to Anland by the GPU" "$SCENARIO_DIR/bridge.log"
  check "frames were copied by the real helper" grep -q "presented the first Android frame to Anland (copied by the GPU)" "$SCENARIO_DIR/bridge.log"
  check "the real helper's picture is correct" check_picture "$SCENARIO_DIR/frame-40.ppm" 720 1280
  check "the real helper never fell back" bash -c "! grep -q 'GPU copy turned off' '$SCENARIO_DIR/bridge.log'"
else
  echo "== real-gpu-copy skipped (no EGL/GLES development files)"
fi

# 2g. "auto" settings follow the drawing mode.
scenario auto-cpu 720 1280 app_first --render_scale auto --max_fps auto --gpu 0
check "auto uses half the resolution when the CPU draws" grep -q "hwc: drawing at 360x640" "$SCENARIO_DIR/hwc.log"
check "auto limits to 30 fps when the CPU draws" grep -q "scale 50%, max 30 fps" "$SCENARIO_DIR/bridge.log"
FAKE_HWC_DMABUF=1 scenario auto-gpu 720 1280 app_first --render_scale auto --max_fps auto --gpu 1
check "auto uses the full resolution with the GPU" grep -q "hwc: drawing at 720x1280" "$SCENARIO_DIR/hwc.log"
check "auto allows 60 fps with the GPU" grep -q "scale 100%, max 60 fps" "$SCENARIO_DIR/bridge.log"

# 3. Android starts before the app; the app connects later with another size.
scenario app-late 600 1000 hwc_first --render_scale 100 --wait_for_display 1 --default_width 480 --default_height 800
check "Android starts at the default size" grep -q "hwc: drawing at 480x800" "$SCENARIO_DIR/hwc.log"
check "Android switches to the app's size" grep -q "hwc: drawing at 600x1000" "$SCENARIO_DIR/hwc.log"
check "picture is correct after the switch" check_picture "$SCENARIO_DIR/frame-40.ppm" 600 1000

# Keep asking for frames for longer than Anland's five-second timeout, first
# without a Wayland client, then with just one Android frame on screen.
d="$T/idle"; mkdir -p "$d/xdg"
echo "== idle"
"$T/display_daemon" "$d/display.sock" >"$d/daemon.log" 2>&1 & PIDS+=("$!")
"$BRIDGE" --anland_socket "$d/display.sock" --runtime_dir "$d/xdg" --log "$d/bridge.log" \
  --render_scale 100 --max_fps 30 >"$d/bridge.out" 2>&1 & PIDS+=("$!")
timeout 25 "$T/fake-app" "$d/display.sock" 160 240 385 "$d/frame" idle >"$d/app.log" 2>&1 &
app_pid=$!; PIDS+=("$app_pid")
sleep 6
XDG_RUNTIME_DIR="$d/xdg" WAYLAND_DISPLAY=wayland-0 "$T/fake-hwc" 1 25 >"$d/hwc.log" 2>&1 &
hwc_pid=$!; PIDS+=("$hwc_pid")
app_rc=0
wait "$app_pid" || app_rc=$?
check "Anland stayed connected through startup and an idle screen" test "$app_rc" -eq 0
check "the first Anland buffer is black before Android draws" python3 - "$d/frame-1.ppm" <<'PY'
import pathlib
import sys

assert pathlib.Path(sys.argv[1]).read_bytes() == b'P6\n160 240\n255\n' + bytes(160 * 240 * 3)
PY
for frame in 383 384 385; do
  check "the last Android frame survives in buffer $(((frame - 1) % 3))" check_picture "$d/frame-$frame.ppm" 160 240
done
SCENARIO_DIR="$d"

if [[ "$FAILED" -ne 0 ]]; then
  echo
  echo "Some checks failed. Logs of the last scenario:"
  tail -n 25 "$SCENARIO_DIR"/*.log "$SCENARIO_DIR/bridge.out" || true
  exit 1
fi
echo
echo "All bridge checks passed."
