#!/usr/bin/env bash
# Build a Droidspaces rootfs that IS Waydroid's Android image.
#
# The result is a .tar.xz that Droidspaces imports like any other rootfs. It
# boots Android directly (no Linux distribution, no LXC, no nested container)
# and shows it through the Anland app.
#
#   system.img + vendor.img  ->  one directory tree
#   + /sbin/init                          (ds-waydroid-init, prepares the container)
#   + /system/bin/anland-waydroid-bridge  (display bridge to Anland)
#   + /system/etc/droidspaces-waydroid.conf
#   + Adreno GPU drivers in /vendor          (with -G, from build-gpu-drivers.sh)
#   + /system/bin/ds-waydroid-diag        (network and sound report for bug reports)
#
# Must run as root on Linux (it loop-mounts the Android images).
#
# Usage: build-waydroid-rootfs.sh [options]
#   -t TYPE     Waydroid system flavour: VANILLA (default) or GAPPS
#   -a ARCH     waydroid_arm64_only (default; runs on every ARM64 phone) or
#               waydroid_arm64 (adds 32-bit app support; only for processors
#               that still run 32-bit code)
#   -S FILE     use this system image (.zip or .img) instead of downloading
#   -V FILE     use this vendor image (.zip or .img) instead of downloading
#   -T DIR      directory with the prebuilt tools (default: build them)
#   -G DIR      add the Adreno GPU drivers from DIR (one or more output
#               directories of build-gpu-drivers.sh, merged; DIR/vendor/...)
#   -o DIR      output directory (default: ./out)
#   -c LEVEL    xz compression level 0-9 (default 6)
#   -k          keep the unpacked rootfs directory
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SYSTEM_TYPE="VANILLA"
OTA_ARCH="waydroid_arm64_only"
VENDOR_TYPE="MAINLINE"
SYSTEM_FILE=""
VENDOR_FILE=""
TOOLS_DIR=""
GPU_DIRS=()
OUT_DIR="$SCRIPT_DIR/out"
XZ_LEVEL=6
KEEP_ROOTFS=false

: "${WAYDROID_OTA_URL:=https://ota.waydro.id}"
: "${WAYDROID_OTA_FALLBACK_URL:=https://raw.githubusercontent.com/waydroid/OTA/master}"

while getopts "t:a:S:V:T:G:o:c:kh" opt; do
  case "$opt" in
    t) SYSTEM_TYPE="$OPTARG" ;;
    a) OTA_ARCH="$OPTARG" ;;
    S) SYSTEM_FILE="$OPTARG" ;;
    V) VENDOR_FILE="$OPTARG" ;;
    T) TOOLS_DIR="$OPTARG" ;;
    G) GPU_DIRS+=("$OPTARG") ;;
    o) OUT_DIR="$OPTARG" ;;
    c) XZ_LEVEL="$OPTARG" ;;
    k) KEEP_ROOTFS=true ;;
    *) sed -n '2,29p' "$0"; exit 1 ;;
  esac
done

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ "$(id -u)" -eq 0 ]] || die "run this script as root (sudo): it has to mount the Android images"
case "$SYSTEM_TYPE" in VANILLA|GAPPS) ;; *) die "-t must be VANILLA or GAPPS" ;; esac
case "$OTA_ARCH" in waydroid_arm64|waydroid_arm64_only) ;; *) die "-a must be waydroid_arm64 or waydroid_arm64_only" ;; esac
[[ "$XZ_LEVEL" =~ ^[0-9]$ ]] || die "-c must be a digit from 0 to 9"
for tool in curl unzip tar xz python3 mount sha256sum; do
  command -v "$tool" >/dev/null || die "missing tool: $tool"
done

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
WORK="$OUT_DIR/work"
ROOTFS="$WORK/rootfs"
MNT="$WORK/mnt"
mkdir -p "$WORK/downloads"

cleanup() {
  mountpoint -q "$MNT" 2>/dev/null && umount "$MNT" || true
  if [[ "$KEEP_ROOTFS" != true ]]; then rm -rf "$ROOTFS"; fi
  rm -rf "$WORK/images"
}
trap cleanup EXIT

# --- 1. find and download the images ----------------------------------------
# Prints "filename url sha256" of the newest image in a Waydroid OTA index.
latest_image() { # index path, e.g. system/lineage/waydroid_arm64/VANILLA.json
  local index="$1" json=""
  json="$(curl -fsSL --retry 3 --max-time 60 "$WAYDROID_OTA_URL/$index" 2>/dev/null)" \
    || json="$(curl -fsSL --retry 3 --max-time 60 "$WAYDROID_OTA_FALLBACK_URL/$index")" \
    || die "cannot read the Waydroid image index ($index)"
  python3 -c '
import json, sys
entries = json.load(sys.stdin)["response"]
newest = max(entries, key=lambda e: e["datetime"])
print(newest["filename"], newest["url"], newest["id"])
' <<<"$json"
}

download() { # url, sha256, destination
  local url="$1" sum="$2" dest="$3"
  if [[ -f "$dest" ]] && echo "$sum  $dest" | sha256sum -c --status 2>/dev/null; then
    echo "already downloaded: $(basename "$dest")"
    return
  fi
  # SourceForge "…/files/<path>/download" pages redirect browsers; the plain
  # download host serves the same file to command-line tools.
  if [[ "$url" =~ ^https://sourceforge\.net/projects/([^/]+)/files/(.+)/download$ ]]; then
    url="https://downloads.sourceforge.net/project/${BASH_REMATCH[1]}/${BASH_REMATCH[2]}"
  fi
  echo "downloading $url"

  # Every request is redirected to a mirror picked at random, and some mirrors
  # deliver only a few hundred kB/s. Many parallel connections spread the file
  # over several mirrors; connections that crawl are dropped and retried.
  if command -v aria2c >/dev/null; then
    if aria2c --dir="$(dirname "$dest")" --out="$(basename "$dest").part" \
         --max-connection-per-server=16 --split=16 --min-split-size=4M \
         --lowest-speed-limit=100K --timeout=30 --connect-timeout=20 \
         --max-tries=40 --retry-wait=2 --max-file-not-found=5 \
         --file-allocation=none --allow-overwrite=true --auto-file-renaming=false \
         --console-log-level=warn --summary-interval=30 --download-result=hide \
         "$url" && echo "$sum  $dest.part" | sha256sum -c --status; then
      rm -f "$dest.part.aria2"
      mv "$dest.part" "$dest"
      return
    fi
    echo "aria2c did not deliver a correct file, trying curl" >&2
    rm -f "$dest.part" "$dest.part.aria2"
  fi

  # Single connection: give up on a slow mirror and resume on the next one.
  # The speed floor is dropped after a few rounds in case every mirror is slow.
  local attempt
  local -a floor
  for attempt in $(seq 1 12); do
    floor=()
    [[ "$attempt" -le 8 ]] && floor=(--speed-limit 400000 --speed-time 20)
    if curl -fL -C - --connect-timeout 20 "${floor[@]}" -o "$dest.part" "$url"; then
      if echo "$sum  $dest.part" | sha256sum -c --status; then
        mv "$dest.part" "$dest"
        return
      fi
      echo "checksum mismatch, starting over" >&2
      rm -f "$dest.part"
    fi
    sleep 2
  done
  die "download failed or checksum mismatch: $url"
}

SYSTEM_NAME=""; VENDOR_NAME=""
if [[ -z "$SYSTEM_FILE" ]]; then
  log "Looking up the newest Waydroid system image ($OTA_ARCH, $SYSTEM_TYPE)"
  read -r SYSTEM_NAME url sum < <(latest_image "system/lineage/$OTA_ARCH/$SYSTEM_TYPE.json")
  SYSTEM_FILE="$WORK/downloads/$SYSTEM_NAME"
  download "$url" "$sum" "$SYSTEM_FILE"
else
  SYSTEM_NAME="$(basename "$SYSTEM_FILE")"
fi
if [[ -z "$VENDOR_FILE" ]]; then
  log "Looking up the newest Waydroid vendor image ($OTA_ARCH, $VENDOR_TYPE)"
  read -r VENDOR_NAME url sum < <(latest_image "vendor/$OTA_ARCH/$VENDOR_TYPE.json")
  VENDOR_FILE="$WORK/downloads/$VENDOR_NAME"
  download "$url" "$sum" "$VENDOR_FILE"
else
  VENDOR_NAME="$(basename "$VENDOR_FILE")"
fi
[[ -f "$SYSTEM_FILE" ]] || die "system image not found: $SYSTEM_FILE"
[[ -f "$VENDOR_FILE" ]] || die "vendor image not found: $VENDOR_FILE"

# --- 2. helper programs ------------------------------------------------------
if [[ -z "$TOOLS_DIR" ]]; then
  log "Building the helper programs"
  "$SCRIPT_DIR/build-tools.sh" -o "$WORK/tools" -w "$WORK/tools-work"
  TOOLS_DIR="$WORK/tools"
fi
for tool in ds-waydroid-init anland-waydroid-bridge; do
  [[ -x "$TOOLS_DIR/$tool" ]] || die "missing helper program: $TOOLS_DIR/$tool"
  file -b "$TOOLS_DIR/$tool" 2>/dev/null | grep -q "aarch64" \
    || echo "warning: $tool is not an ARM64 program; this rootfs will only run on its own architecture" >&2
done

# --- 3. unpack the images into one tree -------------------------------------
image_path() { # zip-or-img, wanted name -> path of the .img
  local src="$1" name="$2"
  case "$src" in
    *.zip)
      mkdir -p "$WORK/images"
      unzip -o -q "$src" "$name" -d "$WORK/images"
      echo "$WORK/images/$name" ;;
    *) echo "$src" ;;
  esac
}

# Extended attributes are left out. Android's SELinux labels would only confuse
# the host (SELinux is off inside Waydroid), and the Droidspaces app unpacks
# with BusyBox tar, which is happiest with a plain archive.
TAR_XATTRS=()

copy_image() { # image, destination directory
  local img="$1" dest="$2"
  mkdir -p "$MNT" "$dest"
  mount -o loop,ro "$img" "$MNT"
  tar -C "$MNT" --numeric-owner "${TAR_XATTRS[@]}" --exclude=./lost+found -cf - . |
    tar -C "$dest" --numeric-owner "${TAR_XATTRS[@]}" -xpf -
  umount "$MNT"
}

rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"

log "Unpacking the system image"
SYSTEM_IMG="$(image_path "$SYSTEM_FILE" system.img)"
copy_image "$SYSTEM_IMG" "$ROOTFS"
[[ "$SYSTEM_IMG" == "$WORK/images/"* ]] && rm -f "$SYSTEM_IMG"

log "Unpacking the vendor image"
VENDOR_IMG="$(image_path "$VENDOR_FILE" vendor.img)"
copy_image "$VENDOR_IMG" "$ROOTFS/vendor"
[[ "$VENDOR_IMG" == "$WORK/images/"* ]] && rm -f "$VENDOR_IMG"

[[ -x "$ROOTFS/system/bin/init" ]] || die "the system image has no /system/bin/init"
[[ -f "$ROOTFS/vendor/lib64/hw/hwcomposer.waydroid.so" || -f "$ROOTFS/vendor/lib/hw/hwcomposer.waydroid.so" ]] \
  || die "the vendor image has no Waydroid hardware composer"

# --- 4. add the Droidspaces pieces ------------------------------------------
log "Installing the Droidspaces integration"

# The Droidspaces app only installs archives that look like a Linux rootfs:
# bin, sbin, etc and usr must exist at the top. Android has no /usr.
install -d -m 0755 -o 0 -g 0 "$ROOTFS/usr"

# /etc, /bin, ... are absolute links into /system in the Android image. Tools
# that work on the unpacked tree from outside (the app's post-install script
# writes to <rootfs>/etc) would follow them into the host's own /system. A
# relative link points to the same place inside Android and stays inside the
# tree outside of it.
for entry in "$ROOTFS"/*; do
  [[ -L "$entry" ]] || continue
  target="$(readlink "$entry")"
  if [[ "$target" == /system/* ]]; then
    ln -sfn "${target#/}" "$entry"
    chown -h 0:0 "$entry"
  fi
done

# Marks the app's Linux-specific post-install fixes as already done; none of
# them apply to Android.
echo "Waydroid rootfs: the Linux post-extraction fixes do not apply" > "$ROOTFS/system/etc/droidspaces"
chmod 0644 "$ROOTFS/system/etc/droidspaces"
install -d -m 0755 -o 0 -g 0 "$ROOTFS/sbin"
install -m 0755 -o 0 -g 0 "$TOOLS_DIR/ds-waydroid-init" "$ROOTFS/sbin/init"
install -m 0755 -o 0 -g 2000 "$TOOLS_DIR/anland-waydroid-bridge" "$ROOTFS/system/bin/anland-waydroid-bridge"
install -m 0755 -o 0 -g 2000 "$SCRIPT_DIR/rootfs/ds-waydroid-benchmark" "$ROOTFS/system/bin/ds-waydroid-benchmark"
install -m 0755 -o 0 -g 2000 "$SCRIPT_DIR/rootfs/ds-waydroid-diag" "$ROOTFS/system/bin/ds-waydroid-diag"
install -m 0644 -o 0 -g 0 "$SCRIPT_DIR/rootfs/anland-bridge.rc" "$ROOTFS/system/etc/init/anland-bridge.rc"
install -m 0644 -o 0 -g 0 "$SCRIPT_DIR/rootfs/ds-logcat.rc" "$ROOTFS/system/etc/init/ds-logcat.rc"
install -m 0644 -o 0 -g 0 "$SCRIPT_DIR/rootfs/ds-gpu-guard.rc" "$ROOTFS/system/etc/init/ds-gpu-guard.rc"
install -m 0755 -o 0 -g 0 "$SCRIPT_DIR/rootfs/ds-gpu-guard.sh" "$ROOTFS/system/etc/ds-gpu-guard.sh"
install -m 0644 -o 0 -g 0 "$SCRIPT_DIR/rootfs/droidspaces-waydroid.conf" "$ROOTFS/system/etc/droidspaces-waydroid.conf"

# LineageOS registers a physical button shortcut for bugreport. Android init
# then opens every entry in /dev/input in blocking mode. Waydroid provides
# FIFOs here, so init can hang waiting for a writer before it starts Android.
LINEAGE_INIT="$ROOTFS/system/system_ext/etc/init/init.lineage-system_ext.rc"
if [[ -f "$LINEAGE_INIT" ]]; then
  sed -i -E '/^[[:space:]]*keycodes[[:space:]]+114[[:space:]]+115[[:space:]]+116[[:space:]]*$/d' "$LINEAGE_INIT"
fi

# A declared but unavailable OMX HAL makes Android wait indefinitely while
# enumerating codecs. Mainline Waydroid ships software Codec2.
python3 "$SCRIPT_DIR/remove-unused-omx.py" "$ROOTFS"

# Adreno GPU drivers. They replace Waydroid's gralloc.gbm.so (which needs a
# DRM device phones do not have) and add the *_kgsl EGL/GLES libraries.
# /sbin/init only switches Android to them when the phone has /dev/kgsl-3d0.
GPU_RELEASE="$WORK/gpu-release.txt"
: > "$GPU_RELEASE"
for gpu_dir in "${GPU_DIRS[@]}"; do
  [[ -d "$gpu_dir/vendor" ]] || die "-G $gpu_dir has no vendor/ directory (run build-gpu-drivers.sh)"
  log "Installing the Adreno GPU drivers from $gpu_dir"
  (cd "$gpu_dir/vendor" && find . -type f) | while read -r file; do
    file="${file#./}"
    case "$file" in
      bin/*) install -D -m 0755 -o 0 -g 2000 "$gpu_dir/vendor/$file" "$ROOTFS/vendor/$file" ;;
      *)     install -D -m 0644 -o 0 -g 0 "$gpu_dir/vendor/$file" "$ROOTFS/vendor/$file" ;;
    esac
    echo "  /vendor/$file"
  done
  for info in "$gpu_dir"/gpu-release-*.txt; do
    if [[ -f "$info" ]]; then sed 's/^/gpu_/' "$info" >> "$GPU_RELEASE"; fi
  done
done
if [[ ${#GPU_DIRS[@]} -gt 0 ]]; then
  [[ -f "$ROOTFS/vendor/lib64/egl/libEGL_kgsl.so" ]] \
    || die "the GPU drivers have no 64-bit EGL library"
  if [[ -e "$ROOTFS/system/bin/linker" && ! -f "$ROOTFS/vendor/lib/egl/libEGL_kgsl.so" ]]; then
    echo "warning: this image runs 32-bit apps but no 32-bit GPU driver was given;" \
         "Android will draw with the CPU" >&2
  fi
fi

ANDROID_RELEASE="$(sed -n 's/^ro.build.version.release=//p' "$ROOTFS/system/build.prop" | head -n1)"
LINEAGE_VERSION="$(sed -n 's/^ro.lineage.build.version=//p' "$ROOTFS/system/build.prop" | head -n1)"
BUILD_DATE="$(date -u +%Y%m%d)"
ANLAND_REV="$(cat "$TOOLS_DIR/anland-revision.txt" 2>/dev/null || echo unknown)"
BUILDER_REV="$(git -C "$SCRIPT_DIR" rev-parse --verify HEAD 2>/dev/null || echo unknown)"

# Lets Droidspaces show a sensible name for the container.
cat > "$ROOTFS/system/etc/os-release" <<EOF
NAME="Waydroid"
PRETTY_NAME="Waydroid (LineageOS ${LINEAGE_VERSION:-?}, Android ${ANDROID_RELEASE:-?}) for Droidspaces"
ID=waydroid
ID_LIKE=android
VERSION_ID="${LINEAGE_VERSION:-unknown}"
EOF
cat > "$ROOTFS/system/etc/droidspaces-waydroid-release" <<EOF
built=$BUILD_DATE
system_image=$SYSTEM_NAME
vendor_image=$VENDOR_NAME
anland_revision=$ANLAND_REV
builder_revision=$BUILDER_REV
gpu_drivers=$([[ ${#GPU_DIRS[@]} -gt 0 ]] && echo adreno-kgsl || echo none)
EOF
cat "$GPU_RELEASE" >> "$ROOTFS/system/etc/droidspaces-waydroid-release"
chmod 0644 "$ROOTFS/system/etc/os-release" "$ROOTFS/system/etc/droidspaces-waydroid-release"

# The same rule the Droidspaces app applies before it installs an archive.
for entry in bin sbin etc usr; do
  [[ -e "$ROOTFS/$entry" || -L "$ROOTFS/$entry" ]] \
    || die "the rootfs has no top-level $entry; the Droidspaces app would refuse it"
done
[[ -f "$ROOTFS/etc/os-release" ]] || die "/etc does not resolve inside the rootfs"

# --- 5. pack -----------------------------------------------------------------
# The label says what the tree really contains, also for images passed with -S.
if [[ -e "$ROOTFS/system/bin/linker" ]]; then ABI_LABEL="arm64_32"; else ABI_LABEL="arm64_only"; fi
ARCHIVE="$OUT_DIR/Waydroid-LineageOS-${LINEAGE_VERSION:-unknown}-${SYSTEM_TYPE}-${ABI_LABEL}-Droidspaces-Anland-${BUILD_DATE}.tar.xz"
log "Packing $(basename "$ARCHIVE") (this takes a few minutes)"
tar -C "$ROOTFS" --format=gnu --numeric-owner "${TAR_XATTRS[@]}" -cf - . | xz -T0 "-$XZ_LEVEL" > "$ARCHIVE.part"
mv "$ARCHIVE.part" "$ARCHIVE"
(cd "$OUT_DIR" && sha256sum "$(basename "$ARCHIVE")" > "$(basename "$ARCHIVE").sha256")

log "Done"
ls -lh "$ARCHIVE"
echo "Unpacked size: $(du -sh "$ROOTFS" | cut -f1)"
echo "$ARCHIVE" > "$OUT_DIR/latest-archive.txt"
