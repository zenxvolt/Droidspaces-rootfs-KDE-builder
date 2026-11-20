# Progres Waydroid untuk Droidspaces

Diperbarui 20 November 2026 (malam) untuk serah terima. Branch kerja
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
| `77fcc7b` | `/proc/sys/net` tetap bisa ditulis (seperti `proc:mixed` LXC), `ds-waydroid-diag`. |
| `6ec2576`, `7965426` | Server PulseAudio di bridge ke kanal audio AnLand; direktori socket 0755 walau umask 077. |
| `0fae9d4` | `ds-net-probe` untuk membandingkan TCP sebagai root dan sebagai UID aplikasi. |

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

### Jaringan aplikasi: penyebab yang paling mungkin (20 November, lanjutan)

Laporan dari HP uji (`ds-waydroid-diag`, logcat, dan
`dumpsys connectivity trafficcontroller` dari Android host):

- Jaringan Android di container **sehat**. DHCP dari `172.28.0.1` berhasil,
  network 100 berstatus `VALIDATED`, NetworkMonitor mendapat HTTP 204 dari
  `https://www.google.com/generate_204`, dan private DNS ke 1.1.1.1/8.8.8.8
  tervalidasi. Yang gagal hanya koneksi dari **aplikasi**.
- Android host mengaktifkan chain firewall
  `STANDBY_MATCH OEM_DENY_1_MATCH BACKGROUND_MATCH` (configuration 4616).
  `BACKGROUND_MATCH` adalah allowlist: UID ≥ 10000 yang tidak tercantum dengan
  `BACKGROUND_MATCH` di `uid_owner_map` milik host akan di-drop. Hanya 76 UID
  10xxx di host yang tercantum.
- Program BPF netd milik host terpasang di root cgroup, sehingga juga menilai
  soket di dalam container. UID tidak di-namespace-kan, jadi aplikasi Waydroid
  dengan UID 10xxx dinilai dengan aturan aplikasi host yang kebetulan ber-UID
  sama. UID sistem (< 10000) selalu lolos: itulah sebabnya root, `nc`, DNS
  (resolver), dan NetworkMonitor (1073) berhasil, sedangkan Jelly macet di
  `SYN-SENT`. Di VM Linux tidak ada program itu, jadi Jelly berhasil.
- Sumber netd (`packages/modules/Connectivity`, `bpf/progs/netd.c`):
  `bpf_owner_match()` membebaskan UID sistem, dan **membebaskan loopback dari
  `BACKGROUND_MATCH`** (`if (skb->ifindex == 1) enabledRules &= ~BACKGROUND_MATCH`).
  Denylist `STANDBY`/`OEM_DENY_1` tetap berlaku di loopback, tetapi UID
  aplikasi Waydroid di log (10050–10122) tidak ada di denylist host (entri
  STANDBY terendah 10142).
- Batasan: `cgroupsock/inet_create` netd juga menolak **pembuatan** soket untuk
  UID yang di peta izin host bertanda tanpa INTERNET; host menandai 10000–10095
  dan banyak lagi sebagai `PERMISSION_NONE`. Jelly tetap bisa membuat soket di
  pengujian sebelumnya, jadi versi netd host tampaknya belum menegakkan ini
  untuk container. Tetap perlu diverifikasi per aplikasi.

Rencana yang belum dikerjakan: proxy TCP transparan di dalam Android. Aturan
`iptables -t nat OUTPUT` mengalihkan TCP dari UID 10000–99999 ke proxy yang
berjalan sebagai root di loopback (`SO_ORIGINAL_DST`, juga IPv6 lewat
`ip6tables` atau, bila REDIRECT IPv6 tidak ada, `REJECT` agar aplikasi jatuh ke
IPv4). Proxy membuka koneksi keluar sebagai root, yang lolos filter host. Jalur
aplikasi→proxy lewat loopback, yang bebas dari `BACKGROUND_MATCH`. QUIC (UDP)
tidak tertangani; browser akan jatuh ke TCP. Solusi yang benar (pergeseran
UID lewat user namespace) membutuhkan dukungan di Droidspaces.

`ds-net-probe` sebelumnya membuat soket **sebelum** berganti UID, jadi uji
"as app uid" di laporan lama sebenarnya uji root, dan skrip diag memilih mark
yang salah (`0x0`). Keduanya sudah diperbaiki: soket kini dibuat setelah
berganti UID (diverifikasi lewat pemilik soket di `/proc/net/tcp`), dan mark
diambil dari aturan `.../0x1ffff iif lo uidrange ... lookup eth0` (0x64).

`/sbin/init` membiarkan `/proc/sys/net` tetap bisa ditulis seperti
`proc:mixed` milik LXC. Itu memperbaiki penyimpangan dari Waydroid, tetapi
bukan penyebab masalah jaringan aplikasi.

### Suara: status

Rantai di Waydroid: AudioFlinger → HAL `audio.primary.waydroid` → ALSA device
`pulse` (alsa-plugins 1.2.7.1) → libpulse 12 → `$PULSE_RUNTIME_PATH/native`
(`waydroid.pulse_runtime_path=/run/xdg/pulse`). Waydroid mematikan pemeriksaan
pemilik direktori di libpulse (`pa_make_secure_dir` selalu 0).

Yang sudah dikerjakan:

- `pulse-server.c` di bridge menjawab protokol native PulseAudio di
  `/run/xdg/pulse/native`, mencampur stream secara real-time, dan mengirim PCM
  lewat kanal audio AnLand (`AUDIO_MSG_PCM`, format dari `AUDIO_MSG_FORMAT`
  aplikasi). Lulus skenario `sound` di `run-bridge-test.sh` dengan
  `aplay -D pulse`.
- Bug yang ditemukan di HP: init Android menjalankan service dengan umask 077,
  sehingga `/run/xdg/pulse` menjadi 0700 root dan `audioserver` tidak bisa
  masuk. Logcat: `audio_hw_primary: Failed to open pcm_out after 100 tries`
  lalu `AudioFlinger: write blocked for 2959 msecs`; musik/video tampak macet.
  Sudah diperbaiki (chmod 0755 eksplisit), dengan uji umask 077.

Masih terbuka (build `3287f8f` di HP: musik/video masih merayap):

- `tests/hal-audio-sim.c` meniru panggilan ALSA HAL persis: 4 period, buffer
  1024 frame (≈21 ms) di 48 kHz, lalu `snd_pcm_writei` 1024 frame per panggilan
  (seukuran seluruh buffer). Dengan pola ini pemutaran **macet atau merayap,
  bahkan terhadap PulseAudio 16 asli** (`enable-shm = no`): 0,06 s audio dalam
  2 s. Jadi masalahnya ada pada kombinasi HAL + plugin ALSA pulse dengan buffer
  sekecil itu, bukan hanya pada server bridge.
- Mekanisme di plugin (`pulse/pcm_pulse.c`): `update_ptr()` membatasi ukuran
  yang bisa ditulis ke `tlength - frame_size`, jadi buffer ALSA tidak pernah
  terlihat penuh-kosong 1024 frame; `pulse_hw_params()` memakai
  `tlength = buffer_size`, `minreq = period`, `prebuf = buffer - period`.
  Dengan server bridge, klien juga menulis 4092 byte yang tidak diminta lalu
  menunggu selamanya (`requested_bytes` libpulse negatif).
- Di desktop Waydroid jalan karena server (biasanya PipeWire) membalas atribut
  buffer yang berbeda. Langkah berikutnya: tangkap balasan `CREATE_PLAYBACK_STREAM`
  dan pola `REQUEST` dari pipewire-pulse untuk HAL yang sama, lalu tiru di
  `pulse-server.c` (misalnya tlength/minreq yang lebih besar dan permintaan awal
  yang sesuai). Alternatif: override `pcm.pulse` di
  `/vendor/usr/share/alsa/alsa.conf` dengan buffer yang lebih besar, atau ganti
  HAL.

Validasi perangkat masih diperlukan untuk helper salin GPU, Vulkan Turnip,
`arm64_32`, dan ponsel selain Adreno 725.

## Build dan sumber bukti

- Workflow [Build Waydroid RootFS](../.github/workflows/build-waydroid-rootfs.yml)
  membangun helper, menjalankan regresi bridge, lalu mengemas image Waydroid.
  Profil untuk ponsel uji adalah `abi=arm64_only`, `system_type=VANILLA`,
  dan `gpu=true`. Input `archive_format` memilih `zst` (bawaan), `gz`, atau `xz`;
  aplikasi Droidspaces resmi hanya menerima `.tar.xz`/`.tar.gz` di
  **Install from file** (`FilePickerUtils.kt`), jadi pakai `gz` bila `.tar.zst`
  ditolak. Metadata image, commit builder, revisi AnLand, dan revisi driver
  tersedia di `/system/etc/droidspaces-waydroid-release` dalam arsip.
- `waydroid/tests/run-bridge-test.sh` menguji frame, input, clipboard dua arah,
  refresh, helper salin GPU, dan suara (termasuk umask 077). VM memakai aplikasi Android yang sebenarnya
  dan klien uji protokol AnLand, dengan renderer software.
- Log dari instalasi sparse image tersedia di
  `/mnt/Droidspaces/Waydroid/ds-waydroid-boot.log`,
  `ds-waydroid-bridge.log`, dan `data/ds-waydroid-logcat.txt`. Snapshot kegagalan
  GPU ada di `data/ds-gpu-failed.*`.
