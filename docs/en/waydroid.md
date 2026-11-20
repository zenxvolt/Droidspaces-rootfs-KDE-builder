[Documentation index](README.md) · [Project home](../../README_english.md) · [Bahasa Indonesia](../id/waydroid.md)

# Waydroid as a Droidspaces RootFS (experimental)

This builds a rootfs that **is** Waydroid's Android image. Droidspaces boots Android directly as the container: there is no Linux distribution around it, no LXC and no nested container. The screen is shown in the Anland app.

```text
Droidspaces container
└── /sbin/init (ds-waydroid-init)        prepares the container, then becomes…
    └── Android init (PID 1)             Waydroid's LineageOS image
        ├── anland-waydroid-bridge  ───► Anland app (your phone's screen)
        └── SurfaceFlinger → Waydroid hardware composer ──┘
              └── draws with the Adreno GPU (Snapdragon) or with the CPU
```

> [!WARNING]
> **Status: experimental.** Android has booted and appeared in Anland on the test phone with software rendering and with its Adreno 725 GPU. Use `gpu.mode=off` to select software rendering in [Settings](#3-settings).

## What works and what does not

| Area | State |
| --- | --- |
| Display | Shown in the Anland app. On Snapdragon phones with Hardware Access, Android draws with the Adreno GPU at full resolution. Everywhere else it draws with the CPU (SwiftShader) at half resolution and 30 fps, which looks blurry and stutters. |
| Touch | Multi-touch. |
| Keyboard | Hardware keys are forwarded. Use Android's own on-screen keyboard for typing. |
| Mouse | A left click or drag is delivered as a finger tap or swipe. No pointer, no wheel. |
| GPU acceleration | Snapdragon (Adreno) only, OpenGL ES through Mesa freedreno on KGSL. Optional Vulkan (Mesa turnip). OpenGL ES was verified on Adreno 725. Vulkan still needs testing on a phone. |
| Clipboard | Plain text can be copied and pasted between Anland and Android in Waydroid. |
| Sound | Plays from the Anland app, through Anland's own audio channel like a Linux desktop on Anland. No Termux or PulseAudio setup is needed. |
| Camera, sensors, microphone | Not available. |
| Network | Droidspaces' NAT network; Android gets its address by DHCP like on a Waydroid host. Jelly loaded an HTTPS test page through Droidspaces NAT in a Linux VM. |

## Requirements

- A rooted ARM64 phone running Droidspaces **with built-in Anland support** (the container settings have an **Anland Display** switch).
- The **Anland app** from the [Anland releases](https://github.com/SuperTurtleDev/anland/releases).
- A kernel with **binderfs** (`CONFIG_ANDROID_BINDERFS=y`). Kernels 5.10 and newer normally have it. Android needs binder devices of its own; the phone's are never shared.
- About 8 GB of free storage.
- For GPU drawing: a **Snapdragon** phone (Adreno GPU, `/dev/kgsl-3d0`). Kernels 5.10 and newer also provide the dma-buf heap the driver needs.

## 1. Build

1. Open the repository's **Actions** tab and run **Build Waydroid RootFS (Android on Droidspaces via Anland)**.
2. Choose the image:
   - `arm64_only` runs on every ARM64 phone. Pick this if unsure.
   - `arm64_32` also runs 32-bit apps, but only starts on processors that still run 32-bit code. Many recent flagship processors do not.
3. Leave **gpu** on. It adds the Adreno driver (a few tens of MB); phones without a Snapdragon GPU simply keep drawing with the CPU. The first build with it takes about 20 minutes longer; later builds reuse the driver.
4. Download the `.tar.xz` from **Releases**.

To build on your own ARM64 Linux machine instead:

```bash
sudo ./waydroid/build-waydroid-rootfs.sh
```

The GPU driver is built separately with the Android NDK on an x86-64 machine and passed with `-G`:

```bash
./waydroid/build-gpu-drivers.sh -n /path/to/android-ndk -a aarch64 -o gpu-aarch64
sudo ./waydroid/build-waydroid-rootfs.sh -G gpu-aarch64
```

## 2. Install in Droidspaces

1. Import the `.tar.xz` as a new container. Choose **sparse image** storage with a size of **8 GB or more**; Android keeps its data inside the rootfs. Do not install it as a plain folder: Droidspaces itself warns that a rootfs folder on the phone's storage gets damaged by SELinux labelling.
2. In the container settings:
   - turn **Anland Display** on;
   - turn **privileged mode** on with `nocaps` and `noseccomp`;
   - turn **SELinux permissive** on;
   - on a Snapdragon phone, turn **Hardware Access** on so Android can use the GPU;
   - keep the network on **NAT** (in host mode Android would rewrite the phone's routing, so it gets no network there);
   - keep **Configure PulseAudio** off, as Anland recommends: sound goes through the Anland app;
   - leave the init program at its default (`/sbin/init`).
3. Start the container, then use **Launch Anland**.

The first boot takes several minutes while Android prepares its apps. Anland shows a black screen until Android submits its first frame. You can open Anland while Android starts.

## 3. Settings

One file inside the rootfs holds all settings: `/system/etc/droidspaces-waydroid.conf`. It is read on every start.

| Setting | Meaning |
| --- | --- |
| `gpu.mode` | `auto` (default) uses the Adreno GPU when `/dev/kgsl-3d0` and a dma-buf heap can be opened, otherwise the CPU. `off` always draws with the CPU. `on` uses the GPU driver even when the checks fail (troubleshooting only). |
| `gpu.vulkan` | `1` also offers Vulkan (Mesa turnip) to apps and games. Default `0`. |
| `bridge.render_scale` | Percentage of the screen resolution Android renders at. `auto` (default) is `100` with the GPU and `50` with the CPU. `50` draws a quarter of the pixels and enlarges the picture, which looks blurry. |
| `bridge.ui_width_dp` | Sets density before SurfaceFlinger starts, using the short side of `bridge.default_width` / `bridge.default_height` after applying `render_scale`. `400` suits a phone. Lower values make the UI larger. An explicit `ro.sf.lcd_density` takes precedence. |
| `bridge.max_fps` | Refresh cap advertised to Android. With `auto`, GPU mode follows Anland and CPU mode caps refresh at 30 Hz. A number sets an explicit cap. |
| `bridge.gpu_copy` | Who copies Android's frames into the Anland buffer. `auto` (default) lets the GPU do it when Android draws with the GPU. `0` always uses the CPU. |
| `bridge.wait_for_display` | Seconds Android's display start waits for the Anland app before using the default size. |
| other `key=value` lines | Android system properties. |

For a running sparse-image container the rootfs is mounted on the phone under `/mnt/Droidspaces/<container name>/`.

## Speed and heat

How Android draws decides almost everything:

- **With the GPU** (Snapdragon phone, Hardware Access on), Android renders at full screen resolution and follows the refresh reported by Anland. Open Anland during boot so the Waydroid composer picks up that refresh when it starts. New-frame FPS depends on rendering and copy workload. Set `bridge.max_fps` for a lower cap.
- **With the CPU** (every other phone, or `gpu.mode=off`), SwiftShader draws every pixel. That is why the rootfs then renders only a quarter of the pixels (`render_scale` 50) at 30 fps: the picture is blurry and animations stutter, but the phone stays usable. Raise both in the settings file for a sharper, smoother picture at the cost of heat.

Either way:

- The first start is the heaviest: Android compiles its apps once. Later starts are lighter.
- In Android, **Settings → System → Developer options**, setting the three animation scales to `0.5x` or off removes a lot of drawing.
- Droidspaces can limit the processor share of the container (CPU limit in the container settings) if the phone gets too warm.

`/ds-waydroid-boot.log` says which drawing mode was chosen (lines starting with `gpu:`). `/ds-waydroid-bridge.log` says `drawn by the GPU` or `drawn by the CPU` when the first frame arrives, and `copied by the GPU` or `copied by the CPU` when it reaches Anland.

### Frame rate and GPU benchmark

Open Anland and scroll through the same screen during the 15-second sample. Replace `Waydroid` if your container has a different name.

```sh
su -c 'R="/mnt/Droidspaces/Waydroid"; sh "$R/system/bin/ds-waydroid-benchmark" "$R" 15'
```

- **Android FPS** counts frame submissions from the composer. **Anland new FPS** counts new frames successfully submitted to Anland by the bridge. It excludes repeats, blank acknowledgments and copies of a frame to the other buffers in the pool. The phone panel is outside the scope of this measurement.
- **Configured rendering**, whether the bridge can open `/dev/kgsl-3d0`, and the `dma-buf` frame count describe the GPU path. **Buffer copies** shows which processor performs the copies. Running `/system/bin/ds-waydroid-benchmark / 15` inside Waydroid also includes SurfaceFlinger's GLES renderer when available.

## 4. When it does not work

Three places hold the answer, in the order things happen:

| Where | What it says |
| --- | --- |
| `/ds-waydroid-boot.log` in the rootfs | What `/sbin/init` found and did before Android started. Problems are lines starting with `ERROR:`. The report of the boot before is kept as `ds-waydroid-boot.previous.log`. |
| `dmesg` on the phone (as root) | Android's own early messages, lines starting with `init:`. Lines starting with `anland-bridge` are the display bridge. |
| `/ds-waydroid-bridge.log` in the rootfs | The display bridge: whether the Anland app connected and at what size. |

Once Android is up, its log is also written to `/data/ds-waydroid-logcat.txt` in the rootfs (for a running sparse-image container: `/mnt/Droidspaces/<container name>/data/ds-waydroid-logcat.txt`), so it can be read from the phone without entering the container. The log of the start before is kept as `ds-waydroid-logcat.previous.txt`, and the bridge's as `ds-waydroid-bridge.log.previous`, which matters after an automatic switch to CPU drawing.

For network or sound problems, run `ds-waydroid-diag` in a shell inside the container: it writes `/ds-waydroid-diag.txt` in the rootfs with the addresses, routes, what Android's network service decided, the sound setup and the matching `logcat` lines. Send it along with `/ds-waydroid-boot.log`.

| Message | Meaning |
| --- | --- |
| `binder: cannot mount binderfs` | The kernel has no binderfs, or the container may not mount it. Check privileged mode and SELinux permissive. Without binderfs in the kernel this cannot work. |
| `processor cannot run 32-bit programs` | Use the `arm64_only` rootfs. |
| `Android was started 5 times within three minutes` | Android keeps rebooting. The cause is in `dmesg`. Starting was paused to save the battery; start the container again to retry. |
| `cannot reach the Anland daemon at /run/display.sock` | **Anland Display** is off for this container. |
| `cannot map Anland buffer` | This phone's screen buffers cannot be written by the CPU, which this bridge needs. |
| `gpu: there is no /dev/kgsl-3d0` | Android draws with the CPU. On a Snapdragon phone, turn on **Hardware Access** in the container settings. |
| `gpu: no dma-buf heap ... can be opened` | The kernel has neither `/dev/dma_heap/system` nor `/dev/ion`, or Hardware Access does not pass them in. Android draws with the CPU. |
| Repeated `Waited one second for android.hardware.media.omx@1.0::IOmxStore/default` | The vendor manifest advertises an OMX service that the image does not ship. The builder removes the stale entry on Waydroid Mainline images that include software Codec2. |
| `GPU copy turned off (...)` in the bridge log | The GPU could not copy frames into the Anland buffer, for the reason in brackets. The CPU copies them instead, so the picture still works but uses more battery. Please report the message. |
| `gpu: the GPU driver failed before` in the boot log | SurfaceFlinger crashed three times with the GPU driver, so Android was restarted with CPU drawing (a GPU fault also makes the phone's own screen stutter). Delete `/data/ds-gpu-failed` in the rootfs, or set `gpu.mode=on`, to try the GPU again. The failed start's logs are kept as `/data/ds-gpu-failed.logcat.txt` and `/data/ds-gpu-failed.bridge.log`; please include them in a report. |
| Black screen or Android restarting after switching to the GPU | Set `gpu.mode=off` in the settings file to go back to CPU drawing, and send `logcat` lines from `SurfaceFlinger`, `libEGL`, `MESA` and `gralloc.dmaheap`. |
| `gralloc: couldn't create ashmem` | Android could not allocate a graphics buffer. Current builds default to `sys.use_memfd=true` even when Hardware Access exposes an ashmem device. For an older image, add `sys.use_memfd=true` to `/system/etc/droidspaces-waydroid.conf` and restart the container. |
| `network: there is no eth0` | The container has no network of its own. Set the network mode to **NAT**. |
| `network: the container shares the phone's network` | The network mode is host. Set it to **NAT**. |
| `/proc/sys/net is read-only` | Android cannot bring its network up. Include this report when asking for help. |

Anland's display path records three milestones in the bridge log. `Anland app connected` confirms that the bridge mapped the app's buffers. `received Android frame` confirms that Waydroid's hardware composer submitted a frame. `presented the first Android frame to Anland` confirms that the bridge copied it and sent the render completion. If the log stops at `Anland app connected`, check the container's `logcat` for messages from the hardware composer and `SurfaceFlinger`.

When reporting a problem, the easiest way is the Droidspaces app's **Generate a bug report**. It writes one archive to the phone's storage with the container log (which holds the `/sbin/init` report), the kernel log, and the kernel's record of the last crash if the phone restarted.

## How it works

Waydroid normally needs its Python tools and LXC on a Linux host. Here two small static programs take their place:

- **`ds-waydroid-init`** (`/sbin/init`) builds a clean `/dev` with a private binder instance, hides the phone's kernel command line from the guest, chooses between GPU and CPU drawing, writes `/vendor/waydroid.prop` from the settings file and then executes Android's `/init`, which stays PID 1.
- **`anland-waydroid-bridge`** is a minimal Wayland compositor. Waydroid's hardware composer is a Wayland client; the bridge accepts its frames (shared memory from the CPU path, linux-dmabuf from the GPU path), copies them into the buffer the Anland app selected and turns Anland input into Wayland input. It links Anland's `libdisplay_producer`, so the binary is GPL-3.0.

### The GPU path

The Mesa inside the Waydroid image only talks to GPUs through the mainline Linux interface (`/dev/dri`). Phone kernels drive the Adreno GPU through Qualcomm's `kgsl` interface instead, so the rootfs adds its own driver, built with the Android NDK from the same Adreno Mesa tree the Linux rootfs images of this repository use ([mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container)):

| File | Role |
| --- | --- |
| `/vendor/lib64/egl/libEGL_kgsl.so`, `libGLESv1_CM_kgsl.so`, `libGLESv2_kgsl.so`, `/vendor/lib64/libgallium_kgsl.so` | OpenGL ES: Mesa freedreno on KGSL. Small patches in `waydroid/gpu/` let Mesa's Android EGL platform open `/dev/kgsl-3d0`, let freedreno accept Android's dma-buf buffers on KGSL, and stop Mesa from writing a register that KGSL protects on the Adreno 725. |
| `/vendor/lib64/hw/vulkan.kgsl.so` | Vulkan: Mesa turnip on KGSL (only with `gpu.vulkan=1`). |
| `/vendor/lib64/hw/gralloc.gbm.so` | Replaces Waydroid's allocator, which needs `/dev/dri`. Graphics buffers come from the kernel's dma-buf heap, which the GPU imports through KGSL and the bridge can map. |
| `/vendor/bin/anland-gpu-copy` | Copies frames into the Anland buffer with the GPU for the bridge. |

When the GPU is chosen, `/sbin/init` sets `ro.hardware.gralloc=gbm`, `gralloc.gbm.legacy=true` (otherwise Waydroid's init swaps `gbm` for its minigbm allocator, which needs a DRM GPU), `ro.hardware.egl=kgsl` and `mesa.loader.driver.override=kgsl`. Setting any of these properties in the settings file overrides the automatic choice.

Waydroid's composer then hands each finished frame to the bridge as a dma-buf. The bridge itself is a static program without a GPU driver (it has to run before Android is up), so on the first GPU frame it starts `anland-gpu-copy`, which is linked to Android's EGL. The helper imports Android's frame and the Anland app's screen buffers as EGL images and copies with one draw, scaling the picture when `render_scale` is below 100. It hands the GPU's render-done fence back to the bridge, which passes it to Anland, the same way the KDE and GNOME backends of the Linux rootfs images hand over their frames: nobody waits for the GPU on the CPU. If the helper fails, the bridge copies with the CPU, as in CPU mode.

The desktop rootfs images go one step further: their compositor draws straight into the Anland buffer, with no copy at all. Android's SurfaceFlinger chooses its own buffers, while Anland decides which of its buffers is drawn next, so here one GPU copy remains.

The builder removes LineageOS's physical-button bug report shortcut so Android init does not block while scanning Waydroid's input FIFOs. The Android `dumpstate` command remains available.

## How the phone is protected from the guest

Android's boot scripts assume they own the machine. They tune the scheduler and memory manager through `/proc/sys`, switch on kernel panics for hung tasks, rearrange the CPU groups, and Android's init can ask the kernel to crash through `/proc/sysrq-trigger`. On a normal Waydroid host, LXC makes those read-only. In a privileged Droidspaces container nothing does, and "the machine" is the phone.

`/sbin/init` therefore rebuilds LXC's protections before it starts Android:

- `/proc/sys`, `/proc/sysrq-trigger` and `/sys` are read-only for Android. The exception is `/proc/sys/net`, as with LXC: it holds only the settings of the container's own network, and Android needs to write them to bring its network up (builds before this was fixed had no network in apps);
- Android gets a cgroup subtree of its own, so it cannot see or rewrite the phone's `top-app`, `background` and other groups;
- the capabilities are reduced to the set Waydroid's LXC configuration keeps (no kernel modules, no raw hardware access).

Rootfs archives built before this was added do not have these protections. Do not use them.

## What was tested

On an x86-64 Linux machine, with the scripts in `waydroid/tests/`:

- `run-bridge-test.sh`: the real Anland daemon, a stand-in for the Anland app and a stand-in for the Waydroid composer. Checks colours, scaling, input, late resolution changes, continuous presentation before the first Android frame and while Android holds a static frame, GPU-style dma-buf frames with padded rows, the GPU copy helper (the real one on Mesa's software renderer, and a stand-in for render fences and failures), and the `auto` settings for both drawing modes.
- `run-init-test.sh`: `ds-waydroid-init` inside a real Droidspaces container in default, hardware-access and privileged mode, with a stand-in that makes the same calls as Waydroid's first-stage init.
- The ARM64 rootfs reached the home screen in Droidspaces under CPU emulation. Gallery, Files, bidirectional text clipboard, and an HTTPS test page through NAT were verified in Anland.

See the [progress notes](../../waydroid/PROGRESS.md) for Adreno 725 validation and the remaining Jelly issue on the phone.
