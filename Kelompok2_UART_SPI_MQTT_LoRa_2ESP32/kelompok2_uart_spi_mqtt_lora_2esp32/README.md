# Kelompok 2 — working two-ESP implementation (wire v2)

Source ini berbasis paket tim dan telah diperbaiki pada repo ini. Versi impor tetap tersedia di riwayat git. **Bukan klaim hardware telah berhasil.**

```text
Laptop A USB → A UART → B SPI → A MQTT (broker Laptop A)
→ B E220 LoRa → A Wi-Fi TCP → Laptop B
```

Panduan utama yang konsisten dengan source saat ini: [docs/LOCAL_WORKFLOW.md](../../docs/LOCAL_WORKFLOW.md). Mulai dari [README root](../../README.md), bukan dengan mengedit password ke `.ino`.

Dari **root repository**:

```sh
./run_sender.sh preflight
./run_sender.sh configure      # input privat; local_config.h ignored pada A/B
./run_sender.sh build          # compile saja, tidak upload
./run_receiver.sh --bind 0.0.0.0 --port 5001 --output ./received_images  # Laptop B
./run_sender.sh                # Laptop A; setelah upload manual + safety approval
```

## Pin tetap

- UART: A TX17→B RX16, B TX17→A RX16, common GND.
- SPI: B master / A slave, SCK18 MISO19 MOSI23 CS5; A READY4→B READY4. Mode1, 250kHz; DMA256byte, frame200byte + padding56byte.
- E220 tiap ESP: modul TXD→RX26, RXD←TX27, AUX34, M0=32, M1=33. UART115200 8N1. Verifikasi model, catu/level logika, antena dan setting radio bersama lab sebelum menyalakan. Jangan menebak wiring catu dari nama VIN/5V.
- Jeda minimum transmit10detik tetap; bukan jaminan kepatuhan duty-cycle.

`E220_Config` adalah utility referensi yang **menulis konfigurasi radio**, bukan plug-and-play prerequisite untuk langsung di-upload. Build launcher hanya mengompilasinya. Setting frekuensi/daya tidak diubah; readback/manual/model/aturan lab tetap harus diperiksa sebelum penggunaan.

## Perubahan kompatibilitas

Wire v2: magic `K2I2`, header19byte, payload maksimum181byte, session nonzero uint32 pada offset11..14, CRC offset15..18. ACK teks `ACK:<session>:<seq>`. START/DATA/END/ACK=1/2/3/4, ABORT5, ERROR6. **Update kedua board dan script bersamaan**; paket lama `K2IM`/header15 tidak kompatibel. Detail format/retry/recovery ada di panduan utama.

`protocol.py` dipakai kedua script host; `protocol.h` A/B identik dan diuji lintas bahasa. Header sanitasi `config.example.h` dipakai jika header privat belum dibuat (compile-only, endpoint kosong). `mosquitto.conf` lama adalah referensi, bukan config yang dipakai launcher: managed broker memakai bind IP spesifik dan lifecycle terbatas pada invocation sender. Jangan expose anonymous broker ke internet.

Firmware A/B/E220 telah dikompilasi untuk ESP32 core3.3.12, ESP32DMASPI0.8.0 dan PubSubClient2.8. Tes TCP/MQTT lokal dan kontrak native/source bukan pengujian board/radio. Persetujuan topologi dua-node dan semua uji fisik tetap diperlukan.
