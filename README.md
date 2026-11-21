# PJK Kelompok 2 — UART → SPI → MQTT → LoRa

Proyek Praktikum Jaringan Komunikasi: pengiriman gambar dari Laptop A ke Laptop B menggunakan **dua ESP32 DevKit V1** dan **dua E220-900T22D**.

## Rancangan yang dipilih

```text
Laptop A (pengirim + broker MQTT)
  -> USB -> ESP A
  -> UART -> ESP B
  -> SPI -> ESP A
  -> MQTT melalui broker Laptop A -> ESP B
  -> LoRa melalui pasangan E220 -> ESP A
  -> Wi-Fi/TCP -> Laptop B (penerima)
```

ESP A dan ESP B digunakan ulang pada tiap tahap; diagram tidak menunjukkan board tambahan. SPI master adalah ESP B, slave adalah ESP A. Kedua board harus tetap berdekatan karena ada link kabel UART dan SPI.

## Source

Implementasi referensi dari paket tim berada di:

[`Kelompok2_UART_SPI_MQTT_LoRa_2ESP32/kelompok2_uart_spi_mqtt_lora_2esp32/`](Kelompok2_UART_SPI_MQTT_LoRa_2ESP32/kelompok2_uart_spi_mqtt_lora_2esp32/)

Baca README di folder tersebut untuk pin map, dependensi, format paket, dan urutan menjalankan sistem. File firmware dan script diimpor dari paket referensi tim; bukan klaim implementasi baru atau hasil pengujian berhasil.

## Status dan batasan

- Rancangan dua-ESP loop dipilih oleh tim; persetujuan asisten atas topologi tetap perlu dipastikan.
- Firmware dua-ESP ini **belum diverifikasi kompilasi, upload, atau pengiriman end-to-end** dalam penyiapan repository ini.
- Hasil compile starter empat-ESP yang lama tidak berlaku sebagai validasi firmware ini.
- E220 memakai UART, bukan antarmuka SPI SX1278. Jangan menggunakan wiring starter lama.
- Periksa konfigurasi kanal, daya, antena, airtime/duty cycle dan ketentuan lab sebelum menyalakan radio. Nilai contoh dalam source bukan jaminan kepatuhan regulasi.
- Konfigurasi broker anonymous hanya untuk LAN laboratorium tepercaya; jangan buka ke internet.
- Jangan commit password Wi-Fi, token, atau kredensial nyata. Source awal menggunakan placeholder.

## Laptop pengirim Linux

Dependensi Arduino: ESP32 board package, ESP32DMASPI, PubSubClient. Jangan menganggap semua dependensi tersedia hanya karena kode sudah ada.

Contoh penyiapan Python (jalankan di folder implementasi):

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install pyserial
python Laptop_A/send_image.py --help
```

Saat pengujian gunakan port serial Linux yang benar, misalnya `/dev/ttyUSB0`, bukan `COM6`. Cocokkan port dengan board fisik sebelum upload. Isi Wi-Fi dan IP broker/penerima secara lokal sebelum pengujian.

PDF percakapan, slide kuliah, ZIP, environment Python, cache, dan alternatif starter lama sengaja tidak dipublikasikan.
