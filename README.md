# PJK Kelompok 2 — UART → SPI → MQTT → LoRa

Dua ESP32 dipakai bergantian untuk pengiriman gambar:

```text
Laptop A (sender + MQTT broker)
  → USB → ESP A → UART → ESP B → SPI → ESP A
  → MQTT melalui broker Laptop A → ESP B
  → E220 LoRa → ESP A → Wi-Fi TCP → Laptop B (receiver)
```

**Workflow software siap dijalankan lokal; belum terverifikasi end-to-end pada hardware.** Tidak ada board USB yang terdeteksi saat pemeriksaan. Password Wi-Fi dan IP receiver belum diberikan; tidak ada flash, perubahan radio, firewall atau Wi-Fi yang dilakukan.

## Mulai dari root repository

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
./run_sender.sh preflight            # inventory; exit 1 bila belum ada board/config
./run_sender.sh configure            # input privat lokal, bukan lewat chat
./run_sender.sh build                # compile A/B/E220; TIDAK upload/configure radio
```

Setelah wiring/power/radio disetujui lab, identitas/port board dicocokkan dan firmware **kedua board** di-upload manual:

```sh
# Laptop B (jalankan dahulu)
./run_receiver.sh --bind 0.0.0.0 --port 5001 --output ./received_images
# Laptop A
./run_sender.sh                      # pilih port + gambar lewat prompt
# atau eksplisit; ganti port/file setelah dicocokkan
./run_sender.sh send --port /dev/ttyUSB0 --file ./small.png --lab-approved
```

[Petunjuk lengkap: prasyarat, pin map, private config, upload manual, broker lifecycle, recovery dan diagnosis](docs/LOCAL_WORKFLOW.md).

- Launcher memakai `.venv` proyek; tidak memilih/flash board diam-diam. Pilihan eksplisit tetap wajib meskipun hanya satu port.
- Broker managed terikat ke IP/port lokal yang dipilih dan hanya hidup selama invocation `send`; dihentikan saat selesai/error/Ctrl-C. Broker external hanya diprobe, tidak disentuh. Anonymous MQTT hanya untuk LAN lab tepercaya.
- Kredensial masuk `local_config.h` ignored mode600, endpoint nonsecret masuk `config.local.json` ignored; contoh header sanitasi disediakan. Jangan commit header lokal atau binary firmware berkredensial.
- Semua komponen memakai **wire v2 `K2I2`** dengan session ID di frame/ACK. Update kedua firmware dan kedua script bersamaan; tidak kompatibel dengan paket referensi v1.
- MQTT receipt punya timeout/retry bounded melalui SPI; LoRa retry exhaustion membersihkan pending dan mengirim error upstream. ABORT/new-session membatalkan kerja lama. Jeda radio minimum 10 detik dan pin map tetap dipertahankan.
- Receiver hanya mempublikasikan file final setelah ukuran+CRC cocok; transfer berjalan/gagal tetap `.part`.

## Bukti software dan batasnya

```sh
.venv/bin/python -m unittest discover -s tests -v
./run_sender.sh build
```

Pengujian mencakup TCP loopback nyata, Mosquitto loopback nyata beserta cleanup proses, kontrak wire Python/C++ dan retry native, source guards SPI, dan unit test launcher/config. **Bukan uji radio/MCU**. Ketiga sketch dikompilasi dengan `esp32:esp32:esp32`, core3.3.12, ESP32DMASPI0.8.0, PubSubClient2.8. Lihat [status/batasan](docs/KNOWN_ISSUES.md).

Implementasi kerja berbasis source tim berada di [`Kelompok2_UART_SPI_MQTT_LoRa_2ESP32/kelompok2_uart_spi_mqtt_lora_2esp32/`](Kelompok2_UART_SPI_MQTT_LoRa_2ESP32/kelompok2_uart_spi_mqtt_lora_2esp32/). Riwayat git menyimpan versi impor. PDF/PPTX/ZIP privat, `.venv`, cache, dan starter alternatif tetap tidak dipublikasikan. Persetujuan asisten atas loop dua-node dan pemeriksaan model/catu/antena/kanal/daya/duty-cycle E220 tetap diperlukan sebelum operasi fisik.
