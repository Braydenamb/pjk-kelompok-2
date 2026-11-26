/*
  JARKOM KELOMPOK 2 - ESP32 B
  Peran: SPI slave -> MQTT publisher -> LoRa receiver -> UART output ke Laptop B.
  Board: ESP32 Dev Module (ESP32 DevKit V1 klasik)
  Core yang disarankan: Espressif ESP32 Arduino 3.3.12
  Library eksternal: PubSubClient
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include "driver/spi_slave.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ====== WAJIB DISESUAIKAN, HARUS SAMA DENGAN ESP32-A ======
const char* WIFI_SSID = "GANTI_NAMA_WIFI";
const char* WIFI_PASS = "GANTI_PASSWORD_WIFI";
IPAddress MQTT_BROKER(192, 168, 1, 14); // IPv4 Laptop A

// ====== PIN ======
static const int PIN_SPI_MOSI  = 23;
static const int PIN_SPI_MISO  = 19;
static const int PIN_SPI_SCLK  = 18;
static const int PIN_SPI_CS    = 13;
static const int PIN_SPI_READY = 27; // output ke GPIO27 ESP32-A

static const int PIN_LORA_RX = 16;  // ESP32 RX2 <- E220 TXD
static const int PIN_LORA_TX = 17;  // ESP32 TX2 -> E220 RXD
static const int PIN_LORA_M0 = 25;
static const int PIN_LORA_M1 = 26;

static const uint32_t USB_BAUD  = 460800;
static const uint32_t LORA_BAUD = 115200; // E220 juga harus dikonfigurasi 115200

static const uint16_t FRAME_MAGIC = 0xA55A;
static const uint16_t CHUNK_SIZE = 180;
static const size_t FRAME_SIZE = 192;
static const uint8_t TYPE_START = 1;
static const uint8_t TYPE_DATA  = 2;
static const uint8_t TYPE_END   = 3;

static const char* TOPIC_FRAME = "jarkom2/frame";
static const char* TOPIC_ACK   = "jarkom2/ack";

struct __attribute__((packed, aligned(4))) Frame {
  uint16_t magic;
  uint8_t type;
  uint8_t reserved;
  uint16_t seq;
  uint16_t len;
  uint32_t crc;
  uint8_t data[CHUNK_SIZE];
};
static_assert(sizeof(Frame) == FRAME_SIZE, "Ukuran Frame harus 192 byte");

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
HardwareSerial LoRaSerial(2);

// Buffer global aligned 4 byte untuk kebutuhan DMA SPI slave.
Frame spiFrame __attribute__((aligned(4)));
Frame loraFrame __attribute__((aligned(4)));

uint32_t crc32Update(uint32_t crc, uint8_t value) {
  crc ^= value;
  for (uint8_t i = 0; i < 8; i++) {
    crc = (crc >> 1) ^ (0xEDB88320UL & (-(int32_t)(crc & 1)));
  }
  return crc;
}

uint32_t calculateFrameCRC(const Frame& frame) {
  const uint8_t* raw = reinterpret_cast<const uint8_t*>(&frame);
  uint32_t crc = 0xFFFFFFFFUL;
  for (int i = 2; i < 8; i++) crc = crc32Update(crc, raw[i]);
  for (uint16_t i = 0; i < frame.len; i++) crc = crc32Update(crc, frame.data[i]);
  return ~crc;
}

bool validFrame(const Frame& frame) {
  if (frame.magic != FRAME_MAGIC || frame.len > CHUNK_SIZE) return false;
  return frame.crc == calculateFrameCRC(frame);
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(400);
}

void connectMQTT() {
  while (!mqtt.connected()) {
    if (mqtt.connect("JARKOM2_ESP32_B")) {
      // ESP32-B adalah publisher untuk frame dan ACK.
    } else {
      delay(1000);
    }
  }
}

bool initSPISlave() {
  spi_bus_config_t busConfig = {};
  busConfig.mosi_io_num = PIN_SPI_MOSI;
  busConfig.miso_io_num = PIN_SPI_MISO;
  busConfig.sclk_io_num = PIN_SPI_SCLK;
  busConfig.quadwp_io_num = -1;
  busConfig.quadhd_io_num = -1;
  busConfig.max_transfer_sz = FRAME_SIZE;

  spi_slave_interface_config_t slaveConfig = {};
  slaveConfig.spics_io_num = PIN_SPI_CS;
  slaveConfig.flags = 0;
  slaveConfig.queue_size = 1;
  slaveConfig.mode = 1; // harus sama dengan SPI master ESP32-A

  return spi_slave_initialize(
      SPI3_HOST, &busConfig, &slaveConfig, SPI_DMA_CH_AUTO) == ESP_OK;
}

bool publishBinary(const char* topic, const uint8_t* data, unsigned int length) {
  for (int attempt = 0; attempt < 5; attempt++) {
    if (!mqtt.connected()) connectMQTT();
    mqtt.loop();
    if (mqtt.publish(topic, data, length)) return true;
    delay(150);
  }
  return false;
}

bool readLoRaFrame(Frame& frame, uint32_t timeoutMs) {
  uint8_t* destination = reinterpret_cast<uint8_t*>(&frame);
  size_t received = 0;
  unsigned long start = millis();

  while (received < FRAME_SIZE && millis() - start < timeoutMs) {
    if (!mqtt.connected()) connectMQTT();
    mqtt.loop();
    if (LoRaSerial.available()) {
      destination[received++] = (uint8_t)LoRaSerial.read();
    } else {
      delay(1);
    }
  }
  return received == FRAME_SIZE;
}

void sendFrameToLaptopB(const Frame& frame) {
  if (frame.type == TYPE_START) {
    uint32_t fileSize = 0;
    char filename[65] = {0};
    memcpy(&fileSize, frame.data, sizeof(fileSize));
    memcpy(filename, frame.data + 4, 64);
    filename[64] = '\0';
    Serial.printf("START|%s|%lu\n", filename, (unsigned long)fileSize);
    Serial.flush();
  } else if (frame.type == TYPE_DATA) {
    // Jangan menambahkan teks/debug ke Serial selama data gambar dikirim.
    Serial.write(frame.data, frame.len);
    Serial.flush();
  } else if (frame.type == TYPE_END) {
    Serial.println("END");
    Serial.flush();
  }
}

void setup() {
  Serial.begin(USB_BAUD);
  delay(800);

  pinMode(PIN_SPI_READY, OUTPUT);
  digitalWrite(PIN_SPI_READY, LOW);

  pinMode(PIN_LORA_M0, OUTPUT);
  pinMode(PIN_LORA_M1, OUTPUT);
  digitalWrite(PIN_LORA_M0, LOW); // E220 normal/transparent mode
  digitalWrite(PIN_LORA_M1, LOW);

  LoRaSerial.begin(LORA_BAUD, SERIAL_8N1, PIN_LORA_RX, PIN_LORA_TX);

  if (!initSPISlave()) {
    // Jangan menulis pesan ke Serial karena Serial dipakai untuk data gambar.
    while (true) delay(1000);
  }

  connectWiFi();
  mqtt.setServer(MQTT_BROKER, 1883);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(60);
  connectMQTT();
}

void loop() {
  // Beri tahu master bahwa slave siap menerima transaksi SPI berikutnya.
  digitalWrite(PIN_SPI_READY, HIGH);

  spi_slave_transaction_t transaction = {};
  transaction.length = FRAME_SIZE * 8;
  transaction.rx_buffer = &spiFrame;
  transaction.tx_buffer = nullptr;

  esp_err_t result = spi_slave_transmit(SPI3_HOST, &transaction, portMAX_DELAY);
  digitalWrite(PIN_SPI_READY, LOW);
  if (result != ESP_OK) return;
  if (!validFrame(spiFrame)) return;

  // Tahap MQTT: ESP32-B menerbitkan frame SPI ke broker di Laptop A.
  if (!publishBinary(TOPIC_FRAME,
                     reinterpret_cast<const uint8_t*>(&spiFrame),
                     FRAME_SIZE)) return;

  // Tahap LoRa: tunggu frame yang sama dikirim balik oleh ESP32-A melalui E220.
  if (!readLoRaFrame(loraFrame, 30000)) return;
  if (!validFrame(loraFrame)) return;
  if (loraFrame.seq != spiFrame.seq ||
      loraFrame.type != spiFrame.type ||
      loraFrame.len != spiFrame.len ||
      loraFrame.crc != spiFrame.crc) return;

  // Keluarkan frame ke Laptop B melalui USB-UART.
  sendFrameToLaptopB(loraFrame);

  // ACK dikirim setelah CRC benar dan data sudah diteruskan ke Laptop B.
  uint16_t ackSequence = loraFrame.seq;
  publishBinary(TOPIC_ACK,
                reinterpret_cast<const uint8_t*>(&ackSequence),
                sizeof(ackSequence));
}
