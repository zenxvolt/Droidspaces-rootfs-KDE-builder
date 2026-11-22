[English](../en/phosh.md) · [Beranda proyek](../../README_english.md)

# Phosh di Fedora 44 (eksperimental)

Profil desktop `Phosh` membangun RootFS Fedora 44 berisi [Phosh](https://phosh.mobi/), antarmuka ponsel dari Purism dan GNOME. Layarnya tampil di aplikasi Anland seperti desktop Anland lainnya.

```text
Aplikasi Anland (layar ponsel)
    ▲ buffer, sentuhan, tombol, clipboard, audio
    │
/run/display.sock  (daemon Anland)
    ▲
phoc-anland        phoc 0.57 + backend wlroots milik Anland
    └── gnome-session --session=phosh
        ├── phosh             layar kunci, bilah atas, daftar aplikasi
        ├── stevia            keyboard layar
        └── aplikasi GNOME    Console, Files, Text Editor, Calculator, Clocks, …
```

> [!WARNING]
> **Status: compositor-nya sudah diuji di ponsel dengan pengganti aplikasi Anland, tetapi sesi Phosh lengkap di aplikasi Anland sungguhan belum pernah terlihat.** Lihat [Apa yang sudah diuji](#apa-yang-sudah-diuji). Kemungkinan besar percobaan pertama perlu dikirimi log.

## Bedanya dengan desktop Anland lain

Desktop Anland lain memasang paket compositor yang dibangun di tempat lain lalu diterbitkan sebagai Release. Paket seperti itu belum ada untuk phoc (compositor Phosh). Karena itu profil ini mengompilasinya saat RootFS dibangun:

- phoc dibangun pada **versi yang sama dengan paket phoc Fedora**, supaya phosh, skema GSettings-nya, dan phoc tetap cocok.
- phoc membawa wlroots sendiri dan menautkannya secara statis. wlroots itu diberi patch wlroots dari Anland (swapchain eksternal, keymap privat untuk teks).
- **Backend wlroots Anland milik Mango** dikompilasi ke dalam phoc tanpa diubah. Lapisan penghubung kecil memberinya seat, display, pointer constraint, dan text-input milik phoc.
- Hasilnya satu program, `/usr/libexec/phoc-anland`. Paket `phoc` dan `wlroots` milik Fedora tidak disentuh.
- Toolchain dihapus lagi setelah build. `phoc` dan `phosh` dikunci pada versi yang dibangun dengan `dnf versionlock`, karena phosh yang lebih baru mungkin butuh phoc yang lebih baru.

Sumbernya ada di [`scripts/anland/`](../../scripts/anland/):

| Berkas | Fungsi |
| --- | --- |
| `build-phoc-anland.sh` | Mengambil phoc dan Anland pada versi tetap, menerapkan patch, dan membangun `phoc-anland`. |
| `phoc/phoc-anland.patch` | Perubahan kecil pada phoc: opsi meson, memilih backend Anland bila `ANLAND_SOCKET` diset, dan jalur render output. |
| `phoc/glue/` | Penghubung antara phoc dan backend Anland yang tidak diubah. |
| `phoc/phosh-anland` | Peluncur sesi (`phosh-session` dengan `phoc-anland`). |
| `install-anland-phosh.sh` | Berjalan saat RootFS dibangun: memasang dependensi build, membangun, memasang, lalu membersihkan. |
| `tests/` | Uji ujung-ke-ujung yang berjalan di ponsel. |

## Membangun

Di GitHub Actions, jalankan workflow RootFS dengan:

- `build_target`: `Fedora-44`
- `desktop`: `Phosh` (display backend otomatis Anland Wayland)

Secara lokal:

```bash
./build_rootfs-native.sh -i Fedora-44.Dockerfile -K Phosh -L true -u <user>
```

Kompilasi phoc dan wlroots membuat build ini jelas lebih lama dibanding desktop Fedora lainnya.

## Persiapan ponsel

Sama seperti desktop Anland lainnya: lihat [Anland Wayland host setup](../en/desktop-and-anland.md#anland-wayland-host-setup). Isinya: modul daemon Anland, aplikasi Anland, akses hardware, mode privileged, dan bind mount `/data/local/tmp/display_daemon.sock -> /run/display.sock`. Opsi GPU Snapdragon (`enable_mesa`) sangat disarankan.

## Menjalankan

Dengan `desktop_autostart`, sesi berjalan saat boot. Untuk menjalankannya manual sebagai pengguna biasa:

```bash
phosh-anland --check   # memeriksa program, socket, dan render node tanpa menjalankan apa pun
phosh-anland
```

| Variabel | Bawaan | Efek |
| --- | --- | --- |
| `PHOC_ANLAND_SCALE` | menyesuaikan lebar 360 piksel logis | Skala layar, misalnya `2.5`. Makin besar, makin besar tombolnya. |
| `PHOC_INI` | `/etc/phosh/phoc.ini`, atau `/usr/share/phosh/phoc.ini` | Konfigurasi phoc. Baris `scale=` di bagian `[output:ANLAND-1]` juga mengatur skala. |
| `ANLAND_SOCKET` | `/run/display.sock` | Socket daemon Anland. |
| `ANLAND_DRM_DEVICE` | `/dev/dri/renderD128` | Render node GPU. |

Peluncur menolak berjalan bila desktop Anland lain milik pengguna yang sama sedang aktif.

## Yang berfungsi dan yang belum

| Bagian | Keadaan |
| --- | --- |
| Tampilan | Digambar oleh GPU langsung ke buffer aplikasi Anland. |
| Sentuhan, keyboard, mouse | Diteruskan ke phoc sebagai perangkat sentuh, keyboard, dan pointer. |
| Skala | Dipilih otomatis dari ukuran layar (lebar 720 px → 2, 1080 px → 3). |
| Teks dari keyboard Android | Dikirim lewat text-input-v3 bila aplikasinya mendukung, jika tidak diketik sebagai penekanan tombol. **Belum diuji.** |
| Clipboard, audio | Disediakan backend Anland seperti di Mango. **Belum diuji.** |
| Night Light | Diabaikan: Anland tidak bisa menerapkan tabel gamma. |
| Rotasi | Aplikasi Anland melaporkan ukuran layar baru dan phoc mengikutinya. **Belum diuji.** |
| Telepon, SMS, data seluler, sensor | Tidak tersedia di dalam container. ModemManager dihapus. |
| Fedora 43 | Tidak didukung: Fedora 43 memakai phoc 0.54 dengan wlroots 0.19, yang tidak menjadi sasaran patch ini. |

## Apa yang sudah diuji

Di ponsel Snapdragon (Adreno 725), di dalam container Droidspaces Fedora 44:

- `build-phoc-anland.sh` berhasil membangun phoc 0.57.0 dengan wlroots 0.20.2 yang di-embed dan di-patch, beserta library producer Anland statis.
- [`tests/run-phoc-anland-test.sh`](../../scripts/anland/tests/run-phoc-anland-test.sh) lolos. Uji ini menjalankan daemon Anland asli di socket privat, aplikasi pengganti yang menyediakan buffer GPU seperti aplikasi aslinya, serta phoc-anland dengan klien uji. Yang diperiksa:
  - gambar sampai ke "aplikasi" dengan warna yang tepat;
  - klien melihat output 720x1280 dengan skala 2 (360x640 logis);
  - sentuhan dan keyboard Anland sampai ke klien di posisi yang benar;
  - phoc berhenti dengan bersih.

Belum diuji: build RootFS Docker itu sendiri, `gnome-session --session=phosh` di dalam container Droidspaces, dan aplikasi Anland sungguhan.

Untuk menjalankan uji di ponsel setelah phoc-anland dibangun (socket Anland asli tidak disentuh):

```bash
scripts/anland/tests/run-phoc-anland-test.sh /usr/libexec/phoc-anland <checkout-anland>
```

## Pemecahan masalah

- `journalctl --user -b` dan `journalctl -u desktop-session -b` menampilkan pesan phoc dan phosh.
- Baris `Using the Anland backend on …` di log berarti phoc menemukan socket-nya. Bila tidak ada, `ANLAND_SOCKET` tidak diset.
- `Software rendering detected` berarti driver Mesa kgsl belum aktif: nyalakan opsi GPU Snapdragon, atau set `WLR_RENDERER_ALLOW_SOFTWARE=1`.
- Tombol terlalu kecil atau terlalu besar: atur `PHOC_ANLAND_SCALE`.
