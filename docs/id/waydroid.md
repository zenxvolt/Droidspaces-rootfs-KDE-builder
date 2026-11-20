[English](../en/waydroid.md) · [Beranda proyek](../../README_english.md)

# Waydroid sebagai RootFS Droidspaces (eksperimental)

Ini membangun rootfs yang isinya **adalah** image Android milik Waydroid. Droidspaces mem-boot Android langsung sebagai container: tidak ada distro Linux di sekelilingnya, tidak ada LXC, dan tidak ada container bertingkat. Layarnya tampil di aplikasi Anland.

```text
Container Droidspaces
└── /sbin/init (ds-waydroid-init)        menyiapkan container, lalu menjadi…
    └── init Android (PID 1)             image LineageOS dari Waydroid
        ├── anland-waydroid-bridge  ───► aplikasi Anland (layar ponsel)
        └── SurfaceFlinger → hardware composer Waydroid ──┘
              └── menggambar dengan GPU Adreno (Snapdragon) atau dengan CPU
```

> [!WARNING]
> **Status: eksperimental.** Android sudah boot dan tampil di Anland pada ponsel uji dengan renderer software serta GPU Adreno 725. Gunakan `gpu.mode=off` untuk memilih renderer software di [Pengaturan](#3-pengaturan).

## Yang berfungsi dan yang belum

| Bagian | Keadaan |
| --- | --- |
| Tampilan | Tampil di aplikasi Anland. Di ponsel Snapdragon dengan Hardware Access, Android menggambar dengan GPU Adreno pada resolusi penuh. Di ponsel lain Android menggambar dengan CPU (SwiftShader) pada setengah resolusi dan 30 fps, sehingga terlihat buram dan patah-patah. |
| Sentuhan | Multi-sentuh. |
| Keyboard | Tombol fisik diteruskan. Untuk mengetik, pakai keyboard layar milik Android. |
| Mouse | Klik kiri atau seret diteruskan sebagai ketukan atau usapan jari. Tidak ada kursor dan roda gulir. |
| Akselerasi GPU | Khusus Snapdragon (Adreno): OpenGL ES lewat Mesa freedreno di atas KGSL, dan Vulkan (Mesa turnip) bila diaktifkan. OpenGL ES terverifikasi di Adreno 725. Vulkan masih perlu pengujian pada ponsel. |
| Clipboard | Teks bisa disalin dan ditempel antara Anland dan Android di Waydroid. |
| Suara, kamera, sensor | Belum ada. |
| Jaringan | HTTPS di Jelly berhasil lewat NAT Droidspaces di VM Linux. Timeout TCP di Jelly pada ponsel uji masih terbuka. |

## Syarat

- Ponsel ARM64 yang sudah di-root dengan Droidspaces **yang punya dukungan Anland bawaan** (di pengaturan container ada sakelar **Anland Display**).
- **Aplikasi Anland** dari [rilis Anland](https://github.com/SuperTurtleDev/anland/releases).
- Kernel dengan **binderfs** (`CONFIG_ANDROID_BINDERFS=y`). Kernel 5.10 ke atas umumnya punya. Android butuh perangkat binder miliknya sendiri; binder milik ponsel tidak pernah dipakai bersama.
- Ruang penyimpanan kosong sekitar 8 GB.
- Untuk penggambaran GPU: ponsel **Snapdragon** (GPU Adreno, `/dev/kgsl-3d0`). Kernel 5.10 ke atas juga menyediakan dma-buf heap yang dibutuhkan driver.

## 1. Membangun

1. Buka tab **Actions** di repositori lalu jalankan **Build Waydroid RootFS (Android on Droidspaces via Anland)**.
2. Pilih image:
   - `arm64_only` berjalan di semua ponsel ARM64. Pilih ini bila ragu.
   - `arm64_32` juga menjalankan aplikasi 32-bit, tetapi hanya bisa menyala di prosesor yang masih mendukung kode 32-bit. Banyak prosesor kelas atas terbaru tidak.
3. Biarkan **gpu** menyala. Pilihan ini menambahkan driver Adreno (beberapa puluh MB); ponsel tanpa GPU Snapdragon tetap menggambar dengan CPU. Build pertama dengan pilihan ini sekitar 20 menit lebih lama; build berikutnya memakai ulang driver yang sudah jadi.
4. Unduh berkas `.tar.xz` dari **Releases**.

Untuk membangun sendiri di mesin Linux ARM64:

```bash
sudo ./waydroid/build-waydroid-rootfs.sh
```

Driver GPU dibangun terpisah dengan Android NDK di mesin x86-64, lalu diberikan lewat `-G`:

```bash
./waydroid/build-gpu-drivers.sh -n /path/ke/android-ndk -a aarch64 -o gpu-aarch64
sudo ./waydroid/build-waydroid-rootfs.sh -G gpu-aarch64
```

## 2. Memasang di Droidspaces

1. Impor `.tar.xz` sebagai container baru. Pilih penyimpanan **sparse image** berukuran **8 GB atau lebih**; data Android disimpan di dalam rootfs. Jangan memasangnya sebagai folder biasa: Droidspaces sendiri memperingatkan bahwa rootfs berupa folder di penyimpanan ponsel bisa rusak karena pelabelan SELinux.
2. Di pengaturan container:
   - nyalakan **Anland Display**;
   - nyalakan **mode privileged** dengan `nocaps` dan `noseccomp`;
   - nyalakan **SELinux permissive**;
   - di ponsel Snapdragon, nyalakan **Hardware Access** agar Android bisa memakai GPU;
   - biarkan jaringan di **NAT**;
   - biarkan program init pada bawaannya (`/sbin/init`).
3. Jalankan container, lalu tekan **Launch Anland**.

Boot pertama memakan beberapa menit karena Android menyiapkan aplikasinya. Kamu bisa membuka Anland saat Android mulai. Anland menampilkan layar hitam sampai Android mengirim frame pertama.

## 3. Pengaturan

Semua pengaturan ada di satu berkas di dalam rootfs: `/system/etc/droidspaces-waydroid.conf`. Berkas ini dibaca setiap kali container dijalankan.

| Pengaturan | Arti |
| --- | --- |
| `gpu.mode` | `auto` (bawaan) memakai GPU Adreno bila `/dev/kgsl-3d0` dan dma-buf heap bisa dibuka, selain itu CPU. `off` selalu menggambar dengan CPU. `on` memakai driver GPU walaupun pemeriksaannya gagal (hanya untuk mencari masalah). |
| `gpu.vulkan` | `1` juga menyediakan Vulkan (Mesa turnip) untuk aplikasi dan game. Bawaan `0`. |
| `bridge.render_scale` | Persentase resolusi layar yang dipakai Android untuk menggambar. `auto` (bawaan) berarti `100` dengan GPU dan `50` dengan CPU. `50` menggambar seperempat jumlah piksel lalu memperbesarnya, sehingga terlihat buram. |
| `bridge.ui_width_dp` | Menentukan kepadatan sebelum SurfaceFlinger mulai, berdasarkan sisi pendek `bridge.default_width` / `bridge.default_height` setelah `render_scale` diterapkan. `400` cocok untuk ponsel. Angka lebih kecil memperbesar UI. `ro.sf.lcd_density` eksplisit punya prioritas. |
| `bridge.max_fps` | Batas refresh yang diumumkan ke Android. Dengan `auto`, mode GPU mengikuti refresh Anland dan mode CPU membatasi ke 30 Hz. Nilai angka menetapkan batas eksplisit. |
| `bridge.gpu_copy` | Siapa yang menyalin frame Android ke buffer Anland. `auto` (bawaan) menyerahkannya ke GPU bila Android menggambar dengan GPU. `0` selalu memakai CPU. |
| `bridge.wait_for_display` | Berapa detik Android menunggu aplikasi Anland sebelum memakai ukuran bawaan. |
| baris `kunci=nilai` lainnya | Properti sistem Android. |

Untuk container sparse image yang sedang berjalan, rootfs terpasang di ponsel pada `/mnt/Droidspaces/<nama container>/`.

## Kecepatan dan panas

Cara Android menggambar menentukan hampir semuanya:

- **Dengan GPU** (ponsel Snapdragon, Hardware Access menyala), Android menggambar pada resolusi layar penuh dan mengikuti refresh yang dilaporkan Anland. Buka Anland saat boot agar composer Waydroid mengambil refresh tersebut saat mulai. FPS gambar baru bergantung pada beban render dan penyalinan. Gunakan `bridge.max_fps` untuk memilih batas yang lebih rendah.
- **Dengan CPU** (ponsel lain, atau `gpu.mode=off`), SwiftShader menggambar setiap piksel. Karena itu rootfs lalu hanya menggambar seperempat jumlah piksel (`render_scale` 50) pada 30 fps: gambarnya buram dan animasinya patah-patah, tetapi ponsel tetap bisa dipakai. Naikkan keduanya di berkas pengaturan bila ingin gambar lebih tajam dan halus, dengan akibat lebih panas.

Pada kedua mode:

- Penyalaan pertama adalah yang terberat: Android mengompilasi aplikasinya sekali. Penyalaan berikutnya lebih ringan.
- Di Android, **Settings → System → Developer options**, menyetel tiga skala animasi ke `0.5x` atau mati menghilangkan banyak pekerjaan menggambar.
- Droidspaces bisa membatasi jatah prosesor untuk container (batas CPU di pengaturan container) bila ponsel terlalu panas.

`/ds-waydroid-boot.log` mencatat mode mana yang dipilih (baris berawalan `gpu:`). `/ds-waydroid-bridge.log` menulis `drawn by the GPU` atau `drawn by the CPU` saat frame pertama datang, dan `copied by the GPU` atau `copied by the CPU` saat frame itu sampai di Anland.

### Benchmark frame rate dan akses GPU

Buka Anland dan gulir layar yang sama selama 15 detik. Ganti `Waydroid` dengan nama container jika berbeda.

```sh
su -c 'R="/mnt/Droidspaces/Waydroid"; sh "$R/system/bin/ds-waydroid-benchmark" "$R" 15'
```

- **Android FPS** menghitung frame dari composer. **Anland new FPS** menghitung frame baru yang bridge kirim melalui protokol Anland. Hitungan kedua mengabaikan pengulangan frame, ACK layar kosong, dan pengisian ulang buffer bergilir. FPS panel ponsel berada di luar cakupan pengukuran ini.
- **Configured rendering**, hasil pembukaan `/dev/kgsl-3d0`, dan jumlah frame `dma-buf` menunjukkan jalur GPU yang tersedia dan beroperasi. **Buffer copies** menunjukkan pekerjaan penyalinan oleh CPU atau GPU. Jika dijalankan di dalam shell Waydroid dengan `/system/bin/ds-waydroid-benchmark / 15`, laporan juga menyertakan renderer GLES SurfaceFlinger jika tersedia.

## 4. Bila tidak berjalan

Jawabannya ada di tiga tempat, sesuai urutan kejadian:

| Di mana | Isinya |
| --- | --- |
| `/ds-waydroid-boot.log` di dalam rootfs | Apa yang ditemukan dan dilakukan `/sbin/init` sebelum Android dimulai. Masalah ditandai baris berawalan `ERROR:`. Laporan boot sebelumnya disimpan sebagai `ds-waydroid-boot.previous.log`. |
| `dmesg` di ponsel (sebagai root) | Pesan awal dari Android sendiri, baris berawalan `init:`. Baris berawalan `anland-bridge` berasal dari jembatan tampilan. |
| `/ds-waydroid-bridge.log` di dalam rootfs | Jembatan tampilan: apakah aplikasi Anland tersambung dan pada ukuran berapa. |

Setelah Android menyala, log-nya juga ditulis ke `/data/ds-waydroid-logcat.txt` di dalam rootfs (untuk container sparse image yang sedang berjalan: `/mnt/Droidspaces/<nama container>/data/ds-waydroid-logcat.txt`), jadi bisa dibaca dari ponsel tanpa masuk ke container. Log dari start sebelumnya disimpan sebagai `ds-waydroid-logcat.previous.txt`, dan log bridge sebagai `ds-waydroid-bridge.log.previous`; ini penting setelah peralihan otomatis ke penggambaran CPU.

| Pesan | Arti |
| --- | --- |
| `binder: cannot mount binderfs` | Kernel tidak punya binderfs, atau container tidak diizinkan memasangnya. Periksa mode privileged dan SELinux permissive. Tanpa binderfs di kernel, ini tidak bisa berjalan. |
| `processor cannot run 32-bit programs` | Pakai rootfs `arm64_only`. |
| `Android was started 5 times within three minutes` | Android terus-menerus restart. Penyebabnya ada di `dmesg`. Penyalaan dihentikan sementara untuk menghemat baterai; jalankan container lagi untuk mencoba ulang. |
| `cannot reach the Anland daemon at /run/display.sock` | **Anland Display** mati untuk container ini. |
| `cannot map Anland buffer` | Buffer layar ponsel ini tidak bisa ditulis oleh CPU, padahal jembatan ini membutuhkannya. |
| `gpu: there is no /dev/kgsl-3d0` | Android menggambar dengan CPU. Di ponsel Snapdragon, nyalakan **Hardware Access** di pengaturan container. |
| `gpu: no dma-buf heap ... can be opened` | Kernel tidak punya `/dev/dma_heap/system` maupun `/dev/ion`, atau Hardware Access tidak meneruskannya. Android menggambar dengan CPU. |
| `Waited one second for android.hardware.media.omx@1.0::IOmxStore/default` terus berulang | Manifest vendor mengiklankan layanan codec OMX yang tidak disertakan. Builder membuang entri yang tidak punya layanan pada image Waydroid Mainline yang menyertakan software Codec2. |
| `GPU copy turned off (...)` di log bridge | GPU tidak bisa menyalin frame ke buffer Anland, dengan alasan di dalam kurung. CPU yang menyalin sebagai gantinya, jadi gambar tetap tampil tetapi lebih boros baterai. Mohon laporkan pesannya. |
| `gpu: the GPU driver failed before` di log boot | SurfaceFlinger crash tiga kali dengan driver GPU, jadi Android dimulai ulang dengan penggambaran CPU (GPU fault juga membuat layar ponsel sendiri tersendat). Hapus `/data/ds-gpu-failed` di dalam rootfs, atau setel `gpu.mode=on`, untuk mencoba GPU lagi. Log dari start yang gagal disimpan sebagai `/data/ds-gpu-failed.logcat.txt` dan `/data/ds-gpu-failed.bridge.log`; sertakan keduanya saat melapor. |
| Layar hitam atau Android terus restart setelah beralih ke GPU | Setel `gpu.mode=off` di berkas pengaturan untuk kembali ke penggambaran CPU, lalu kirim baris `logcat` dari `SurfaceFlinger`, `libEGL`, `MESA`, dan `gralloc.dmaheap`. |
| `gralloc: couldn't create ashmem` | Android gagal membuat buffer grafis. Build terbaru mengaktifkan `sys.use_memfd=true` secara bawaan, termasuk saat mode hardware menyediakan perangkat ashmem. Pada image lama, tambahkan `sys.use_memfd=true` ke `/system/etc/droidspaces-waydroid.conf` lalu mulai ulang container. |

Log bridge mencatat tiga tahap. `Anland app connected` berarti bridge sudah memetakan buffer aplikasi. `received Android frame` berarti hardware composer Waydroid sudah mengirim frame. `presented the first Android frame to Anland` berarti bridge sudah menyalin frame dan membalas permintaan buffer Anland. Jika log berhenti di `Anland app connected`, periksa pesan hardware composer dan `SurfaceFlinger` di `logcat` dalam container.

Saat melaporkan masalah, cara termudah adalah **Generate a bug report** di aplikasi Droidspaces. Ia menulis satu arsip ke penyimpanan ponsel berisi log container (yang memuat laporan `/sbin/init`), log kernel, dan catatan kernel tentang crash terakhir bila ponsel sempat restart.

## Cara kerjanya

Waydroid biasanya butuh alat Python-nya dan LXC di host Linux. Di sini dua program statis kecil menggantikannya:

- **`ds-waydroid-init`** (`/sbin/init`) membuat `/dev` yang bersih dengan instance binder privat, menyembunyikan command line kernel ponsel dari Android tamu, memilih antara penggambaran GPU dan CPU, menulis `/vendor/waydroid.prop` dari berkas pengaturan, lalu menjalankan `/init` milik Android yang tetap menjadi PID 1.
- **`anland-waydroid-bridge`** adalah compositor Wayland minimal. Hardware composer Waydroid adalah klien Wayland; jembatan ini menerima frame-nya (shared memory dari jalur CPU, linux-dmabuf dari jalur GPU), menyalinnya ke buffer yang dipilih aplikasi Anland, dan mengubah input Anland menjadi input Wayland. Program ini memakai `libdisplay_producer` milik Anland, jadi binernya berlisensi GPL-3.0.

### Jalur GPU

Mesa di dalam image Waydroid hanya bisa berbicara dengan GPU lewat antarmuka Linux mainline (`/dev/dri`). Kernel ponsel menjalankan GPU Adreno lewat antarmuka `kgsl` milik Qualcomm, jadi rootfs menambahkan driver sendiri. Driver ini dibangun dengan Android NDK dari pohon Mesa Adreno yang sama dengan yang dipakai rootfs Linux di repositori ini ([mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container)):

| Berkas | Peran |
| --- | --- |
| `/vendor/lib64/egl/libEGL_kgsl.so`, `libGLESv1_CM_kgsl.so`, `libGLESv2_kgsl.so`, `/vendor/lib64/libgallium_kgsl.so` | OpenGL ES: Mesa freedreno di atas KGSL. Beberapa patch kecil di `waydroid/gpu/` membuat platform EGL Android milik Mesa bisa membuka `/dev/kgsl-3d0`, membuat freedreno mau menerima buffer dma-buf Android di KGSL, dan mencegah Mesa menulis register yang dilindungi KGSL pada Adreno 725. |
| `/vendor/lib64/hw/vulkan.kgsl.so` | Vulkan: Mesa turnip di atas KGSL (hanya dengan `gpu.vulkan=1`). |
| `/vendor/lib64/hw/gralloc.gbm.so` | Menggantikan alokator Waydroid yang membutuhkan `/dev/dri`. Buffer grafis diambil dari dma-buf heap kernel, yang bisa diimpor GPU lewat KGSL dan dipetakan oleh bridge. |
| `/vendor/bin/anland-gpu-copy` | Menyalin frame ke buffer Anland dengan GPU atas nama bridge. |

Saat GPU dipilih, `/sbin/init` menyetel `ro.hardware.gralloc=gbm`, `gralloc.gbm.legacy=true` (tanpa ini init Waydroid mengganti `gbm` dengan alokator minigbm miliknya yang butuh GPU DRM), `ro.hardware.egl=kgsl`, dan `mesa.loader.driver.override=kgsl`. Menyetel salah satu properti tersebut di berkas pengaturan akan mengesampingkan pilihan otomatis.

Hardware composer Waydroid lalu menyerahkan setiap frame yang sudah jadi ke bridge sebagai dma-buf. Bridge sendiri adalah program statis tanpa driver GPU (ia harus berjalan sebelum Android menyala), jadi pada frame GPU pertama ia menjalankan `anland-gpu-copy`, yang ditautkan ke EGL Android. Helper ini mengimpor frame Android dan buffer layar aplikasi Anland sebagai EGL image, lalu menyalin dengan satu kali draw, sekaligus menskalakan gambar bila `render_scale` di bawah 100. Fence tanda selesai dari GPU dikembalikan ke bridge lalu diteruskan ke Anland, sama seperti backend KDE dan GNOME di rootfs Linux menyerahkan frame-nya: tidak ada pihak yang menunggu GPU di CPU. Bila helper gagal, bridge menyalin dengan CPU seperti pada mode CPU.

Rootfs desktop selangkah lebih jauh: compositor-nya menggambar langsung ke buffer Anland tanpa salinan sama sekali. SurfaceFlinger Android memilih buffer-nya sendiri, sedangkan Anland yang menentukan buffer mana yang digambar berikutnya, jadi di sini tetap ada satu salinan oleh GPU.

Builder menghapus pintasan laporan bug melalui tombol fisik LineageOS agar Android init tidak macet saat memindai FIFO input Waydroid. Perintah Android `dumpstate` tetap tersedia.

## Cara ponsel dilindungi dari Android tamu

Skrip boot Android menganggap dirinya pemilik mesin. Ia menyetel penjadwal dan pengelola memori lewat `/proc/sys`, menyalakan kernel panic untuk tugas yang macet, mengatur ulang grup CPU, dan init Android bisa meminta kernel untuk crash lewat `/proc/sysrq-trigger`. Di host Waydroid biasa, LXC membuat semua itu hanya-baca. Di container Droidspaces mode privileged tidak ada yang menahannya, dan "mesin" itu adalah ponsel Anda.

Karena itu `/sbin/init` membangun ulang perlindungan LXC sebelum menjalankan Android:

- `/proc/sys`, `/proc/sysrq-trigger`, dan `/sys` hanya-baca bagi Android;
- Android mendapat subtree cgroup sendiri, sehingga tidak bisa melihat atau menulis ulang grup `top-app`, `background`, dan lainnya milik ponsel;
- kapabilitas dikurangi menjadi kumpulan yang dipertahankan konfigurasi LXC Waydroid (tanpa modul kernel, tanpa akses hardware mentah).

Arsip rootfs yang dibangun sebelum ini ditambahkan tidak punya perlindungan tersebut. Jangan dipakai.

## Apa yang sudah diuji

Di mesin Linux x86-64, dengan skrip di `waydroid/tests/`:

- `run-bridge-test.sh`: daemon Anland asli, tiruan aplikasi Anland, dan tiruan composer Waydroid. Memeriksa warna, penskalaan, input, pergantian ukuran saat Anland tersambung belakangan, kelancaran koneksi sebelum frame pertama dan saat layar Android diam, frame dma-buf ala GPU dengan baris berpadding, helper penyalin GPU (helper asli di renderer software Mesa, dan tiruan untuk fence dan kegagalan), serta pengaturan `auto` untuk kedua mode penggambaran.
- `run-init-test.sh`: `ds-waydroid-init` di dalam container Droidspaces sungguhan pada mode biasa, akses hardware, dan privileged, dengan tiruan yang melakukan pemanggilan yang sama seperti init tahap pertama Waydroid.
- Rootfs ARM64 asli mencapai home screen di Droidspaces lewat emulasi CPU. Gallery, Files, clipboard teks dua arah, dan halaman HTTPS uji melalui NAT terlihat berfungsi di Anland.

Lihat [catatan progres](../../waydroid/PROGRESS.md) untuk pengujian Adreno 725 dan kendala Jelly di ponsel.
