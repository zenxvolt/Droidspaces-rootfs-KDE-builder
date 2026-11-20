#!/usr/bin/env bash
# End-to-end test of anland-waydroid-bridge on an ordinary Linux machine.
#
# Runs the real Anland display daemon, a stand-in for the Anland app and a
# stand-in for the Waydroid composer, and checks that pictures and input make
# it through the bridge in both directions, and that sound played by a
# PulseAudio client reaches the app.
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

XDG_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"
wayland-scanner client-header "$XDG_XML" "$T/xdg-shell-client-protocol.h"
wayland-scanner private-code "$XDG_XML" "$T/xdg-shell-protocol.c"

cc -O1 -o "$T/display_daemon" "$ANLAND_SRC/daemon/daemon.c" \
  "$ANLAND_SRC/libdisplay_daemon/display_daemon.c" "$ANLAND_SRC/common/socket_utils.c" -lpthread
cc -O1 -I"$ANLAND_SRC/libdisplay_consumer" -I"$ANLAND_SRC/common" -o "$T/fake-app" \
  "$SCRIPT_DIR/fake-anland-app.c" "$ANLAND_SRC/libdisplay_consumer/display_consumer.c" \
  "$ANLAND_SRC/common/socket_utils.c" -lpthread
cc -O1 -I"$T" -o "$T/fake-hwc" "$SCRIPT_DIR/fake-waydroid-hwc.c" "$T/xdg-shell-protocol.c" \
  $(pkg-config --cflags --libs wayland-client)

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
scenario app-first 720 1280 app_first
check "Android was told the app's screen size" grep -q "hwc: drawing at 720x1280" "$SCENARIO_DIR/hwc.log"
check "only the simple Wayland path is offered" grep -q "subcompositor=0 viewporter=0 dmabuf=0" "$SCENARIO_DIR/hwc.log"
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

# 3. Android starts before the app; the app connects later with another size.
scenario app-late 600 1000 hwc_first --wait_for_display 1 --default_width 480 --default_height 800
check "Android starts at the default size" grep -q "hwc: drawing at 480x800" "$SCENARIO_DIR/hwc.log"
check "Android switches to the app's size" grep -q "hwc: drawing at 600x1000" "$SCENARIO_DIR/hwc.log"
check "picture is correct after the switch" check_picture "$SCENARIO_DIR/frame-40.ppm" 600 1000

# 4. Sound: a PulseAudio client plays through the bridge to the app's speaker.
# aplay's "pulse" device is the same ALSA plugin Waydroid's audio HAL uses.
if command -v aplay >/dev/null && aplay -L 2>/dev/null | grep -qx pulse; then
  d="$T/sound"; mkdir -p "$d/xdg"
  echo "== sound"
  python3 - "$d/tone.raw" <<'PY'
import math, struct, sys
rate = 44100
with open(sys.argv[1], 'wb') as f:
    for i in range(rate * 2):
        v = int(8000 * math.sin(2 * math.pi * 440 * i / rate))
        f.write(struct.pack('<hh', v, v))
PY
  "$T/display_daemon" "$d/display.sock" >"$d/daemon.log" 2>&1 & dpid=$!; PIDS+=("$dpid")
  sleep 0.3
  "$BRIDGE" --anland_socket "$d/display.sock" --runtime_dir "$d/xdg" --log "$d/bridge.log" \
    --pulse_socket "$d/xdg/pulse/native" --verbose 1 >"$d/bridge.out" 2>&1 & bpid=$!; PIDS+=("$bpid")
  sleep 0.3
  "$T/fake-app" "$d/display.sock" 320 480 0 "$d/frame" "$d/received.raw" 4 >"$d/app.log" 2>&1 & apid=$!
  PIDS+=("$apid")
  sleep 1
  start=$(date +%s%N)
  play_rc=0
  PULSE_RUNTIME_PATH="$d/xdg/pulse" HOME="$d" timeout 20 \
    aplay -q -D pulse -t raw -f S16_LE -r 44100 -c 2 "$d/tone.raw" >"$d/aplay.log" 2>&1 || play_rc=$?
  took_ms=$(( ($(date +%s%N) - start) / 1000000 ))
  app_rc=0; wait "$apid" || app_rc=$?
  kill "$bpid" "$dpid" 2>/dev/null || true
  wait "$bpid" "$dpid" 2>/dev/null || true
  check "the ALSA pulse plugin plays through the bridge" test "$play_rc" -eq 0
  check "playback runs in real time (2 s of sound took ${took_ms} ms)" test "$took_ms" -ge 1900 -a "$took_ms" -le 3500
  check "the stand-in app received sound" test "$app_rc" -eq 0
  check "the app's speaker format was used" grep -q "Anland app plays sound at 48000 Hz, 2 channel" "$d/bridge.log"
  check "the tone arrives whole, in pitch and at its volume" python3 - "$d/received.raw" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
n = len(data) // 4
s = struct.unpack('<%dh' % (2 * n), data[:4 * n])
left, right = s[0::2], s[1::2]
assert left == right, "left and right differ"
loud = [i for i, v in enumerate(left) if abs(v) > 100]
assert loud, "only silence arrived"
span = (loud[-1] - loud[0]) / 48000
assert 1.95 <= span <= 2.05, f"tone lasted {span:.3f} s, expected 2 s at 48 kHz"
seg = left[loud[0] + 4800:loud[0] + 4800 + 48000]
crossings = sum(1 for a, b in zip(seg, seg[1:]) if a < 0 <= b)
assert 435 <= crossings <= 445, f"tone is {crossings} Hz, expected 440"
assert 7600 <= max(seg) <= 8400, f"peak is {max(seg)}, expected about 8000"
PY
  SCENARIO_DIR="$d"
else
  echo "== sound"
  echo "  skip  aplay with the ALSA pulse plugin is not installed (alsa-utils, libasound2-plugins)"
fi

if [[ "$FAILED" -ne 0 ]]; then
  echo
  echo "Some checks failed. Logs of the last scenario:"
  tail -n 25 "$SCENARIO_DIR"/*.log "$SCENARIO_DIR/bridge.out" || true
  exit 1
fi
echo
echo "All bridge checks passed."
