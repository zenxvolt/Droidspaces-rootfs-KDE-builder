[English](../en/waydroid.md) · [Beranda proyek](../../README_english.md)

# Waydroid sebagai RootFS Droidspaces (eksperimental)

Ini membangun rootfs yang isinya **adalah** image Android milik Waydroid. Droidspaces mem-boot Android langsung sebagai container: tidak ada distro Linux di sekelilingnya, tidak ada LXC, dan tidak ada container bertingkat. Layarnya tampil di aplikasi Anland.

```text
Container Droidspaces
└── /sbin/init (ds-waydroid-init)        menyiapkan container, lalu menjadi…
    └── init Android (PID 1)             image LineageOS dari Waydroid
        ├── anland-waydroid-bridge  ───► aplikasi Anland (layar ponsel)
        └── SurfaceFlinger → hardware composer Waydroid ──┘
```

> [!WARNING]
> **Status: belum terbukti di ponsel sungguhan.** Semua tahap sampai titik Android membutuhkan driver binder sudah diuji (lihat [Apa yang sudah diuji](#apa-yang-sudah-diuji)). Mesin uji tidak punya driver binder, jadi boot Android sampai selesai belum bisa diamati. Kemungkinan besar percobaan pertama perlu dikirimi log.

## Yang berfungsi dan yang belum

| Bagian | Keadaan |
| --- | --- |
| Tampilan | Tampil di aplikasi Anland. Digambar oleh CPU (SwiftShader) sehingga lambat; turunkan `bridge.render_scale` bila perlu. |
| Sentuhan | Multi-sentuh. |
| Keyboard | Tombol fisik diteruskan. Untuk mengetik, pakai keyboard layar milik Android. |
| Mouse | Klik kiri atau seret diteruskan sebagai ketukan atau usapan jari. Tidak ada kursor dan roda gulir. |
| Akselerasi GPU | Belum ada. |
| Suara, kamera, sensor, clipboard | Belum ada. |
| Jaringan | Memakai jaringan container Droidspaces. Belum terbukti. |

## Syarat

- Ponsel ARM64 yang sudah di-root dengan Droidspaces **yang punya dukungan Anland bawaan** (di pengaturan container ada sakelar **Anland Display**).
- **Aplikasi Anland** dari [rilis Anland](https://github.com/SuperTurtleDev/anland/releases).
- Kernel dengan **binderfs** (`CONFIG_ANDROID_BINDERFS=y`). Kernel 5.10 ke atas umumnya punya. Android butuh perangkat binder miliknya sendiri; binder milik ponsel tidak pernah dipakai bersama.
- Ruang penyimpanan kosong sekitar 8 GB.

## 1. Membangun

1. Buka tab **Actions** di repositori lalu jalankan **Build Waydroid RootFS (Android on Droidspaces via Anland)**.
2. Pilih image:
   - `arm64_only` berjalan di semua ponsel ARM64. Pilih ini bila ragu.
   - `arm64_32` juga menjalankan aplikasi 32-bit, tetapi hanya bisa menyala di prosesor yang masih mendukung kode 32-bit. Banyak prosesor kelas atas terbaru tidak.
3. Unduh berkas `.tar.xz` dari **Releases**.

Untuk membangun sendiri di mesin Linux ARM64:

```bash
sudo ./waydroid/build-waydroid-rootfs.sh
```

## 2. Memasang di Droidspaces

1. Impor `.tar.xz` sebagai container baru. Pilih penyimpanan **sparse image** berukuran **8 GB atau lebih**; data Android disimpan di dalam rootfs. Jangan memasangnya sebagai folder biasa: Droidspaces sendiri memperingatkan bahwa rootfs berupa folder di penyimpanan ponsel bisa rusak karena pelabelan SELinux.
2. Di pengaturan container:
   - nyalakan **Anland Display**;
   - nyalakan **mode privileged** dengan `nocaps` dan `noseccomp`;
   - nyalakan **SELinux permissive**;
   - biarkan jaringan di **NAT**;
   - biarkan program init pada bawaannya (`/sbin/init`).
3. Jalankan container, lalu tekan **Launch Anland**.

Boot pertama memakan beberapa menit karena Android menyiapkan aplikasinya. Kamu bisa membuka Anland saat Android mulai. Anland menampilkan layar hitam sampai Android mengirim frame pertama.

## 3. Pengaturan

Semua pengaturan ada di satu berkas di dalam rootfs: `/system/etc/droidspaces-waydroid.conf`. Berkas ini dibaca setiap kali container dijalankan.

| Pengaturan | Arti |
| --- | --- |
| `bridge.render_scale` | Persentase resolusi layar yang dipakai Android untuk menggambar. Bawaan `50` membuat Android menggambar seperempat jumlah piksel; gambarnya diperbesar hingga memenuhi layar. `100` berarti resolusi penuh. |
| `bridge.ui_width_dp` | Memilih kepadatan layar Android supaya sisi pendek layar selebar sekian dp. `400` terlihat seperti ponsel biasa; angka lebih besar membuat semuanya lebih kecil. |
| `bridge.max_fps` | Laju frame tertinggi yang boleh dihasilkan Android. Bawaan `30`. |
| `bridge.wait_for_display` | Berapa detik Android menunggu aplikasi Anland sebelum memakai ukuran bawaan. |
| baris `kunci=nilai` lainnya | Properti sistem Android. |

Untuk container sparse image yang sedang berjalan, rootfs terpasang di ponsel pada `/mnt/Droidspaces/<nama container>/`.

## Kecepatan dan panas

Android digambar oleh CPU, sehingga ponsel bekerja keras dan menjadi hangat. Ini keterbatasan utama versi sekarang.

- Rootfs dikirim dengan `bridge.render_scale=50` dan `bridge.max_fps=30`: Android menggambar seperempat jumlah piksel pada setengah laju frame. Naikkan keduanya di berkas pengaturan bila ingin gambar lebih tajam dan halus, dengan akibat lebih panas.
- Penyalaan pertama adalah yang terberat: Android mengompilasi aplikasinya sekali. Penyalaan berikutnya lebih ringan.
- Di Android, **Settings → System → Developer options**, menyetel tiga skala animasi ke `0.5x` atau mati menghilangkan banyak pekerjaan menggambar.
- Droidspaces bisa membatasi jatah prosesor untuk container (batas CPU di pengaturan container) bila ponsel terlalu panas.

Akselerasi GPU belum tersedia. Driver GPU di dalam image Waydroid tidak bisa berbicara dengan GPU ponsel: driver Adreno-nya hanya mendukung antarmuka kernel Linux mainline, bukan antarmuka `kgsl` yang dipakai kernel ponsel, dan untuk Mali tidak ada driver terbuka sama sekali. Karena itu jalur GPU membutuhkan driver yang dibangun terpisah dan hanya mungkin di ponsel Snapdragon (Adreno).

## 4. Bila tidak berjalan

Jawabannya ada di tiga tempat, sesuai urutan kejadian:

| Di mana | Isinya |
| --- | --- |
| `/ds-waydroid-boot.log` di dalam rootfs | Apa yang ditemukan dan dilakukan `/sbin/init` sebelum Android dimulai. Masalah ditandai baris berawalan `ERROR:`. Laporan boot sebelumnya disimpan sebagai `ds-waydroid-boot.previous.log`. |
| `dmesg` di ponsel (sebagai root) | Pesan awal dari Android sendiri, baris berawalan `init:`. Baris berawalan `anland-bridge` berasal dari jembatan tampilan. |
| `/ds-waydroid-bridge.log` di dalam rootfs | Jembatan tampilan: apakah aplikasi Anland tersambung dan pada ukuran berapa. |

Setelah Android menyala, `logcat` bisa dipakai dari shell di dalam container.

| Pesan | Arti |
| --- | --- |
| `binder: cannot mount binderfs` | Kernel tidak punya binderfs, atau container tidak diizinkan memasangnya. Periksa mode privileged dan SELinux permissive. Tanpa binderfs di kernel, ini tidak bisa berjalan. |
| `processor cannot run 32-bit programs` | Pakai rootfs `arm64_only`. |
| `Android was started 5 times within three minutes` | Android terus-menerus restart. Penyebabnya ada di `dmesg`. Penyalaan dihentikan sementara untuk menghemat baterai; jalankan container lagi untuk mencoba ulang. |
| `cannot reach the Anland daemon at /run/display.sock` | **Anland Display** mati untuk container ini. |
| `cannot map Anland buffer` | Buffer layar ponsel ini tidak bisa ditulis oleh CPU, padahal jembatan ini membutuhkannya. |
| `gralloc: couldn't create ashmem` | Android gagal membuat buffer grafis. Build terbaru mengaktifkan `sys.use_memfd=true` secara bawaan, termasuk saat mode hardware menyediakan perangkat ashmem. Pada image lama, tambahkan `sys.use_memfd=true` ke `/system/etc/droidspaces-waydroid.conf` lalu mulai ulang container. |

Log bridge mencatat tiga tahap. `Anland app connected` berarti bridge sudah memetakan buffer aplikasi. `received Android frame` berarti hardware composer Waydroid sudah mengirim frame. `presented the first Android frame to Anland` berarti bridge sudah menyalin frame dan membalas permintaan buffer Anland. Jika log berhenti di `Anland app connected`, periksa pesan hardware composer dan `SurfaceFlinger` di `logcat` dalam container.

Saat melaporkan masalah, cara termudah adalah **Generate a bug report** di aplikasi Droidspaces. Ia menulis satu arsip ke penyimpanan ponsel berisi log container (yang memuat laporan `/sbin/init`), log kernel, dan catatan kernel tentang crash terakhir bila ponsel sempat restart.

## Cara kerjanya

Waydroid biasanya butuh alat Python-nya dan LXC di host Linux. Di sini dua program statis kecil menggantikannya:

- **`ds-waydroid-init`** (`/sbin/init`) membuat `/dev` yang bersih dengan instance binder privat, menyembunyikan command line kernel ponsel dari Android tamu, menulis `/vendor/waydroid.prop` dari berkas pengaturan, lalu menjalankan `/init` milik Android yang tetap menjadi PID 1.
- **`anland-waydroid-bridge`** adalah compositor Wayland minimal. Hardware composer Waydroid adalah klien Wayland; jembatan ini menerima frame-nya, menyalinnya ke buffer yang dipilih aplikasi Anland, dan mengubah input Anland menjadi input Wayland. Program ini memakai `libdisplay_producer` milik Anland, jadi binernya berlisensi GPL-3.0.

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

- `run-bridge-test.sh`: daemon Anland asli, tiruan aplikasi Anland, dan tiruan composer Waydroid. Memeriksa warna, penskalaan, input, pergantian ukuran saat Anland tersambung belakangan, serta kelancaran koneksi sebelum frame pertama dan saat layar Android diam.
- `run-init-test.sh`: `ds-waydroid-init` di dalam container Droidspaces sungguhan pada mode biasa, akses hardware, dan privileged, dengan tiruan yang melakukan pemanggilan yang sama seperti init tahap pertama Waydroid.
- Rootfs ARM64 asli di-boot di Droidspaces lewat emulasi CPU. Init Android menjalankan kedua tahapnya, memuat properti yang dibuat, dan menyalakan jembatan, lalu berhenti di layanan pertama yang butuh binder.

Belum diuji: binderfs privat di dalam Droidspaces pada ponsel, semua hal di Android setelah binder, kecepatan, dan jaringan.
