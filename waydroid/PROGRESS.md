# Waydroid rootfs — progres dan kendala (catatan serah terima)

Status per 2026-11-20. Branch kerja: `ccr-4a58af0b-eo3vvg` (dibuat dari
`waydroid/anland-frame-pacing`, belum di-merge ke `main`). Dokumentasi untuk
pengguna ada di `docs/en/waydroid.md` dan `docs/id/waydroid.md`; catatan ini
untuk developer/agent berikutnya.

## Tujuan pemilik repo

- **Satu rootfs Waydroid generik** yang bisa dipasang di ponsel ARM64 mana pun
  lewat Droidspaces dan ditampilkan di aplikasi Anland.
- Pendekatan sebisa mungkin **seragam dengan rootfs Wayland lain di repo ini**
  (Mesa Adreno dari `lfdevs/mesa-for-android-container`, freedreno lewat KGSL,
  `MESA_LOADER_DRIVER_OVERRIDE=kgsl`). Pemilik **tidak mau** pendekatan driver
  vendor ponsel (Halium/`/vendor_extra`) karena tidak generik.
- Pemilik tidak suka tambalan/inisiatif tambahan yang tidak diminta. Sebelum
  menambah lapisan baru, jelaskan dan tanyakan dulu.

## Yang sudah berjalan (terbukti di ponsel)

Ponsel uji: Xiaomi "marble"/UKEE, SM7475, **Adreno 725** (chip id 0x07030002),
kernel 5.10.236-android12, image Waydroid LineageOS 20 (Android 13) `arm64_only`.

- Rootfs boot sampai home screen dan tampil di Anland dalam **mode CPU**
  (SwiftShader/ANGLE, `gralloc.default`). Input sentuh/keyboard jalan.
- Mode CPU di-render 50% resolusi dan 30 fps (`bridge.render_scale=auto`,
  `bridge.max_fps=auto`) → itu sebab tampilan buram dan patah-patah.
- Dengan perbaikan terakhir, **GPU Adreno sudah menggambar boot animation**
  lewat Mesa freedreno/KGSL; di `dmesg` tidak ada lagi GPU hang.

## Kendala yang masih terbuka (penyebab mode GPU belum bisa dipakai)

1. **SurfaceFlinger abort: `Unable to generate SkImage. isTextureValid:0 dataspace:0`**
   - Backtrace: `AutoBackendTexture::makeImage` ← `SkiaGLRenderEngine::drawLayersInternal`.
   - Artinya impor buffer aplikasi sebagai tekstur (EGLImage dari
     `EGL_NATIVE_BUFFER_ANDROID`, target `GL_TEXTURE_EXTERNAL_OES`) gagal.
     Buffer output SurfaceFlinger sendiri (target `GL_TEXTURE_2D`, RGBA_8888)
     sudah berhasil.
   - Dugaan (belum terbukti): format non-RGBA8888 (boot animation memakai
     `RGB_565`; ada juga `RGBX_8888`/`IMPLEMENTATION_DEFINED`) atau jalur
     `GL_TEXTURE_EXTERNAL_OES` di Mesa/freedreno pada Android. Patch
     `mesa-u-gralloc-libdrm-format.patch` sudah dibuat untuk satu kemungkinan
     (format diambil dari handle gralloc), tetapi crash masih terjadi.
   - **Log yang dibutuhkan belum pernah didapat**: baris `skia`
     (`Could not create EGL image, err=...` atau `glEGLImageTargetTexture2DOES failed`)
     dan baris Mesa dengan `mesa.egl.log.level=debug` + `mesa.fd.mesa.debug=layout`
     dari start mode GPU. Sejak commit `346288d`, saat menyerah
     `ds-gpu-guard.sh` menyimpan log itu ke `/data/ds-gpu-failed.logcat.txt`
     dan `/data/ds-gpu-failed.bridge.log` di rootfs.

2. **Composer Waydroid abort di `hwc_wayland_thread`** setelah SurfaceFlinger
   crash, lalu SurfaceFlinger abort lagi (`DEAD_OBJECT` di
   `HidlComposer::getColorModes`). Kemungkinan akibat dari kendala 1, atau
   bridge menolak buffer. Bridge sudah diperluas menerima RGB565/BGR888
   (commit `15aabda`); belum diverifikasi di ponsel.

3. **Belum diuji di ponsel sama sekali**:
   - helper `anland-gpu-copy` (salinan frame ke Anland oleh GPU, render fence);
   - build `arm64_32` (driver 32-bit; job CI-nya `continue-on-error`);
   - Vulkan/turnip (`gpu.vulkan=1`);
   - Adreno selain 725 dan ponsel non-Snapdragon dengan Hardware Access.

4. **Tidak bisa diuji di CI/VM**: jalur GPU butuh `/dev/kgsl-3d0` dan GPU
   Adreno sungguhan. Environment agent tidak punya KVM.

## Masalah yang sudah ditemukan dan diperbaiki (urut kejadian)

| Gejala di ponsel | Penyebab | Perbaikan |
| --- | --- | --- |
| Buram, patah-patah | CPU rendering, 50%/30 fps | Jalur GPU Adreno (commit `63e8a6f`) |
| SurfaceFlinger: `output buffer not gpu writeable` | Init Waydroid (patch `0014-init-Default-to-minigbm_gbm_mesa`) mengganti `ro.hardware.gralloc=gbm` → `minigbm_gbm_mesa` (butuh GPU DRM; `/dev/dri/renderD128` di ponsel adalah display, bukan GPU) | `/sbin/init` menulis `gralloc.gbm.legacy=true` (`2bc16bb`) |
| `Unable to generate SkSurface`, `Could not create EGL image err=0x3000` | `u_init_pipe_screen_caps` memakai `drmGetCap(DRM_CAP_PRIME)` yang gagal di KGSL → frontend DRI menolak semua impor dma-buf | `mesa-freedreno-kgsl-dmabuf-caps.patch` (`9c5a349`) |
| `submit failed -1 (Resource deadlock would occur)` ribuan kali, ponsel tersendat; dmesg `CP | Protected mode error | WRITE | addr=930a` | Mesa menulis `VPC_UNKNOWN_CNTL` (0x930a) di semua A7xx; KGSL di A725 melindunginya → GPU hang → context di-invalidate (EDEADLK) | Properti device baru `no_vpc_unknown_cntl` khusus FD725 di freedreno+turnip, `mesa-a725-kgsl-protected-reg.patch` (`583814c`) |
| (dugaan salah) | Tambahan 32 baris di buffer gralloc karena dugaan GPU menulis lewat batas buffer | Dicabut lagi di `583814c` |

## Arsitektur saat ini

```
/sbin/init (waydroid/src/ds-waydroid-init.c, statis)
  ├─ /dev bersih, binderfs privat, proteksi host (/proc/sys, /sys ro, cgroup, caps)
  ├─ pilih GPU/CPU (gpu.mode=auto|on|off; auto = /dev/kgsl-3d0 + dma-heap + file driver ada,
  │   dan /data/ds-gpu-failed tidak ada)
  ├─ tulis /vendor/waydroid.prop (GPU: ro.hardware.gralloc=gbm, gralloc.gbm.legacy=true,
  │   ro.hardware.egl=kgsl, mesa.loader.driver.override=kgsl; CPU: default/swiftshader)
  └─ exec Android /init
Android
  ├─ anland-waydroid-bridge (waydroid/src/anland-waydroid-bridge.c, statis, early-init)
  │   compositor Wayland mini: wl_shm + zwp_linux_dmabuf_v1 → salin ke buffer Anland
  │   (CPU, atau GPU lewat /vendor/bin/anland-gpu-copy bila bridge.gpu_copy=auto/1)
  ├─ ds-gpu-guard.rc/.sh: SurfaceFlinger crash 3x di mode GPU → simpan log,
  │   tulis /data/ds-gpu-failed, reboot Android → start berikutnya mode CPU
  └─ ds-logcat.rc: logcat ke /data/ds-waydroid-logcat.txt (start sebelumnya: *.previous.txt)
```

File GPU yang dipasang ke `/vendor` (dibangun `waydroid/build-gpu-drivers.sh`, NDK, API 30):
`lib64/egl/lib{EGL,GLESv1_CM,GLESv2}_kgsl.so`, `lib64/libgallium_kgsl.so`
(rename dari `libgallium_dri.so` agar tidak bentrok dengan Mesa Waydroid),
`lib64/hw/gralloc.gbm.so` (pengganti, dari `waydroid/gpu/gralloc_dmaheap.c`,
alokasi dari `/dev/dma_heap/system`, layout handle = libdrm `gralloc_handle_t`),
`lib64/hw/vulkan.kgsl.so`, `bin/anland-gpu-copy`.

Mesa: `lfdevs/mesa-for-android-container` @ `4c94f3e82f91336609830481a27326cb0bae1f78`
+ semua `waydroid/gpu/mesa-*.patch`.

## Tambalan yang merupakan inisiatif agent (kandidat dibuang/disederhanakan)

Pemilik menganggap ini "ducktape" yang tidak diminta:
- `ds-gpu-guard` (fallback otomatis ke CPU) dan file `ds-gpu-failed*`;
- `ds-logcat.rc` (logcat ke file);
- mode `auto` untuk `render_scale`/`max_fps`/`gpu_copy` dan deteksi GPU otomatis;
- `anland-gpu-copy` (opsi "A": GPU yang menyalin frame ke Anland). Ini diminta
  pemilik, tetapi sebaiknya ditunda sampai jalur GPU dasar stabil.

Saran agent: untuk rootfs generik yang aman dipasang di ponsel mana pun,
jadikan **mode CPU sebagai bawaan** (`gpu.mode=off`) sampai jalur GPU Adreno
terbukti stabil, lalu buang lapisan di atas yang tidak dibutuhkan.

## Build dan uji

- CI: workflow `Build Waydroid RootFS` (`.github/workflows/build-waydroid-rootfs.yml`),
  input `abi`, `system_type`, `gpu`, `publish_release`. Job `gpu-drivers` jalan di
  `ubuntu-24.04` (NDK x86-64) dan di-cache berdasarkan hash `build-gpu-drivers.sh`,
  `waydroid/gpu/**`, `waydroid/src/gpu-copy-protocol.h`. Run sukses terakhir yang
  diuji di ponsel: #17 (`15aabda`).
- Uji lokal bridge (x86-64, tanpa GPU): `waydroid/build-tools.sh` lalu
  `waydroid/tests/run-bridge-test.sh TOOLS WORK` — mencakup shm, dmabuf 32/16-bit,
  helper GPU tiruan dan helper asli di llvmpipe (EGL surfaceless).
- Uji init: `waydroid/tests/run-init-test.sh` (butuh biner `droidspaces`, tidak jalan di CI).

## Cara mengambil log di ponsel (tanpa nsenter)

`nsenter`/`chroot /proc/PID/root` ke container gagal di ponsel uji
(`/proc/self` milik PID namespace container). Pakai file di rootfs
(`/mnt/Droidspaces/<nama>/` saat container sparse-image berjalan):
`ds-waydroid-boot.log`, `ds-waydroid-bridge.log(.previous)`,
`data/ds-waydroid-logcat.txt` (+ `.previous.txt`), `data/tombstones/`,
`data/ds-gpu-failed.*`, dan `dmesg | grep -iE "kgsl|Protected mode|gpu hang"`
dari host.

## Langkah berikutnya yang disarankan

1. Putuskan dengan pemilik: CPU sebagai bawaan untuk rilis generik, GPU opsional.
2. Untuk kendala 1, ambil `/data/ds-gpu-failed.logcat.txt` dari satu start GPU
   dengan `mesa.egl.log.level=debug` dan `mesa.fd.mesa.debug=layout`, lalu cari
   baris `skia`, `MESA`, `EGL-` tepat sebelum `Unable to generate SkImage`.
3. Kirim perbaikan sebagai paket file kecil yang di-extract ke rootfs yang sudah
   terpasang, bukan impor ulang rootfs 500 MB (pemilik keberatan impor berulang).
