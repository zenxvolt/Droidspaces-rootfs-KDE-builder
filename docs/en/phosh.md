[Bahasa Indonesia](../id/phosh.md) · [Documentation index](README.md) · [Project home](../../README_english.md)

# Phosh on Fedora 44 (experimental)

The `Phosh` desktop profile builds a Fedora 44 RootFS with [Phosh](https://phosh.mobi/), the phone shell from Purism and GNOME, and shows it in the Anland app like the other Anland desktops.

```text
Anland app (phone screen)
    ▲ buffers, touch, keys, clipboard, audio
    │
/run/display.sock  (Anland daemon)
    ▲
phoc-anland        phoc 0.57 + Anland's wlroots backend
    └── gnome-session --session=phosh
        ├── phosh             lock screen, top bar, app grid, overview
        ├── stevia            on-screen keyboard
        └── GNOME apps        Console, Files, Text Editor, Calculator, Clocks, …
```

> [!WARNING]
> **Status: the compositor has been tested on a phone with a stand-in for the Anland app, but a full Phosh session in the real Anland app has not been seen yet.** See [What has been tested](#what-has-been-tested). Expect the first attempts to need logs.

## How it differs from the other Anland desktops

The other Anland desktops install compositor packages that are built elsewhere and published as Releases. No such package exists for phoc, Phosh's compositor, so this profile builds it during the RootFS build:

- phoc is built at the **same version as Fedora's phoc package**, so phosh, its GSettings schema and phoc stay compatible.
- phoc embeds its own wlroots and links it statically. That wlroots carries Anland's wlroots patch (externally managed swapchains, private keymaps for committed text).
- Anland's **Mango wlroots backend** is compiled into phoc unmodified. A small glue layer gives it phoc's seat, display, pointer constraint and text-input objects.
- The result is one program, `/usr/libexec/phoc-anland`. Fedora's own `phoc` and `wlroots` packages are left alone.
- The toolchain is removed again after the build. `phoc` and `phosh` are kept at the built version with `dnf versionlock`, because a newer phosh may need a newer phoc.

The sources are in [`scripts/anland/`](../../scripts/anland/):

| File | Purpose |
| --- | --- |
| `build-phoc-anland.sh` | Fetches phoc and Anland at pinned versions, applies the patches and builds `phoc-anland`. |
| `phoc/phoc-anland.patch` | Small phoc changes: a meson option, choosing the Anland backend when `ANLAND_SOCKET` is set, and the output render path. |
| `phoc/glue/` | Glue between phoc and the unmodified Anland backend. |
| `phoc/phosh-anland` | The session launcher (`phosh-session` with `phoc-anland`). |
| `install-anland-phosh.sh` | Runs during the RootFS build: installs build dependencies, builds, installs, cleans up. |
| `tests/` | An end-to-end test that runs on a phone. |

## Build

In GitHub Actions, run the RootFS workflow with:

- `build_target`: `Fedora-44`
- `desktop`: `Phosh` (the display backend is forced to Anland Wayland)

Locally:

```bash
./build_rootfs-native.sh -i Fedora-44.Dockerfile -K Phosh -L true -u <user>
```

Compiling phoc and wlroots makes this build noticeably longer than the other Fedora desktops.

## Phone setup

Same as every Anland desktop: see [Anland Wayland host setup](desktop-and-anland.md#anland-wayland-host-setup) (Anland daemon module, Anland app, hardware access, privileged mode, and the `/data/local/tmp/display_daemon.sock -> /run/display.sock` bind mount). Enabling the Snapdragon GPU option (`enable_mesa`) is strongly recommended.

## Start

With `desktop_autostart`, the session starts on boot. To start it by hand as the normal user:

```bash
phosh-anland --check   # checks the binary, socket and render node, starts nothing
phosh-anland
```

| Variable | Default | Effect |
| --- | --- | --- |
| `PHOC_ANLAND_SCALE` | fit 360 logical pixels | Output scale, e.g. `2.5`. Larger means bigger controls. |
| `PHOC_INI` | `/etc/phosh/phoc.ini`, else `/usr/share/phosh/phoc.ini` | phoc configuration. A `scale=` line in an `[output:ANLAND-1]` section also sets the scale. |
| `ANLAND_SOCKET` | `/run/display.sock` | Anland daemon socket. |
| `ANLAND_DRM_DEVICE` | `/dev/dri/renderD128` | GPU render node. |
| `PHOC_ANLAND_ALLOW_YUV` | unset | `1` lets apps hand phoc YUV (video) buffers again; see the Video row below. |

The launcher refuses to start while another Anland desktop of the same user is running.

## What works and what does not

| Part | State |
| --- | --- |
| Picture | Rendered by the GPU into the Anland app's buffers. |
| Touch, keyboard, mouse | Passed to phoc as touch, keyboard and pointer devices. |
| Scale | Chosen automatically from the screen size (720 px wide → 2, 1080 px → 3). |
| Text from the Android keyboard | Sent to the app through text-input-v3 when it has one, otherwise typed as key presses. **Not tested yet.** |
| Clipboard, audio | Provided by the Anland backend as in Mango. **Not tested yet.** |
| Night Light | Ignored: Anland cannot apply gamma tables. |
| Video | phoc-anland does not offer YUV buffer formats to apps, so browsers and players convert video to RGB themselves. With YUV offered, YouTube in Chrome showed green and magenta colours on an Adreno 725 phone. |
| Rotation | The Anland app reports a new screen size and phoc follows it. **Not tested yet.** |
| Calls, SMS, mobile data, sensors | Not available inside a container. ModemManager is removed. |
| Fedora 43 | Not supported: it ships phoc 0.54 on wlroots 0.19, which this patch does not target. |

## What has been tested

On a Snapdragon phone (Adreno 725) inside a Fedora 44 Droidspaces container:

- `build-phoc-anland.sh` builds phoc 0.57.0 with the embedded, patched wlroots 0.20.2 and the static Anland producer library.
- [`tests/run-phoc-anland-test.sh`](../../scripts/anland/tests/run-phoc-anland-test.sh) passes. It runs the real Anland daemon on a private socket, a stand-in app that offers GPU buffers like the real one, and phoc-anland with a test client. It checks:
  - the picture reaches the "app" with exact colours;
  - the client sees a 720x1280 output at scale 2 (360x640 logical);
  - Anland's touch and keyboard reach the client at the right place;
  - phoc shuts down cleanly.

Not tested yet: the Docker RootFS build itself, `gnome-session --session=phosh` inside a Droidspaces container, and the real Anland app.

To run the test on a phone after building phoc-anland (it does not touch the real Anland socket):

```bash
scripts/anland/tests/run-phoc-anland-test.sh /usr/libexec/phoc-anland <anland-checkout>
```

## Troubleshooting

- `journalctl --user -b` and `journalctl -u desktop-session -b` show phoc's and phosh's messages.
- `Using the Anland backend on …` in the log means phoc found the socket. If it is missing, `ANLAND_SOCKET` was not set.
- `Software rendering detected` means the kgsl Mesa driver is not enabled: enable the Snapdragon GPU option, or set `WLR_RENDERER_ALLOW_SOFTWARE=1`.
- Controls too small or too large: set `PHOC_ANLAND_SCALE`.
