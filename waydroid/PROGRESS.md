# Progres Waydroid untuk Droidspaces

Diperbarui 20 November 2026 untuk serah terima. Branch kerja
`waydroid/gpu-continuation` memuat integrasi Waydroid dan AnLand. Panduan
pengguna tersedia dalam [Bahasa Indonesia](../docs/id/waydroid.md) dan
[bahasa Inggris](../docs/en/waydroid.md).

## Acuan integrasi

- **Waydroid upstream** menyediakan image Android, hardware composer,
  clipboard HAL, dan acuan konfigurasi boot, Binder, cgroup, serta jaringan.
  `ds-waydroid-init` menyiapkan lingkungan itu sebelum menjalankan Android
  `/init`.
- **Droidspaces** menyediakan container, `eth0`, DHCP, dan NAT. Bandingkan
  perilakunya dengan [konfigurasi LXC](https://github.com/waydroid/waydroid/blob/main/data/configs/config_base)
  dan [skrip jaringan desktop Waydroid](https://github.com/waydroid/waydroid/blob/main/data/scripts/waydroid-net.sh).
- **AnLand** menerima frame dari bridge Wayland. Bridge memakai protokol AnLand
  untuk buffer, input, dan clipboard, serta memakai `wl_data_device_manager`
  untuk clipboard teks pada sisi hardware composer Waydroid.
- **Adreno** memakai Mesa freedreno melalui KGSL dan gralloc berbasis dma-buf heap.
  Sumber Mesa dipatok pada [lfdevs/mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container/tree/4c94f3e82f91336609830481a27326cb0bae1f78).
  Paket GPU ARM64 menyertakan EGL/GLES, Vulkan Turnip opsional, dan helper penyalin
  buffer AnLand.

## Status yang terverifikasi

| Area | Hasil |
| --- | --- |
| Boot dan input di HP | Waydroid LineageOS 20 `arm64_only` mencapai home screen di AnLand pada Xiaomi marble (SM7475, Adreno 725, kernel 5.10.236-android12). Sentuhan berfungsi. |
| GPU di HP | Setelah padding gralloc diperbaiki, Android menggambar di Adreno/KGSL pada 1080×2400 dan mengirim frame dma-buf. Skala UI disetel lewat density Android. Pengguna mengonfirmasi animasi lebih mulus setelah bridge mengikuti refresh AnLand 120 Hz. |
| Benchmark GPU | Sampel sebelum perubahan refresh masih mengiklankan 60 Hz pada layar 120 Hz. Sampel 15,761 detik mencatat 504 commit dma-buf, 478 frame baru ke AnLand, dan 532 salinan CPU dengan 0 salinan GPU. Angka itu mengukur sampel beban yang berubah-ubah. Pengiriman frame pada 120 FPS tetap perlu diukur. |
| Gallery dan Files | Pengguna mengonfirmasi keduanya pulih setelah deklarasi OMX yang tidak tersedia dihapus. Keduanya juga terlihat di AnLand pada VM. |
| Clipboard teks | Uji bridge Linux lulus dua arah. Android ARM64 di VM menempel teks dari AnLand di Jelly dan mengirim kembali teks yang disalin dari Jelly. Menu tekan lama pada HP masih perlu dikonfirmasi. |
| Jelly dan jaringan di VM | Android ARM64 dengan renderer software boot di QEMU. Pada Droidspaces 7.2.0 dengan **NAT**, Jelly menerima HTTP 200 lewat HTTPS dan merender halaman server uji di AnLand. Jaringan uji memakai IPv4 dan meneruskan koneksi hanya ke server yang dikendalikan. |

### Perbaikan penting yang sudah masuk

| Commit | Perubahan |
| --- | --- |
| `e5f6833`, `cb68a9e` | Menutup descriptor starter sebelum Android init, lalu mencegah pemeriksaan shortcut keychord LineageOS memblokir di FIFO input. |
| `9c5a349`, `583814c`, `38b963f` | Impor dma-buf pada freedreno/KGSL, register terlindung pada Adreno 725, dan padding tinggi alokasi linear untuk blok GMEM. |
| `d40c14c`, `d03becb` | Density sebelum SurfaceFlinger, benchmark bridge, dan refresh otomatis mengikuti AnLand pada mode GPU. |
| `3dfb37f` | Menghapus deklarasi OMX vendor yang tidak punya layanan pada image Mainline dengan software Codec2. Sebelum perubahan, MediaProvider bisa tertahan menunggu `IOmxStore`. |
| `9e001d6` | Clipboard teks dua arah antara Waydroid dan AnLand. |

## Kendala terbuka

**Jelly masih timeout di HP pada mode NAT.** NetLog menunjukkan DNS berhasil
untuk `google.com` dan `example.com`. Percobaan koneksi TCP IPv4 dan IPv6
berakhir dengan `ERR_CONNECTION_TIMED_OUT` setelah sekitar 129–131 detik.
TLS belum dimulai. Saat pengujian berikutnya, `ss` menampilkan soket UID Jelly
dalam `SYN-SENT` pada port 80 dan 443 dengan `fwmark:0x64` (network ID 100).
Pada perangkat yang sama, `nc` dari shell host dan shell Waydroid berhasil
membuka TCP ke salah satu alamat IPv4 yang sama. Rute Waydroid memakai gateway
`172.28.0.1` lewat `eth0`. Potongan `dumpsys netpolicy` hanya menunjukkan
riwayat status proses Jelly.

VM Linux menyelesaikan alur HTTPS Jelly lewat NAT dengan tabel `eth0` dan
aturan untuk mark `0x64`. Penyebab perbedaan pada host Android masih terbuka.
Batas berikutnya untuk diperiksa adalah jalur SYN dan SYN-ACK antara namespace
Waydroid, penerusan NAT Droidspaces, dan uplink Android host. Pengujian perlu
membandingkan trafik aplikasi dan shell ke tujuan yang sama.

Dugaan berikutnya: Android host di ponsel memasang program BPF netd pada root
cgroup v2. Program itu juga berlaku untuk soket di container dan memfilter
berdasarkan UID. Lalu lintas root, shell, dan DNS (UID sistem) lolos, tetapi UID
aplikasi seperti 10xxx dinilai dengan aturan firewall milik ponsel sendiri
(standby, doze, mode jaringan terbatas). Di VM Linux tidak ada program seperti
itu. Dugaan ini belum terbukti. `ds-waydroid-diag` sekarang menjalankan
`ds-net-probe`, yang membuka TCP sebagai root dan sebagai UID aplikasi, dengan
dan tanpa mark netd:

- gagal hanya bila memakai UID aplikasi, tanpa peduli mark: filter UID (BPF host);
- gagal hanya bila memakai mark: routing kebijakan di dalam Android;
- semua berhasil: masalahnya ada di aplikasi atau di DNS-nya.

Di sisi ponsel, `su -c 'dumpsys connectivity trafficcontroller'` menampilkan
chain firewall yang aktif dan isi `uid_owner_map`; periksa UID aplikasi
Waydroid yang gagal (misalnya UID Jelly).

`/sbin/init` sekarang membiarkan `/proc/sys/net` tetap bisa ditulis seperti
`proc:mixed` milik LXC. Itu memperbaiki penyimpangan dari Waydroid, tetapi tidak
menjelaskan timeout di atas, karena jaringan Android sudah menyala sebelumnya.

Suara: bridge sekarang menjadi server PulseAudio di `/run/xdg/pulse/native`
untuk HAL audio Waydroid dan mengirim suara lewat kanal audio AnLand. Lulus uji
`run-bridge-test.sh` (skenario `sound`); belum diuji di ponsel.

Validasi perangkat masih diperlukan untuk helper salin GPU, Vulkan Turnip,
`arm64_32`, dan ponsel selain Adreno 725.

## Build dan sumber bukti

- Workflow [Build Waydroid RootFS](../.github/workflows/build-waydroid-rootfs.yml)
  membangun helper, menjalankan regresi bridge, lalu mengemas image Waydroid.
  Profil untuk ponsel uji adalah `abi=arm64_only`, `system_type=VANILLA`,
  dan `gpu=true`. Metadata image, commit builder, revisi AnLand, dan revisi driver
  tersedia di `/system/etc/droidspaces-waydroid-release` dalam arsip.
- `waydroid/tests/run-bridge-test.sh` menguji frame, input, clipboard dua arah,
  refresh, dan helper salin GPU. VM memakai aplikasi Android yang sebenarnya
  dan klien uji protokol AnLand, dengan renderer software.
- Log dari instalasi sparse image tersedia di
  `/mnt/Droidspaces/Waydroid/ds-waydroid-boot.log`,
  `ds-waydroid-bridge.log`, dan `data/ds-waydroid-logcat.txt`. Snapshot kegagalan
  GPU ada di `data/ds-gpu-failed.*`.
