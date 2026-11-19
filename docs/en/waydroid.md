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
> **Status: experimental.** Android has been reported to boot and show in Anland on a phone with CPU drawing. The GPU path (Snapdragon only) is new and has not been confirmed on a phone yet; if it fails, Android can be switched back to CPU drawing with `gpu.mode=off` (see [Settings](#3-settings)).

## What works and what does not

| Area | State |
| --- | --- |
| Display | Shown in the Anland app. On Snapdragon phones with Hardware Access, Android draws with the Adreno GPU at full resolution. Everywhere else it draws with the CPU (SwiftShader) at half resolution and 30 fps, which looks blurry and stutters. |
| Touch | Multi-touch. |
| Keyboard | Hardware keys are forwarded. Use Android's own on-screen keyboard for typing. |
| Mouse | A left click or drag is delivered as a finger tap or swipe. No pointer, no wheel. |
| GPU acceleration | Snapdragon (Adreno) only, OpenGL ES through Mesa freedreno on KGSL. Optional Vulkan (Mesa turnip). Not yet confirmed on a phone. Mali, PowerVR and other GPUs: no. |
| Sound, camera, sensors, clipboard | Not available. |
| Network | Uses the Droidspaces container network. Not confirmed. |

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
   - keep the network on **NAT**;
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
| `bridge.ui_width_dp` | Chooses Android's density so the short screen side is this many dp. `400` looks like a normal phone; larger makes everything smaller. |
| `bridge.max_fps` | Highest frame rate Android may produce. `auto` (default) is `60` with the GPU and `30` with the CPU. |
| `bridge.wait_for_display` | Seconds Android's display start waits for the Anland app before using the default size. |
| other `key=value` lines | Android system properties. |

For a running sparse-image container the rootfs is mounted on the phone under `/mnt/Droidspaces/<container name>/`.

## Speed and heat

How Android draws decides almost everything:

- **With the GPU** (Snapdragon phone, Hardware Access on), Android renders at the full screen resolution and up to 60 fps. The CPU still copies each finished frame into the Anland app's buffer, which costs some battery at high resolutions. If the phone gets warm, set `bridge.render_scale=75` or `bridge.max_fps=45`.
- **With the CPU** (every other phone, or `gpu.mode=off`), SwiftShader draws every pixel. That is why the rootfs then renders only a quarter of the pixels (`render_scale` 50) at 30 fps: the picture is blurry and animations stutter, but the phone stays usable. Raise both in the settings file for a sharper, smoother picture at the cost of heat.

Either way:

- The first start is the heaviest: Android compiles its apps once. Later starts are lighter.
- In Android, **Settings → System → Developer options**, setting the three animation scales to `0.5x` or off removes a lot of drawing.
- Droidspaces can limit the processor share of the container (CPU limit in the container settings) if the phone gets too warm.

`/ds-waydroid-boot.log` says which drawing mode was chosen (lines starting with `gpu:`), and `/ds-waydroid-bridge.log` says `drawn by the GPU` or `drawn by the CPU` when the first frame arrives.

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
| `gpu: there is no /dev/kgsl-3d0` | Android draws with the CPU. On a Snapdragon phone, turn on **Hardware Access** in the container settings. |
| `gpu: no dma-buf heap ... can be opened` | The kernel has neither `/dev/dma_heap/system` nor `/dev/ion`, or Hardware Access does not pass them in. Android draws with the CPU. |
| Black screen or Android restarting after switching to the GPU | Set `gpu.mode=off` in the settings file to go back to CPU drawing, and send `logcat` lines from `SurfaceFlinger`, `libEGL`, `MESA` and `gralloc.dmaheap`. |
| `gralloc: couldn't create ashmem` | Android could not allocate a graphics buffer. Current builds default to `sys.use_memfd=true` even when Hardware Access exposes an ashmem device. For an older image, add `sys.use_memfd=true` to `/system/etc/droidspaces-waydroid.conf` and restart the container. |

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
| `/vendor/lib64/egl/libEGL_kgsl.so`, `libGLESv1_CM_kgsl.so`, `libGLESv2_kgsl.so`, `/vendor/lib64/libgallium_kgsl.so` | OpenGL ES: Mesa freedreno on KGSL. A small patch (`waydroid/gpu/mesa-egl-android-kgsl.patch`) lets Mesa's Android EGL platform open `/dev/kgsl-3d0`. |
| `/vendor/lib64/hw/vulkan.kgsl.so` | Vulkan: Mesa turnip on KGSL (only with `gpu.vulkan=1`). |
| `/vendor/lib64/hw/gralloc.gbm.so` | Replaces Waydroid's allocator, which needs `/dev/dri`. Graphics buffers come from the kernel's dma-buf heap, which the GPU imports through KGSL and the bridge can map. |

When the GPU is chosen, `/sbin/init` sets `ro.hardware.gralloc=gbm`, `ro.hardware.egl=kgsl` and `mesa.loader.driver.override=kgsl`. Waydroid's composer then hands each finished frame to the bridge as a dma-buf, and the bridge copies it to the Anland buffer. Setting any of these properties in the settings file overrides the automatic choice.

The builder removes LineageOS's physical-button bug report shortcut so Android init does not block while scanning Waydroid's input FIFOs. The Android `dumpstate` command remains available.

## How the phone is protected from the guest

Android's boot scripts assume they own the machine. They tune the scheduler and memory manager through `/proc/sys`, switch on kernel panics for hung tasks, rearrange the CPU groups, and Android's init can ask the kernel to crash through `/proc/sysrq-trigger`. On a normal Waydroid host, LXC makes those read-only. In a privileged Droidspaces container nothing does, and "the machine" is the phone.

`/sbin/init` therefore rebuilds LXC's protections before it starts Android:

- `/proc/sys`, `/proc/sysrq-trigger` and `/sys` are read-only for Android;
- Android gets a cgroup subtree of its own, so it cannot see or rewrite the phone's `top-app`, `background` and other groups;
- the capabilities are reduced to the set Waydroid's LXC configuration keeps (no kernel modules, no raw hardware access).

Rootfs archives built before this was added do not have these protections. Do not use them.

## What was tested

On an x86-64 Linux machine, with the scripts in `waydroid/tests/`:

- `run-bridge-test.sh`: the real Anland daemon, a stand-in for the Anland app and a stand-in for the Waydroid composer. Checks colours, scaling, input, late resolution changes, continuous presentation before the first Android frame and while Android holds a static frame, GPU-style dma-buf frames with padded rows, and the `auto` settings for both drawing modes.
- `run-init-test.sh`: `ds-waydroid-init` inside a real Droidspaces container in default, hardware-access and privileged mode, with a stand-in that makes the same calls as Waydroid's first-stage init.
- The real ARM64 rootfs was booted in Droidspaces under CPU emulation. Android's init ran both stages, loaded the generated properties and started the bridge, then stopped at the first service that needs binder.

Not tested: the GPU driver on a phone (it is built in CI but no Snapdragon phone was available), speed, and networking.
