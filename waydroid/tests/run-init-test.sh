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
cleanup() {
  (cd "$T" && "$DS" --name="$NAME" stop >/dev/null 2>&1) || true
  if mountpoint -q "$T/host-cpuset" 2>/dev/null; then
    rmdir "$T/host-cpuset/ds-waydroid"/* "$T/host-cpuset/ds-waydroid" "$T/host-cpuset"/dswi-host-group-* 2>/dev/null || true
    umount "$T/host-cpuset" || true
  fi
  rm -rf "$T"
}
trap cleanup EXIT

mkdir -p "$R"/{sbin,system/bin,system/etc,dev,proc,sys,tmp,run,data,vendor,mnt,debug_ramdisk,second_stage_resources}
ln -s /system/etc "$R/etc"
ln -s /system/bin/init "$R/init"
cc -O1 -static -o "$R/system/bin/init" "$SCRIPT_DIR/mock-android-init.c"
install -m 0755 "$TOOLS_DIR/ds-waydroid-init" "$R/sbin/init"
CONF="$R/system/etc/droidspaces-waydroid.conf"

# A legacy cpuset hierarchy with a group in it, like the "top-app" group the
# phone's own Android has. The guest must never get to see it.
HOSTCG="$T/host-cpuset"
HOST_GROUP="dswi-host-group-$$"
HAVE_V1_CPUSET=0
mkdir -p "$HOSTCG"
if mount -t cgroup -o cpuset none "$HOSTCG" 2>/dev/null; then
  mkdir -p "$HOSTCG/$HOST_GROUP" && HAVE_V1_CPUSET=1
fi

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
  check "Android does not inherit the starter's file descriptors" has "^extra inherited descriptors: 0$" "$MOCK"
  check "no first-stage call failed" hasnt "first-stage FAIL" "$MOCK"
  check "the host kernel command line is hidden" has "^cmdline: ds_waydroid=1" "$MOCK"
  check "/dev has the nodes Android needs" bash -c "! grep -E 'node /dev/(null|zero|kmsg|random|urandom|ptmx|pts/ptmx|console|socket) +MISSING' '$MOCK'"
  check "/dev is writable for Android" has "open /dev/null for writing: ok" "$MOCK"
  check "the Waydroid composer can create its touch FIFO" has "^Android system user can create a touch FIFO: yes$" "$MOCK"
  check "the Wayland socket directory exists" has "node /run/xdg  *directory" "$MOCK"
  check "properties were generated from the config" has "^prop ro.hardware.gralloc=default" "$MOCK"
  check "bridge settings are not leaked into properties" hasnt "^prop bridge\." "$MOCK"
  check "gpu settings are not leaked into properties" hasnt "^prop gpu\." "$MOCK"
  check "without the GPU driver files Android draws with the CPU" has "^prop ro.hardware.egl=swiftshader$" "$MOCK"
  check "the dma-buf gralloc is only requested in GPU mode" hasnt "^prop gralloc.gbm.legacy=" "$MOCK"
  check "the GPU decision is explained in the boot log" has "^gpu: " "$BOOT"
  check "memfd is enabled by default" has "^prop sys.use_memfd=true$" "$MOCK"
  check "Android cannot change the host's kernel settings" has "^can change kernel settings (/proc/sys): no" "$MOCK"
  check "Android cannot trigger sysrq (kernel crash, reboot)" has "^can trigger sysrq: no" "$MOCK"
  check "Android cannot write to the host's /sys" has "^can write to /sys: no" "$MOCK"
  # Bounding set: sys_module (bit 16) and sys_rawio (bit 17) gone, sys_admin (21) and sys_boot (22) kept.
  check "module loading and raw hardware access are given up" bash -c "
    v=\$(sed -n 's/^CapBnd:[[:space:]]*//p' '$MOCK'); [[ -n \"\$v\" ]] && (( (0x\$v & 0x30000) == 0 ))"
  check "the capabilities Android needs are kept" bash -c "
    v=\$(sed -n 's/^CapBnd:[[:space:]]*//p' '$MOCK'); [[ -n \"\$v\" ]] && (( (0x\$v & 0x600000) == 0x600000 ))"
  if [[ "$HAVE_V1_CPUSET" -eq 1 ]]; then
    check "Android can still mount the cpuset hierarchy" has "^cpuset groups visible:" "$MOCK"
    check "the host's cpuset groups are hidden from Android" hasnt "^cpuset groups visible:.*$HOST_GROUP" "$MOCK"
  fi
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

echo "== hardware access exposes an unusable ashmem node"
mknod -m 000 "$T/unavailable-ashmem" c 1 3
fresh_config
boot --hw-access --bind="$T/unavailable-ashmem:/dev/ashmem"
check "ashmem is visible to Android" has "^node /dev/ashmem  *char device$" "$R/mock-android-init.log"
check "Android still uses memfd by default" has "^prop sys.use_memfd=true$" "$R/mock-android-init.log"

echo "== explicit sys.use_memfd override"
fresh_config "sys.use_memfd=false"
boot
check "an explicit override is kept" has "^prop sys.use_memfd=false$" "$R/mock-android-init.log"
check "the default does not override it" hasnt "^prop sys.use_memfd=true$" "$R/mock-android-init.log"

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

echo "== init.unprotected=1 leaves a privileged container as Droidspaces made it"
fresh_config "init.unprotected=1"
boot --privileged=full
check "the warning is logged" has "init.unprotected=1" "$R/ds-waydroid-boot.log"
# Proof that the "cannot" checks above are not passing by accident. /sys is used
# because some test machines keep /proc/sys read-only themselves.
if [[ -w /sys/kernel ]]; then
  check "/sys is writable again (shows the checks above are real)" has "^can write to /sys: YES" "$R/mock-android-init.log"
fi
if [[ -w /proc/sys/kernel/sysrq ]]; then
  check "kernel settings are writable again" has "^can change kernel settings (/proc/sys): YES" "$R/mock-android-init.log"
else
  echo "  note  this machine keeps /proc/sys read-only itself; that protection is only checked, not contrasted"
fi
if [[ "$HAVE_V1_CPUSET" -eq 1 ]]; then
  check "the host's cpuset groups are visible again" has "^cpuset groups visible:.*$HOST_GROUP" "$R/mock-android-init.log"
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
