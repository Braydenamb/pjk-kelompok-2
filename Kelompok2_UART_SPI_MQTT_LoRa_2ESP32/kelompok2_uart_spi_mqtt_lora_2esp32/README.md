# Kelompok 2 – UART -> SPI -> MQTT -> LoRa (2 ESP32)

## Topologi yang dipakai
Laptop A -> USB -> ESP32 A -> UART -> ESP32 B -> SPI -> ESP32 A
-> MQTT (Broker Laptop A) -> ESP32 B -> LoRa E220 -> ESP32 A
-> Wi-Fi/TCP -> Laptop B.

Arah protokol bergantian karena hanya ada dua ESP32. Keempat protokol tetap merupakan link nyata. USB dan TCP adalah antarmuka masuk/keluar ke laptop, bukan protokol utama yang dinilai.

PENTING: minta persetujuan dosen/asisten atas topologi loop dua arah. Jika yang diminta adalah rantai satu arah dengan node khusus per tahap, dua ESP32 tidak cukup.

## Isi folder
- ESP32_A/ESP32_A.ino + protocol.h
- ESP32_B/ESP32_B.ino + protocol.h
- E220_Config/E220_Config.ino
- Laptop_A/send_image.py
- Laptop_B/receive_image.py
- mosquitto.conf

## Hardware dan wiring

### UART antar ESP32 (A -> B dan jalur ACK B -> A)
- A GPIO17 TX -> B GPIO16 RX
- B GPIO17 TX -> A GPIO16 RX
- GND A -> GND B

### SPI (ESP32 B Master -> ESP32 A Slave)
- A GPIO23 MOSI <-> B GPIO23 MOSI
- A GPIO19 MISO <-> B GPIO19 MISO
- A GPIO18 SCK <-> B GPIO18 SCK
- A GPIO5 CS <-> B GPIO5 CS
- A GPIO4 READY -> B GPIO4 READY
- GND A -> GND B
Gunakan kabel pendek. Tambahkan pull-up 10k dari CS GPIO5 ke 3.3V bila perlu.
SPI disetel 250 kHz, Mode 1, transaksi DMA 256 byte. Frame aplikasi 200 byte diletakkan di awal dan 56 byte sisanya padding. Padding ini sengaja disediakan karena catatan ESP32DMASPI menyebut isu 4 byte terakhir pada SPI Slave DMA.

### E220-900T22D (pasang satu modul pada masing-masing ESP32)
- E220 TXD -> ESP32 GPIO26 (RX2)
- E220 RXD <- ESP32 GPIO27 (TX2)
- E220 AUX -> ESP32 GPIO34
- E220 M0 -> ESP32 GPIO32
- E220 M1 -> ESP32 GPIO33
- E220 VCC -> supply 5V stabil eksternal
- E220 GND -> GND supply dan GND ESP32
- Pasang antena sebelum modul diberi daya.
Jangan menyambungkan sinyal UART 5V ke ESP32. Level komunikasi E220 adalah 3.3V. E220 dapat menarik arus TX sekitar 110mA; gunakan supply stabil dengan margin cukup, bukan mengandalkan pin 3V3 ESP32.

### USB dan jaringan
- Laptop A USB ke ESP32 A.
- Laptop B USB ke ESP32 B (daya/debug; data gambar akhir lewat TCP/Wi-Fi).
- Laptop A, Laptop B, ESP32 A, ESP32 B harus berada di LAN/Wi-Fi 2.4 GHz yang sama.
- Nonaktifkan AP/client isolation pada router.
- Laptop A menjalankan Mosquitto Broker.
- Laptop B menjalankan TCP receiver port 5001.

## Software
Arduino IDE 2.x, ESP32 Arduino Core >= 2.0.11, library:
1. ESP32DMASPI (hideakitai)
2. PubSubClient (Nick O'Leary)

Python 3 dan:
    py -m pip install pyserial

Mosquitto Broker: https://mosquitto.org/download/
ESP32DMASPI: https://github.com/hideakitai/ESP32DMASPI
PubSubClient: https://github.com/knolleary/pubsubclient
E220 manual: https://www.ebyte.com/Uploadfiles/Files/2021-7-6/202176188184448.pdf

## Konfigurasi E220 – WAJIB sebelum firmware utama
E220 default pabrik pada manual adalah 873.125 MHz. Jangan langsung mengoperasikan default ini untuk proyek di Indonesia.
1. Hubungkan satu modul E220 ke ESP32 sesuai pin di atas.
2. Upload E220_Config.ino.
3. Buka Serial Monitor 115200.
4. Harapkan respons hex: C1 00 06 00 00 E7 02 46 10.
5. Ulangi pada modul E220 kedua.
6. Matikan daya, lalu upload firmware ESP32_A dan ESP32_B.
Config yang dikirim:
- UART module: 115200, 8N1
- Air data rate: 62.5 kbps
- Sub-packet: 200 byte
- Power: 13 dBm
- Channel 70 = 920.125 MHz
- Transparent mode, LBT enabled
Channel/frekuensi, bandwidth, EIRP (termasuk gain antena), duty cycle, sertifikasi perangkat, dan izin penggunaan wajib dicek terhadap aturan Indonesia dan arahan laboratorium. Peraturan LPWAN 920–923 MHz mencantumkan batas EIRP 20 dBm untuk end node serta duty cycle 1% uplink/downlink. Setting 13 dBm bukan jaminan patuh jika gain antena tinggi; hitung EIRP. Jarak uji dekat tidak menghapus kewajiban aturan spektrum.
Firmware memakai jeda awal 10 detik antarpengiriman LoRa. Ini hanya parameter konservatif awal, bukan pengganti perhitungan airtime/duty cycle. Jangan memperkecil jeda tanpa menghitung airtime dan persetujuan laboratorium.

## Edit konfigurasi IP dan Wi-Fi
Di kedua file .ino:
- WIFI_SSID dan WIFI_PASS
- BROKER_IP = alamat IPv4 Laptop A
Di ESP32_A.ino:
- LAPTOP_B_IP = alamat IPv4 Laptop B

Cari IP lewat CMD:
    ipconfig

Gunakan IP yang tetap/reservasi DHCP. Pastikan keduanya satu jaringan.

## Menjalankan sistem (urutan penting)
1. Jalankan Laptop B:
       py receive_image.py
   Pastikan firewall Windows mengizinkan TCP masuk port 5001 pada jaringan Private.
2. Jalankan Mosquitto di Laptop A dengan file mosquitto.conf:
       mosquitto -c mosquitto.conf -v
   Jika command tidak dikenali, jalankan mosquitto.exe dari folder instalasinya dengan argumen -c dan path config.
3. Upload ESP32_B.ino ke ESP32 B. Buka Serial Monitor 115200 dan tunggu koneksi Wi-Fi/MQTT.
4. Upload ESP32_A.ino ke ESP32 A. Jangan buka Serial Monitor A karena port serial dipakai Python.
5. Izinkan TCP 1883 pada Windows Firewall Laptop A untuk jaringan Private.
6. Jalankan Laptop A:
       py send_image.py --port COMx --file "C:\path\foto.jpg"
   Ganti COMx sesuai port ESP32 A.
7. Script menunggu teks READY dari ESP32 A. Setelah setiap frame melewati seluruh rantai, Laptop A menerima ACK:<seq>.
8. File hasil muncul di Laptop B/received_images.

## Format data
Setiap frame 200 byte:
- 4 byte magic K2IM
- 1 byte tipe (START=1, DATA=2, END=3, ACK=4)
- 4 byte sequence little-endian
- 2 byte payload length
- 4 byte CRC32 atas type+sequence+length+payload
- Maksimum payload 185 byte
Frame yang sama diteruskan antarprotokol. Laptop B memeriksa CRC tiap frame, ukuran total dan CRC seluruh file.

## Pengujian bertahap
Jangan langsung kirim gambar besar.
1. Tes UART dan SPI dengan 1 frame kecil dahulu.
2. Tes broker MQTT dan pastikan ESP32 B menerima.
3. Tes LoRa dengan frame 200 byte, jarak 1–3 meter, antena terpasang.
4. Baru tes gambar JPG kecil, lalu naikkan ukuran.
5. Catat waktu end-to-end, ukuran file, jumlah frame, CRC, jarak LoRa dan konfigurasi radio.

## Catatan keterbatasan
- Ini adalah firmware referensi untuk integrasi; wajib diuji di perangkat nyata. Tidak ada klaim bahwa firmware sudah dikompilasi atau diuji pada board fisik pengguna.
- ESP32DMASPI memiliki catatan isu mode SPI/byte terakhir pada DMA Slave. Bila CRC SPI gagal, turunkan clock, pastikan wiring, lalu uji mode 0 atau 3 sesuai hasil uji; jangan langsung lanjut ke MQTT.
- PubSubClient menerbitkan MQTT QoS 0. Sistem ini menggunakan READY dan ACK aplikasi serta stop-and-wait; jangan mengirim frame bertubi-tubi.
- LoRa E220 adalah UART TTL ke modul, tetapi link radio antar-modul adalah LoRa. UART pertama di proyek adalah link kabel langsung antar-ESP32.
- LoRa akan menjadi bottleneck. JPEG biasanya lebih masuk akal daripada PNG untuk uji awal. Duty cycle radio dapat membuat transfer gambar besar lama.
- Firmware ini ditujukan untuk demo laboratorium lokal, bukan jaringan publik. Batasi Mosquitto anonymous dan port firewall ke jaringan Private.
