#!/usr/bin/env bash
# Build the Adreno (Snapdragon) GPU drivers for the Waydroid rootfs.
#
# Phones drive their Adreno GPU through /dev/kgsl-3d0. The Mesa inside the
# Waydroid image only knows the mainline-Linux interface (/dev/dri), so it can
# never reach a phone's GPU. This script builds, with the Android NDK:
#
#   Mesa freedreno for KGSL   OpenGL ES through EGL  (/vendor/lib*/egl/*_kgsl.so)
#   Mesa turnip for KGSL      Vulkan, 64-bit only     (/vendor/lib64/hw/vulkan.kgsl.so)
#   gralloc.gbm.so            graphics buffers from the dma-buf heap
#
# Mesa comes from the same Adreno tree the Linux rootfs images of this
# repository use (lfdevs/mesa-for-android-container), plus one small patch that
# lets Mesa's Android EGL platform open /dev/kgsl-3d0.
#
# The output directory holds a vendor/ tree that is copied over the rootfs.
#
# Usage: build-gpu-drivers.sh -n NDK_DIR [-a ARCH] [-o OUTPUT_DIR] [-w WORK_DIR]
#   -a  aarch64 (default) or arm (32-bit, for the arm64_32 image)
#
# Needs: git, python3 with mako, pyyaml and packaging, meson >= 1.4, ninja,
#        bison, flex, patchelf, and the Android NDK (r26 or newer).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NDK=""
ARCH="aarch64"
OUT_DIR="$SCRIPT_DIR/out/gpu-aarch64"
WORK_DIR="$SCRIPT_DIR/out/gpu-work"

: "${MESA_REPOSITORY:=https://github.com/lfdevs/mesa-for-android-container}"
: "${MESA_REVISION:=4c94f3e82f91336609830481a27326cb0bae1f78}"
# Lowest Android version the drivers load on (30 = Android 11). Waydroid's
# current images are Android 13.
: "${ANDROID_API:=30}"

while getopts "n:a:o:w:h" opt; do
  case "$opt" in
    n) NDK="$OPTARG" ;;
    a) ARCH="$OPTARG" ;;
    o) OUT_DIR="$OPTARG" ;;
    w) WORK_DIR="$OPTARG" ;;
    *) sed -n '2,22p' "$0"; exit 1 ;;
  esac
done

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ -n "$NDK" ]] || NDK="${ANDROID_NDK_LATEST_HOME:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}"
[[ -d "$NDK" ]] || die "Android NDK not found; pass -n NDK_DIR"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
[[ -d "$TOOLCHAIN" ]] || die "no linux-x86_64 LLVM toolchain in $NDK"

case "$ARCH" in
  aarch64)
    TRIPLE="aarch64-linux-android"; CLANG_TRIPLE="aarch64-linux-android$ANDROID_API"
    CPU_FAMILY="aarch64"; CPU="armv8"; LIBDIR="lib64"; VULKAN="freedreno" ;;
  arm)
    TRIPLE="arm-linux-androideabi"; CLANG_TRIPLE="armv7a-linux-androideabi$ANDROID_API"
    CPU_FAMILY="arm"; CPU="armv7"; LIBDIR="lib"; VULKAN="" ;;
  *) die "-a must be aarch64 or arm" ;;
esac

for tool in git meson ninja bison flex patchelf python3; do
  command -v "$tool" >/dev/null || die "missing build tool: $tool"
done
python3 -c 'import mako, yaml, packaging' 2>/dev/null \
  || die "python3 needs mako, pyyaml and packaging (pip install mako pyyaml packaging)"

mkdir -p "$OUT_DIR" "$WORK_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
WORK_DIR="$(cd "$WORK_DIR" && pwd)"
MESA_SRC="$WORK_DIR/mesa"
BUILD="$WORK_DIR/build-$ARCH"
STAGE="$WORK_DIR/stage-$ARCH"

# --- Mesa source -------------------------------------------------------------
if [[ ! -d "$MESA_SRC/.git" ]]; then
  log "Fetching Mesa $MESA_REVISION"
  git init -q "$MESA_SRC"
  git -C "$MESA_SRC" remote add origin "$MESA_REPOSITORY"
fi
if [[ "$(git -C "$MESA_SRC" rev-parse -q --verify HEAD 2>/dev/null || true)" != "$MESA_REVISION" ]]; then
  git -C "$MESA_SRC" fetch -q --depth 1 origin "$MESA_REVISION"
  git -C "$MESA_SRC" checkout -q -f FETCH_HEAD
fi
git -C "$MESA_SRC" reset -q --hard
git -C "$MESA_SRC" clean -q -fdx -e subprojects/packagecache
for patch in "$SCRIPT_DIR"/gpu/mesa-*.patch; do
  log "Applying $(basename "$patch")"
  git -C "$MESA_SRC" apply --whitespace=nowarn "$patch"
done

# --- Mesa --------------------------------------------------------------------
CROSS="$WORK_DIR/android-$ARCH.ini"
cat > "$CROSS" <<CROSS
[binaries]
ar = '$TOOLCHAIN/bin/llvm-ar'
c = ['$TOOLCHAIN/bin/$CLANG_TRIPLE-clang']
cpp = ['$TOOLCHAIN/bin/$CLANG_TRIPLE-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$TOOLCHAIN/bin/llvm-strip'
pkg-config = '/bin/false'

[host_machine]
system = 'android'
cpu_family = '$CPU_FAMILY'
cpu = '$CPU'
endian = 'little'
CROSS

log "Configuring Mesa for $ARCH (Android API $ANDROID_API)"
rm -rf "$BUILD" "$STAGE"
meson setup "$BUILD" "$MESA_SRC" --cross-file "$CROSS" \
  --prefix=/vendor --libdir="$LIBDIR" \
  -Dbuildtype=release -Db_ndebug=true -Dstrip=true \
  -Dplatforms=android -Dplatform-sdk-version="$ANDROID_API" \
  -Dandroid-stub=true -Dandroid-libbacktrace=disabled -Dandroid-libperfetto=disabled \
  -Dgallium-drivers=freedreno -Dvulkan-drivers="$VULKAN" -Dfreedreno-kmds=kgsl \
  -Degl=enabled -Dgles1=enabled -Dgles2=enabled -Dglx=disabled -Dgbm=disabled \
  -Degl-lib-suffix=_kgsl -Dgles-lib-suffix=_kgsl \
  -Dllvm=disabled -Dexpat=disabled -Dxmlconfig=disabled -Dzstd=disabled \
  -Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dgallium-va=disabled -Dvideo-codecs= -Dvulkan-layers= -Dtools= -Dbuild-tests=false \
  -Dallow-fallback-for=libdrm -Dlibdrm:default_library=static \
  -Dlibdrm:tests=false -Dlibdrm:man-pages=disabled -Dlibdrm:valgrind=disabled \
  -Dlibdrm:cairo-tests=disabled -Dlibdrm:intel=disabled -Dlibdrm:radeon=disabled \
  -Dlibdrm:amdgpu=disabled -Dlibdrm:nouveau=disabled -Dlibdrm:vmwgfx=disabled

log "Building Mesa for $ARCH"
ninja -C "$BUILD"
DESTDIR="$STAGE" ninja -C "$BUILD" install >/dev/null

# --- collect the files Android loads -----------------------------------------
VENDOR="$OUT_DIR/vendor"
rm -rf "$VENDOR"
install -d "$VENDOR/$LIBDIR/egl" "$VENDOR/$LIBDIR/hw"

find_one() { # name pattern -> path inside the stage
  local path
  path="$(find "$STAGE" -name "$1" \( -type f -o -type l \) | head -n1)"
  [[ -n "$path" ]] || die "Mesa did not produce $1"
  readlink -f "$path"
}

for lib in libEGL_kgsl.so libGLESv1_CM_kgsl.so libGLESv2_kgsl.so; do
  install -m 0644 "$(find_one "$lib")" "$VENDOR/$LIBDIR/egl/$lib"
done

# Waydroid's own Mesa already ships /vendor/lib*/libgallium_dri.so. Ours gets a
# name of its own so the two never meet.
install -m 0644 "$(find_one libgallium_dri.so)" "$VENDOR/$LIBDIR/libgallium_kgsl.so"
patchelf --set-soname libgallium_kgsl.so "$VENDOR/$LIBDIR/libgallium_kgsl.so"
for lib in "$VENDOR/$LIBDIR"/egl/*.so; do
  if patchelf --print-needed "$lib" | grep -qx libgallium_dri.so; then
    patchelf --replace-needed libgallium_dri.so libgallium_kgsl.so "$lib"
  fi
done

if [[ -n "$VULKAN" ]]; then
  # Android's Vulkan loader opens /vendor/lib64/hw/vulkan.<ro.hardware.vulkan>.so
  # and wants the library's own name to match.
  install -m 0644 "$(find_one libvulkan_freedreno.so)" "$VENDOR/$LIBDIR/hw/vulkan.kgsl.so"
  patchelf --set-soname vulkan.kgsl.so "$VENDOR/$LIBDIR/hw/vulkan.kgsl.so"
fi

# --- gralloc -----------------------------------------------------------------
log "Building gralloc.gbm.so for $ARCH"
"$TOOLCHAIN/bin/$CLANG_TRIPLE-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -shared \
  -fvisibility=hidden -D_GNU_SOURCE \
  -I"$MESA_SRC/include/android_stub" \
  -Wl,-soname,gralloc.gbm.so -Wl,--no-undefined \
  -o "$VENDOR/$LIBDIR/hw/gralloc.gbm.so" "$SCRIPT_DIR/gpu/gralloc_dmaheap.c" -llog
"$TOOLCHAIN/bin/llvm-strip" --strip-unneeded "$VENDOR/$LIBDIR/hw/gralloc.gbm.so"

# --- report ------------------------------------------------------------------
cat > "$OUT_DIR/gpu-release-$ARCH.txt" <<EOF
mesa_repository=$MESA_REPOSITORY
mesa_revision=$MESA_REVISION
mesa_patches=$(cd "$SCRIPT_DIR/gpu" && sha256sum mesa-*.patch | awk '{print $1}' | tr '\n' ' ')
gralloc_source=$(sha256sum "$SCRIPT_DIR/gpu/gralloc_dmaheap.c" | awk '{print $1}')
android_api=$ANDROID_API
arch=$ARCH
EOF

log "Done"
find "$VENDOR" -type f -printf '%P  %s bytes\n' | sort
for lib in $(find "$VENDOR" -name '*.so' | sort); do
  printf '%s needs: %s\n' "${lib#"$VENDOR"/}" "$(patchelf --print-needed "$lib" | tr '\n' ' ')"
done
