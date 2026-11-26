# PJK Kelompok 2 — rancangan final

Acuan tim: **Panduan Proyek Empat Protokol.pdf**.

## Satu-satunya source aktif

[`Panduan_Proyek_Jarkom_Kelompok2/jarkom_kelompok2/`](Panduan_Proyek_Jarkom_Kelompok2/jarkom_kelompok2/)

```text
Laptop A (pengirim + broker Mosquitto)
  → USB/UART → ESP A
  → SPI → ESP B
  → MQTT melalui broker Laptop A → ESP A
  → E220 LoRa → ESP B
  → USB/UART → Laptop B (penerima)
```

ESP A adalah SPI master, MQTT subscriber, LoRa transmitter. ESP B adalah SPI slave, MQTT publisher, LoRa receiver. Tidak ada UART kabel langsung antar-ESP atau penerima TCP dalam rancangan ini.

## Lokasi file

- Firmware pengirim: `Panduan_Proyek_Jarkom_Kelompok2/jarkom_kelompok2/ESP32_A/ESP32_A.ino`
- Firmware penerima: `Panduan_Proyek_Jarkom_Kelompok2/jarkom_kelompok2/ESP32_B/ESP32_B.ino`
- Script pengirim: `Panduan_Proyek_Jarkom_Kelompok2/jarkom_kelompok2/Laptop_A/send_image.py`
- Script penerima: `Panduan_Proyek_Jarkom_Kelompok2/jarkom_kelompok2/Laptop_B/receive_image.py`
- Konfigurasi broker lokal: `Panduan_Proyek_Jarkom_Kelompok2/jarkom_kelompok2/mosquitto.local.conf`
- Gambar pengujian lokal: `Pengujian/Pengujian 1.jpeg`

## Menjalankan pengirim di Linux

Dari root repository:

```bash
cd /home/miq/Documents/Kuliah/PJK
arduino-cli board list
```

Pastikan penerima Nopal sudah berjalan, wiring/daya/radio terverifikasi, dan gunakan port ESP A yang benar. Contoh dengan port terakhir yang ditemukan:

```bash
.venv/bin/python "Panduan_Proyek_Jarkom_Kelompok2/jarkom_kelompok2/Laptop_A/send_image.py" /dev/ttyACM0 "Pengujian/Pengujian 1.jpeg"
```

Script ini memakai argumen posisi `port image`, **bukan** `--port` dan `--file`. Port bisa berubah saat USB dipindah. Tutup Serial Monitor dan pembaca serial lain sebelum menjalankan.

Penerima di laptop teman menggunakan script final dengan port ESP B lokal, misalnya pada Windows:

```text
python receive_image.py COM6
```

`COM6` hanya contoh. Penerima terhubung lewat USB, bukan port TCP 5001.

## Status nyata

- Kedua sketch final berhasil dikompilasi sebelumnya.
- ESP A pernah berhasil di-upload, mengeluarkan READY dan tersambung MQTT.
- Script pengirim telah diperbaiki untuk mengenali READY yang menempel pada boot noise dan membuka serial dengan DTR/RTS tidak aktif. Tes regresinya berada di `tests/test_final_sender_ready.py`.
- Tes pengiriman terakhir melewati READY lalu gagal dengan `ERR_START_PIPELINE`. Pengiriman gambar end-to-end **belum berhasil diverifikasi**; pemeriksaan SPI/MQTT/LoRa masih diperlukan.
- IP broker lokal terakhir: `192.168.1.14:1883`; alamat DHCP dan proses broker harus dicek kembali saat pengujian.
- Kredensial Wi-Fi ESP A berada di `local_config.h` yang diabaikan Git. Jangan mengunggah kredensial atau binary firmware yang memuatnya.
- Pemeriksaan jalur daya VIN board, suplai radio, antena serta izin kanal/daya radio tetap diperlukan. Dokumen panduan bukan jaminan keamanan wiring atau kepatuhan radio.

Tes script final (tidak mengirim radio):

```bash
.venv/bin/python -m unittest discover -s tests -v
```

## Pembersihan rancangan lama

Starter empat-ESP, rancangan dua-ESP lama dengan penerima TCP, sketch SPI alternatif, launcher lama, tools, dokumentasi dan tes khusus rancangan lama telah dihapus dari folder kerja sesuai permintaan. Tidak ada salinan arsip lokal yang dipertahankan dari pembersihan ini. Riwayat commit Git terdahulu tetap ada; pembersihan ini tidak menulis ulang riwayat atau otomatis memperbarui GitHub.

Dokumen panduan, slide kuliah, gambar pengujian, environment Python utama dan kode final dipertahankan.
