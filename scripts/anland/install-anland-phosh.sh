#!/usr/bin/env bash
# Install Anland support for the Phosh profile while the rootfs is built.
#
# The other Anland desktops download prebuilt packages. No such package
# exists for phoc, so this compiles phoc-anland inside the rootfs, installs it
# with the phosh-anland launcher, and removes the build toolchain again.
#
# Usage: install-anland-phosh [ignored arguments]
set -euo pipefail

SOURCE_DIR="${DROIDSPACES_ANLAND_SOURCE_DIR:-/usr/local/lib/droidspaces/anland}"

die() { printf 'install-anland-phosh: %s\n' "$*" >&2; exit 1; }

[[ $(id -u) == 0 ]] || die "run as root"
source /etc/os-release
[[ "$ID" == fedora ]] || die "only Fedora is supported (found $ID)"
[[ -x "$SOURCE_DIR/build-phoc-anland.sh" ]] || die "build script missing in $SOURCE_DIR"
rpm -q phoc >/dev/null 2>&1 || die "install the phosh desktop profile first"

build_deps=(
    gcc meson ninja-build cmake pkgconf-pkg-config patch gettext git-core
    glib2-devel gnome-desktop3-devel gsettings-desktop-schemas-devel
    libinput-devel systemd-devel libdrm-devel pixman-devel
    wayland-devel wayland-protocols-devel libxkbcommon-devel
    libglvnd-devel mesa-libgbm-devel mesa-libEGL-devel
    libdisplay-info-devel libseat-devel libliftoff-devel lcms2-devel hwdata-devel
    libxcb-devel xcb-util-wm-devel xcb-util-renderutil-devel xcb-util-errors-devel
    xorg-x11-server-Xwayland-devel pipewire-devel gmobile-devel
)

# Remove afterwards only what this script added.
added=()
for package in "${build_deps[@]}"; do
    rpm -q "$package" >/dev/null 2>&1 || added+=("$package")
done

echo "--> Installing the phoc-anland build dependencies"
dnf install -y --setopt=install_weak_deps=False "${build_deps[@]}"

work_dir="$(mktemp -d /tmp/phoc-anland.XXXXXX)"
WORK_DIR="$work_dir" "$SOURCE_DIR/build-phoc-anland.sh"
rm -rf "$work_dir"
install -Dm0755 "$SOURCE_DIR/phoc/phosh-anland" /usr/bin/phosh-anland

# phoc-anland is not an RPM, so nothing records that it needs the runtime
# libraries the -devel packages pulled in. Mark their owners as wanted before
# the toolchain goes, or dnf would remove them as unused dependencies.
mapfile -t runtime_packages < <(
    ldd /usr/libexec/phoc-anland | awk '$2 == "=>" && $3 ~ /^\// { print $3 }' |
        xargs -r realpath | xargs -r rpm -qf --qf '%{NAME}\n' | sort -u
)
((${#runtime_packages[@]})) || die "could not resolve the runtime libraries of phoc-anland"
dnf mark user "${runtime_packages[@]}"

if ((${#added[@]})); then
    echo "--> Removing the build dependencies"
    dnf remove -y "${added[@]}"
fi

if ldd /usr/libexec/phoc-anland | grep 'not found'; then
    die "phoc-anland lost a runtime library while the toolchain was removed"
fi

# phosh talks to phoc over private protocols, so a newer phosh from a later
# "dnf upgrade" may not work with this phoc-anland. Keep both at the version
# it was built for; versionlock leaves dnf.conf (and other holds) alone.
dnf versionlock add phoc phosh
echo "--> phoc-anland installed: $(tr '\n' ' ' < /usr/share/phoc-anland/version)"
