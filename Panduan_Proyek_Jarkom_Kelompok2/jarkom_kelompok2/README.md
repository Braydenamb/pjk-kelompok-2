# Proyek Responsi Jaringan Komunikasi - Kelompok 2

## Arsitektur
Laptop A --USB UART--> ESP32-A --SPI--> ESP32-B --MQTT--> Mosquitto broker di Laptop A
Mosquitto broker Laptop A --MQTT--> ESP32-A --LoRa E220--> ESP32-B --USB UART--> Laptop B

Dengan dua ESP32, data kembali ke ESP32-A setelah tahap MQTT, lalu dikirim ke ESP32-B melalui LoRa. Jadi seluruh byte gambar melewati UART, SPI, MQTT, dan LoRa. Ini merupakan arsitektur relay/loopback. Jika dosen mensyaratkan jalur fisik satu arah tanpa kembali ke node sebelumnya, diskusikan kebutuhan node tambahan dengan asisten.

## Isi folder
- `ESP32_A/ESP32_A.ino`: ESP32-A (UART receiver, SPI master, MQTT subscriber, LoRa transmitter)
- `ESP32_B/ESP32_B.ino`: ESP32-B (SPI slave, MQTT publisher, LoRa receiver, UART output)
- `Laptop_A/send_image.py`: pengirim file gambar
- `Laptop_B/receive_image.py`: penerima dan penyimpan file gambar
- `mosquitto.conf.example`: konfigurasi broker contoh

## Software
1. Arduino IDE 2.x.
2. Board package `esp32 by Espressif Systems` (disarankan versi 3.3.12 untuk contoh ini).
3. Library Arduino `PubSubClient` oleh Nick O'Leary.
4. Mosquitto broker pada Laptop A.
5. Python 3 dan pySerial pada kedua laptop: `python -m pip install pyserial`.

WiFi, SPI master/slave driver, dan HardwareSerial sudah disediakan oleh core ESP32. E220 digunakan dalam mode transparent UART sehingga sketch tidak memerlukan library LoRa tambahan.

## Konfigurasi sebelum upload
Pada kedua sketch, ubah:
- `WIFI_SSID`
- `WIFI_PASS`
- `MQTT_BROKER` menjadi IPv4 Laptop A

Keduanya harus menggunakan WiFi yang sama dengan Laptop A. Laptop B juga harus berada di jaringan yang sama agar dapat menerima serial dari ESP32-B melalui USB.

## Konfigurasi Mosquitto
Contoh isi file konfigurasi:

```conf
listener 1883 0.0.0.0
allow_anonymous true
persistence false
```

Jalankan broker dengan file konfigurasi tersebut dan izinkan koneksi TCP port 1883 pada Windows Firewall untuk jaringan privat. `allow_anonymous true` hanya untuk demonstrasi di jaringan lokal yang dipercaya; jangan gunakan konfigurasi ini pada jaringan publik.

## Pin ESP32-A dan ESP32-B
SPI antar-ESP32:
- GPIO23 MOSI ke GPIO23 MOSI
- GPIO19 MISO ke GPIO19 MISO
- GPIO18 SCLK ke GPIO18 SCLK
- GPIO13 CS ESP32-A ke GPIO13 CS ESP32-B
- GPIO27 READY ESP32-B ke GPIO27 input ESP32-A
- GND ke GND

E220 pada masing-masing ESP32:
- E220 TXD -> ESP32 GPIO16 (RX2)
- E220 RXD <- ESP32 GPIO17 (TX2)
- E220 M0 -> GPIO25
- E220 M1 -> GPIO26
- E220 GND -> GND
- E220 VCC -> catu daya stabil sesuai spesifikasi modul
- Pasang antena sebelum memancar.

## Parameter E220
Kedua E220 harus memiliki parameter yang cocok:
- Mode normal/transparent: M1=0, M0=0.
- UART 115200 bps, 8N1.
- Packet length 200 byte atau lebih besar dari 192 byte.
- Air data rate dan channel/frekuensi harus sama.
- RSSI appended dimatikan agar tidak menambahkan byte ke stream.

PENTING: Jangan memakai frekuensi default tanpa memeriksa aturan frekuensi Indonesia. Atur frekuensi dalam rentang yang diizinkan dan daya pancar sesuai regulasi serta arahan asisten/dosen. Modul E220-900T22D memiliki kemampuan RF yang tidak otomatis berarti semua pengaturan dayanya boleh digunakan.

## Urutan pengoperasian
1. Nyalakan Mosquitto broker di Laptop A.
2. Upload `ESP32_A.ino` ke ESP32-A dan `ESP32_B.ino` ke ESP32-B.
3. Pastikan keduanya terhubung ke WiFi dan broker.
4. Jalankan penerima pada Laptop B terlebih dahulu: `python receive_image.py COMx`.
5. Jalankan pengirim pada Laptop A: `python send_image.py COMy path\gambar.jpg`.
6. Setelah selesai, periksa file di folder `received_images` pada Laptop B.

Ganti `COMx` dan `COMy` sesuai port yang muncul di Device Manager atau `serial.tools.list_ports`.

## Catatan pengujian
- Mulai dari file JPG kecil 10-30 KB.
- Setelah stabil, naikkan ke JPG 1280x720 lalu 1920x1080.
- Jangan membuka Serial Monitor Arduino bersamaan dengan Python karena satu port serial tidak dapat dipakai dua program sekaligus.
- Jika baud USB 460800 tidak stabil, ubah `USB_BAUD` pada kedua sketch dan `BAUD` pada kedua script Python menjadi 230400 atau 115200.
- Frame SPI/MQTT/LoRa berukuran 192 byte: 12 byte header + 180 byte data. Ini di bawah batas paket 200 byte yang dinyatakan untuk E220-900T22D.
