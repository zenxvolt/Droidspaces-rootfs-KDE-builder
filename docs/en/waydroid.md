[Documentation index](README.md) · [Project home](../../README_english.md) · [Bahasa Indonesia](../id/waydroid.md)

# Waydroid as a Droidspaces RootFS (experimental)

This builds a rootfs that **is** Waydroid's Android image. Droidspaces boots Android directly as the container: there is no Linux distribution around it, no LXC and no nested container. The screen is shown in the Anland app.

```text
Droidspaces container
└── /sbin/init (ds-waydroid-init)        prepares the container, then becomes…
    └── Android init (PID 1)             Waydroid's LineageOS image
        ├── anland-waydroid-bridge  ───► Anland app (your phone's screen)
        └── SurfaceFlinger → Waydroid hardware composer ──┘
```

> [!WARNING]
> **Status: not yet confirmed on a real phone.** Everything up to the point where Android needs its binder driver was tested (see [What was tested](#what-was-tested)). The test machine has no binder driver, so a complete Android boot could not be observed. Expect to send back a log on the first try.

## What works and what does not

| Area | State |
| --- | --- |
| Display | Shown in the Anland app. Drawn by the CPU (SwiftShader), so it is slow; lower `bridge.render_scale` if needed. |
| Touch | Multi-touch. |
| Keyboard | Hardware keys are forwarded. Use Android's own on-screen keyboard for typing. |
| Mouse | A left click or drag is delivered as a finger tap or swipe. No pointer, no wheel. |
| GPU acceleration | Not available yet. |
| Sound, camera, sensors, clipboard | Not available. |
| Network | Uses the Droidspaces container network. Not confirmed. |

## Requirements

- A rooted ARM64 phone running Droidspaces **with built-in Anland support** (the container settings have an **Anland Display** switch).
- The **Anland app** from the [Anland releases](https://github.com/SuperTurtleDev/anland/releases).
- A kernel with **binderfs** (`CONFIG_ANDROID_BINDERFS=y`). Kernels 5.10 and newer normally have it. Android needs binder devices of its own; the phone's are never shared.
- About 8 GB of free storage.

## 1. Build

1. Open the repository's **Actions** tab and run **Build Waydroid RootFS (Android on Droidspaces via Anland)**.
2. Choose the image:
   - `arm64_only` runs on every ARM64 phone. Pick this if unsure.
   - `arm64_32` also runs 32-bit apps, but only starts on processors that still run 32-bit code. Many recent flagship processors do not.
3. Download the `.tar.xz` from **Releases**.

To build on your own ARM64 Linux machine instead:

```bash
sudo ./waydroid/build-waydroid-rootfs.sh
```

## 2. Install in Droidspaces

1. Import the `.tar.xz` as a new container. Choose **sparse image** storage with a size of **8 GB or more**; Android keeps its data inside the rootfs. Do not install it as a plain folder: Droidspaces itself warns that a rootfs folder on the phone's storage gets damaged by SELinux labelling.
2. In the container settings:
   - turn **Anland Display** on;
   - turn **privileged mode** on with `nocaps` and `noseccomp`;
   - turn **SELinux permissive** on;
   - keep the network on **NAT**;
   - leave the init program at its default (`/sbin/init`).
3. Start the container, then use **Launch Anland**.

The first boot takes several minutes while Android prepares its apps. Anland shows a black screen until Android submits its first frame. You can open Anland while Android starts.

## 3. Settings

One file inside the rootfs holds all settings: `/system/etc/droidspaces-waydroid.conf`. It is read on every start.

| Setting | Meaning |
| --- | --- |
| `bridge.render_scale` | Percentage of the screen resolution Android renders at. The default `50` makes Android draw a quarter of the pixels; the picture is enlarged to fill the screen. `100` is full resolution. |
| `bridge.ui_width_dp` | Chooses Android's density so the short screen side is this many dp. `400` looks like a normal phone; larger makes everything smaller. |
| `bridge.max_fps` | Highest frame rate Android may produce. Default `30`. |
| `bridge.wait_for_display` | Seconds Android's display start waits for the Anland app before using the default size. |
| other `key=value` lines | Android system properties. |

For a running sparse-image container the rootfs is mounted on the phone under `/mnt/Droidspaces/<container name>/`.

## Speed and heat

Android is drawn by the CPU, so the phone works hard and gets warm. This is the main limitation of the current version.

- The rootfs ships with `bridge.render_scale=50` and `bridge.max_fps=30`: Android draws a quarter of the pixels at half the frame rate. Raise them in the settings file for a sharper, smoother picture at the cost of heat.
- The first start is the worst: Android compiles its apps once. Later starts are lighter.
- In Android, **Settings → System → Developer options**, setting the three animation scales to `0.5x` or off removes a lot of drawing.
- Droidspaces can limit the processor share of the container (CPU limit in the container settings) if the phone gets too warm.

GPU acceleration is not available yet. The GPU drivers inside the Waydroid image cannot talk to a phone's GPU: its Adreno driver only supports the mainline Linux kernel interface, not the `kgsl` interface phone kernels have, and there is no open driver for Mali at all. A GPU path therefore needs a separately built driver and is only possible on Snapdragon (Adreno) phones.

## 4. When it does not work

Three places hold the answer, in the order things happen:

| Where | What it says |
| --- | --- |
| `/ds-waydroid-boot.log` in the rootfs | What `/sbin/init` found and did before Android started. Problems are lines starting with `ERROR:`. The report of the boot before is kept as `ds-waydroid-boot.previous.log`. |
| `dmesg` on the phone (as root) | Android's own early messages, lines starting with `init:`. Lines starting with `anland-bridge` are the display bridge. |
| `/ds-waydroid-bridge.log` in the rootfs | The display bridge: whether the Anland app connected and at what size. |

Once Android is up, `logcat` works from a shell in the container.

| Message | Meaning |
| --- | --- |
| `binder: cannot mount binderfs` | The kernel has no binderfs, or the container may not mount it. Check privileged mode and SELinux permissive. Without binderfs in the kernel this cannot work. |
| `processor cannot run 32-bit programs` | Use the `arm64_only` rootfs. |
| `Android was started 5 times within three minutes` | Android keeps rebooting. The cause is in `dmesg`. Starting was paused to save the battery; start the container again to retry. |
| `cannot reach the Anland daemon at /run/display.sock` | **Anland Display** is off for this container. |
| `cannot map Anland buffer` | This phone's screen buffers cannot be written by the CPU, which this bridge needs. |

Anland's display path records three milestones in the bridge log. `Anland app connected` confirms that the bridge mapped the app's buffers. `received Android frame` confirms that Waydroid's hardware composer submitted a frame. `presented the first Android frame to Anland` confirms that the bridge copied it and sent the render completion. If the log stops at `Anland app connected`, check the container's `logcat` for messages from the hardware composer and `SurfaceFlinger`.

When reporting a problem, the easiest way is the Droidspaces app's **Generate a bug report**. It writes one archive to the phone's storage with the container log (which holds the `/sbin/init` report), the kernel log, and the kernel's record of the last crash if the phone restarted.

## How it works

Waydroid normally needs its Python tools and LXC on a Linux host. Here two small static programs take their place:

- **`ds-waydroid-init`** (`/sbin/init`) builds a clean `/dev` with a private binder instance, hides the phone's kernel command line from the guest, writes `/vendor/waydroid.prop` from the settings file and then executes Android's `/init`, which stays PID 1.
- **`anland-waydroid-bridge`** is a minimal Wayland compositor. Waydroid's hardware composer is a Wayland client; the bridge accepts its frames, copies them into the buffer the Anland app selected and turns Anland input into Wayland input. It links Anland's `libdisplay_producer`, so the binary is GPL-3.0.

Nothing in the Android image is modified; the files above are added to it.

## How the phone is protected from the guest

Android's boot scripts assume they own the machine. They tune the scheduler and memory manager through `/proc/sys`, switch on kernel panics for hung tasks, rearrange the CPU groups, and Android's init can ask the kernel to crash through `/proc/sysrq-trigger`. On a normal Waydroid host, LXC makes those read-only. In a privileged Droidspaces container nothing does, and "the machine" is the phone.

`/sbin/init` therefore rebuilds LXC's protections before it starts Android:

- `/proc/sys`, `/proc/sysrq-trigger` and `/sys` are read-only for Android;
- Android gets a cgroup subtree of its own, so it cannot see or rewrite the phone's `top-app`, `background` and other groups;
- the capabilities are reduced to the set Waydroid's LXC configuration keeps (no kernel modules, no raw hardware access).

Rootfs archives built before this was added do not have these protections. Do not use them.

## What was tested

On an x86-64 Linux machine, with the scripts in `waydroid/tests/`:

- `run-bridge-test.sh`: the real Anland daemon, a stand-in for the Anland app and a stand-in for the Waydroid composer. Checks colours, scaling, input, late resolution changes, and continuous presentation before the first Android frame and while Android holds a static frame.
- `run-init-test.sh`: `ds-waydroid-init` inside a real Droidspaces container in default, hardware-access and privileged mode, with a stand-in that makes the same calls as Waydroid's first-stage init.
- The real ARM64 rootfs was booted in Droidspaces under CPU emulation. Android's init ran both stages, loaded the generated properties and started the bridge, then stopped at the first service that needs binder.

Not tested: a private binderfs inside Droidspaces on a phone, everything in Android after binder, speed, and networking.
