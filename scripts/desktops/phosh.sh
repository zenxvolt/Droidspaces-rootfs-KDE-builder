#!/usr/bin/env bash
set -euo pipefail

source "${ROOTFS_DIR:-}/etc/os-release"

configure_environment() {
    local backend="${1:-}"
    local environment_file="${ROOTFS_DIR:-}/etc/environment"
    local assignment key
    local -a assignments=(
        XDG_SESSION_CLASS=user
        XDG_SESSION_TYPE=wayland
        XDG_CURRENT_DESKTOP=Phosh:GNOME
        XDG_SESSION_DESKTOP=phosh
        WAYLAND_DISPLAY=wayland-0
        QT_QPA_PLATFORM=wayland
        ANLAND=1
        ANLAND_SOCKET=/run/display.sock
        ANLAND_DRM_DEVICE=/dev/dri/renderD128
    )

    [[ "$backend" == anland-wayland ]] || {
        echo "Phosh 显示后端无效：$backend" >&2
        return 1
    }

    touch "$environment_file"
    for assignment in "${assignments[@]}"; do
        key="${assignment%%=*}"
        grep -q "^${key}=" "$environment_file" || printf '%s\n' "$assignment" >> "$environment_file"
    done
}

install_fedora() {
    # phoc-anland is built for the phoc release Fedora ships. Fedora 44 has
    # phoc 0.57 on wlroots 0.20, which the Anland backend patch targets.
    case "$VERSION_ID" in
        44) ;;
        *) echo "Phosh 不支持 Fedora $VERSION_ID（仅支持 Fedora 44）" >&2; return 1 ;;
    esac

    dnf install -y --setopt=install_weak_deps=False \
        phosh phoc stevia phosh-mobile-settings \
        gnome-session gnome-settings-daemon gnome-control-center \
        gnome-console nautilus gnome-text-editor gnome-calculator gnome-clocks loupe papers \
        feedbackd polkit upower xdg-user-dirs xdg-desktop-portal-gtk \
        adwaita-icon-theme google-noto-sans-cjk-fonts google-noto-color-emoji-fonts \
        pipewire pipewire-alsa pipewire-pulseaudio wireplumber pulseaudio-utils \
        xorg-x11-server-Xwayland wayland-utils glx-utils vulkan-tools \
        gstreamer1-plugins-base gstreamer1-plugins-good libcanberra sound-theme-freedesktop
    # A container has no modem; Phosh would otherwise keep probing for one.
    dnf remove -y ModemManager || true
}

install_profile() {
    case "$ID" in
        fedora) install_fedora ;;
        *) echo "Phosh 不支持当前发行版：$ID（仅支持 Fedora 44）" >&2; return 1 ;;
    esac
}

case "${1:-install}" in
    install) install_profile ;;
    configure-environment) configure_environment "${2:-}" ;;
    *)
        echo "Phosh profile 操作无效：$1" >&2
        exit 1
        ;;
esac
