#!/usr/bin/env bash
# Test ds-waydroid-init inside a real Droidspaces container on a Linux machine.
#
# A stand-in for Android's init (mock-android-init.c) performs the same calls
# as the Waydroid-patched first stage and fails the same way, so this shows
# whether the environment our init prepares would be accepted by the real one.
#
# The machine running the test usually has no binder driver, so the binder
# step is expected to fail here and is skipped with init.ignore_errors=1.
#
# Usage: sudo run-init-test.sh TOOLS_DIR [DROIDSPACES_BINARY]
#   TOOLS_DIR  output of ../build-tools.sh (native build)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$(cd "${1:?usage: run-init-test.sh TOOLS_DIR [DROIDSPACES_BINARY]}" && pwd)"
DS="${2:-droidspaces}"
command -v "$DS" >/dev/null || { echo "droidspaces binary not found: $DS" >&2; exit 1; }
[[ "$(id -u)" -eq 0 ]] || { echo "run as root" >&2; exit 1; }

# Droidspaces keeps path strings short; work in a short directory.
T="$(mktemp -d /tmp/dswi.XXXXXX)"
R="$T/rootfs"
NAME="dswi$$"
cleanup() { (cd "$T" && "$DS" --name="$NAME" stop >/dev/null 2>&1) || true; rm -rf "$T"; }
trap cleanup EXIT

mkdir -p "$R"/{sbin,system/bin,system/etc,dev,proc,sys,tmp,run,data,vendor,mnt,debug_ramdisk,second_stage_resources}
ln -s /system/etc "$R/etc"
ln -s /system/bin/init "$R/init"
cc -O1 -static -o "$R/system/bin/init" "$SCRIPT_DIR/mock-android-init.c"
install -m 0755 "$TOOLS_DIR/ds-waydroid-init" "$R/sbin/init"
CONF="$R/system/etc/droidspaces-waydroid.conf"

FAILED=0
check() { local what="$1"; shift; if "$@"; then echo "  ok    $what"; else echo "  FAIL  $what"; FAILED=1; fi; }
has() { grep -q -- "$1" "$2" 2>/dev/null; }
hasnt() { ! grep -q -- "$1" "$2" 2>/dev/null; }

boot() { # droidspaces options...
  # The boot report is left in place: our init rotates it itself.
  rm -f "$R"/mock-android-init.log
  local before; before="$(stat -c %Y%N "$R/ds-waydroid-boot.log" 2>/dev/null || echo none)"
  (cd "$T" && timeout 30 "$DS" --name="$NAME" --rootfs="$R" --net=none "$@" start >/dev/null 2>&1) || true
  for _ in $(seq 1 80); do
    if [[ "$(stat -c %Y%N "$R/ds-waydroid-boot.log" 2>/dev/null || echo none)" != "$before" ]] &&
       { grep -q "second-stage report complete" "$R/mock-android-init.log" 2>/dev/null ||
         grep -q "Android was not started" "$R/ds-waydroid-boot.log" 2>/dev/null; }; then
      break
    fi
    sleep 0.1
  done
  (cd "$T" && "$DS" --name="$NAME" stop >/dev/null 2>&1) || true
}

common_checks() {
  local BOOT="$R/ds-waydroid-boot.log" MOCK="$R/mock-android-init.log"
  check "our init ran as PID 1 and handed over to Android" has "starting Android" "$BOOT"
  check "Android stays PID 1" has "second-stage pid=1" "$MOCK"
  check "no first-stage call failed" hasnt "first-stage FAIL" "$MOCK"
  check "the host kernel command line is hidden" has "^cmdline: ds_waydroid=1" "$MOCK"
  check "/dev has the nodes Android needs" bash -c "! grep -E 'node /dev/(null|zero|kmsg|random|urandom|ptmx|pts/ptmx|console|socket) +MISSING' '$MOCK'"
  check "/dev is writable for Android" has "open /dev/null for writing: ok" "$MOCK"
  check "the Wayland socket directory exists" has "node /run/xdg  *directory" "$MOCK"
  check "properties were generated from the config" has "^prop ro.hardware.gralloc=default" "$MOCK"
  check "bridge settings are not leaked into properties" hasnt "^prop bridge\." "$MOCK"
  check "memfd is enabled when ashmem is absent" has "^prop sys.use_memfd=true" "$MOCK"
}

fresh_config() { # extra lines...
  cp "$SCRIPT_DIR/../rootfs/droidspaces-waydroid.conf" "$CONF"
  printf '%s\n' "init.ignore_errors=1" "$@" >> "$CONF"
  rm -f "$R/ds-waydroid-boots" "$R"/ds-waydroid-boot*.log
}

for mode in "" "--hw-access" "--privileged=full"; do
  echo "== Droidspaces ${mode:-default mode}"
  fresh_config
  # shellcheck disable=SC2086
  boot $mode
  common_checks
  check "Android's own first stage is used" has "first-stage result: passed" "$R/mock-android-init.log"
done

echo "== first stage emulated (init.stage=second)"
fresh_config "init.stage=second"
boot
common_checks
check "the second stage is entered directly" has "starting Android: /system/bin/init second_stage" "$R/ds-waydroid-boot.log"
check "the boot time marker is passed on" has "FIRST_STAGE_STARTED_AT=set" "$R/mock-android-init.log"

echo "== without the override, a missing binder driver stops the boot with a report"
if [[ -e /dev/binderfs/binder-control ]] || grep -qw binder /proc/filesystems; then
  echo "  skip  this machine has a binder driver"
else
  cp "$SCRIPT_DIR/../rootfs/droidspaces-waydroid.conf" "$CONF"
  rm -f "$R/ds-waydroid-boots" "$R"/ds-waydroid-boot*.log
  boot
  check "the problem is reported" has "ERROR: binder: cannot mount binderfs" "$R/ds-waydroid-boot.log"
  check "Android is not started" hasnt "starting Android" "$R/ds-waydroid-boot.log"
fi

echo "== boot loop protection"
fresh_config
for _ in 1 2 3 4 5; do boot; done
check "five quick starts are allowed" has "starting Android" "$R/ds-waydroid-boot.log"
boot
check "the sixth start is refused" has "keeps failing to boot" "$R/ds-waydroid-boot.log"
check "the previous report is kept" test -s "$R/ds-waydroid-boot.previous.log"
boot
check "the next start tries again" has "starting Android" "$R/ds-waydroid-boot.log"

if [[ "$FAILED" -ne 0 ]]; then
  echo; echo "Some checks failed. Last reports:"
  cat "$R/ds-waydroid-boot.log" "$R/mock-android-init.log" 2>/dev/null | tail -n 60
  exit 1
fi
echo; echo "All init checks passed."
