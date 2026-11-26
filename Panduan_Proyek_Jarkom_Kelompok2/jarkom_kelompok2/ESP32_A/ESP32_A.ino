/*
  JARKOM KELOMPOK 2 - ESP32 A
  Alur: Laptop A --UART--> ESP32-A --SPI--> ESP32-B --MQTT--> Broker Laptop A
        --MQTT--> ESP32-A --LoRa E220--> ESP32-B --UART--> Laptop B

  Board: ESP32 Dev Module (ESP32 DevKit V1 klasik)
  Core yang disarankan: Espressif ESP32 Arduino 3.3.12
  Library eksternal: PubSubClient
  SPI menggunakan driver ESP-IDF bawaan core ESP32.
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ====== WAJIB DISESUAIKAN ======
// Private local credentials; local_config.h is excluded from Git.
#if __has_include("local_config.h")
#include "local_config.h"
#else
const char* WIFI_SSID = "GANTI_NAMA_WIFI";
const char* WIFI_PASS = "GANTI_PASSWORD_WIFI";
#endif
// Isi dengan IPv4 Laptop A (komputer yang menjalankan Mosquitto).
IPAddress MQTT_BROKER(192, 168, 1, 14);

// ====== PIN ======
static const int PIN_SPI_MOSI  = 23;
static const int PIN_SPI_MISO  = 19;
static const int PIN_SPI_SCLK  = 18;
static const int PIN_SPI_CS    = 13;
static const int PIN_SPI_READY = 27;  // input ESP32-A, dari GPIO27 ESP32-B

static const int PIN_LORA_RX = 16;   // ESP32 RX2 <- E220 TXD
static const int PIN_LORA_TX = 17;   // ESP32 TX2 -> E220 RXD
static const int PIN_LORA_M0 = 25;
static const int PIN_LORA_M1 = 26;

static const uint32_t USB_BAUD  = 460800;
static const uint32_t LORA_BAUD = 115200; // E220 juga harus dikonfigurasi 115200

// ====== PROTOKOL FRAME ======
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
spi_device_handle_t spiDevice = nullptr;

Frame mqttFrame;
bool mqttFrameReady = false;
bool ackReady = false;
uint16_t receivedAckSeq = 0;

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
  // CRC mencakup type, reserved, seq, len (offset 2..7), lalu data sesuai len.
  for (int i = 2; i < 8; i++) crc = crc32Update(crc, raw[i]);
  for (uint16_t i = 0; i < frame.len; i++) crc = crc32Update(crc, frame.data[i]);
  return ~crc;
}

void finalizeFrame(Frame& frame) {
  frame.magic = FRAME_MAGIC;
  frame.reserved = 0;
  frame.crc = calculateFrameCRC(frame);
}

bool validFrame(const Frame& frame) {
  if (frame.magic != FRAME_MAGIC || frame.len > CHUNK_SIZE) return false;
  return frame.crc == calculateFrameCRC(frame);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_FRAME) == 0 && length == FRAME_SIZE) {
    memcpy(&mqttFrame, payload, FRAME_SIZE);
    mqttFrameReady = true;
  } else if (strcmp(topic, TOPIC_ACK) == 0 && length == sizeof(uint16_t)) {
    memcpy(&receivedAckSeq, payload, sizeof(uint16_t));
    ackReady = true;
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(400);
}

void connectMQTT() {
  while (!mqtt.connected()) {
    if (mqtt.connect("JARKOM2_ESP32_A")) {
      mqtt.subscribe(TOPIC_FRAME);
      mqtt.subscribe(TOPIC_ACK);
    } else {
      delay(1000);
    }
  }
}

bool initSPIMaster() {
  spi_bus_config_t busConfig = {};
  busConfig.mosi_io_num = PIN_SPI_MOSI;
  busConfig.miso_io_num = PIN_SPI_MISO;
  busConfig.sclk_io_num = PIN_SPI_SCLK;
  busConfig.quadwp_io_num = -1;
  busConfig.quadhd_io_num = -1;
  busConfig.max_transfer_sz = FRAME_SIZE;

  esp_err_t result = spi_bus_initialize(SPI3_HOST, &busConfig, SPI_DMA_CH_AUTO);
  if (result != ESP_OK) return false;

  spi_device_interface_config_t deviceConfig = {};
  deviceConfig.clock_speed_hz = 1000000; // mulai 1 MHz agar stabil
  deviceConfig.mode = 1;                 // sama dengan SPI slave
  deviceConfig.spics_io_num = PIN_SPI_CS;
  deviceConfig.queue_size = 1;

  result = spi_bus_add_device(SPI3_HOST, &deviceConfig, &spiDevice);
  return result == ESP_OK;
}

bool sendSPIFrame(const Frame& frame) {
  unsigned long start = millis();
  while (digitalRead(PIN_SPI_READY) == LOW) {
    if (millis() - start > 15000) return false;
    delay(1);
  }

  spi_transaction_t transaction = {};
  transaction.length = FRAME_SIZE * 8;
  transaction.tx_buffer = &frame;
  transaction.rx_buffer = nullptr;
  return spi_device_transmit(spiDevice, &transaction) == ESP_OK;
}

// Mengirim satu frame melewati SPI -> MQTT -> LoRa dan menunggu ACK LoRa.
bool sendFramePipeline(Frame& originalFrame) {
  if (!mqtt.connected()) connectMQTT();
  mqttFrameReady = false;
  ackReady = false;

  // Tahap 2: SPI dari ESP32-A (master) ke ESP32-B (slave).
  if (!sendSPIFrame(originalFrame)) return false;

  // Tahap 3: tunggu frame yang dipublikasikan ESP32-B melalui MQTT.
  unsigned long start = millis();
  while (!mqttFrameReady && millis() - start < 30000) {
    if (!mqtt.connected()) connectMQTT();
    mqtt.loop();
    delay(1);
  }
  if (!mqttFrameReady || !validFrame(mqttFrame)) return false;
  if (mqttFrame.seq != originalFrame.seq ||
      mqttFrame.type != originalFrame.type ||
      mqttFrame.len != originalFrame.len ||
      mqttFrame.crc != originalFrame.crc) return false;

  // Tahap 4: kirim hasil MQTT melalui E220 (transparent UART -> LoRa RF).
  size_t written = LoRaSerial.write(
      reinterpret_cast<const uint8_t*>(&mqttFrame), FRAME_SIZE);
  LoRaSerial.flush();
  if (written != FRAME_SIZE) return false;

  // Tunggu ESP32-B mengirim ACK MQTT setelah frame LoRa diterima dan CRC valid.
  start = millis();
  while (millis() - start < 30000) {
    if (!mqtt.connected()) connectMQTT();
    mqtt.loop();
    if (ackReady && receivedAckSeq == originalFrame.seq) return true;
    delay(1);
  }
  return false;
}

bool readUsbLine(String& line, uint32_t timeoutMs) {
  line = "";
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\n') return true;
      if (c != '\r') line += c;
    }
    mqtt.loop();
    delay(1);
  }
  return false;
}

bool readExactUSB(uint8_t* destination, size_t length, uint32_t timeoutMs) {
  size_t received = 0;
  unsigned long start = millis();
  while (received < length && millis() - start < timeoutMs) {
    if (Serial.available()) {
      destination[received++] = (uint8_t)Serial.read();
    } else {
      mqtt.loop();
      delay(1);
    }
  }
  return received == length;
}

void processOneFile() {
  String header;
  if (!readUsbLine(header, 10000)) return;
  if (!header.startsWith("START|")) {
    Serial.println("ERR_HEADER");
    return;
  }

  int separator = header.indexOf('|', 6);
  if (separator < 0) {
    Serial.println("ERR_HEADER_FORMAT");
    return;
  }

  String filename = header.substring(6, separator);
  uint32_t fileSize = (uint32_t)header.substring(separator + 1).toInt();
  if (filename.length() == 0 || fileSize == 0) {
    Serial.println("ERR_FILE_INFO");
    return;
  }

  // START frame membawa ukuran file dan nama file.
  Frame frame = {};
  frame.type = TYPE_START;
  frame.seq = 0;
  frame.len = 68;
  memcpy(frame.data, &fileSize, sizeof(fileSize));
  filename.replace("|", "_");
  size_t nameLength = min((size_t)63, (size_t)filename.length());
  memcpy(frame.data + 4, filename.c_str(), nameLength);
  frame.data[4 + nameLength] = '\0';
  finalizeFrame(frame);

  if (!sendFramePipeline(frame)) {
    Serial.println("ERR_START_PIPELINE");
    return;
  }
  Serial.println("READY_DATA");

  uint32_t remaining = fileSize;
  uint16_t sequence = 1;
  while (remaining > 0) {
    uint16_t amount = (uint16_t)min((uint32_t)CHUNK_SIZE, remaining);
    memset(frame.data, 0, CHUNK_SIZE);
    if (!readExactUSB(frame.data, amount, 30000)) {
      Serial.println("ERR_UART_TIMEOUT");
      return;
    }

    frame.type = TYPE_DATA;
    frame.seq = sequence;
    frame.len = amount;
    finalizeFrame(frame);

    if (!sendFramePipeline(frame)) {
      Serial.printf("ERR_PIPELINE|%u\n", sequence);
      return;
    }

    Serial.printf("ACK|%u\n", sequence);
    remaining -= amount;
    sequence++;
  }

  Frame endFrame = {};
  endFrame.type = TYPE_END;
  endFrame.seq = sequence;
  endFrame.len = 0;
  finalizeFrame(endFrame);
  if (!sendFramePipeline(endFrame)) {
    Serial.println("ERR_END_PIPELINE");
    return;
  }
  Serial.println("DONE");
}

void setup() {
  Serial.begin(USB_BAUD);
  delay(800);

  pinMode(PIN_SPI_READY, INPUT_PULLDOWN);
  pinMode(PIN_LORA_M0, OUTPUT);
  pinMode(PIN_LORA_M1, OUTPUT);
  digitalWrite(PIN_LORA_M0, LOW); // E220 normal/transparent mode
  digitalWrite(PIN_LORA_M1, LOW);

  LoRaSerial.begin(LORA_BAUD, SERIAL_8N1, PIN_LORA_RX, PIN_LORA_TX);

  if (!initSPIMaster()) {
    Serial.println("ERR_SPI_INIT");
    while (true) delay(1000);
  }

  connectWiFi();
  mqtt.setServer(MQTT_BROKER, 1883);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(60);
  connectMQTT();

  Serial.println("READY");
}

void loop() {
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();
  if (Serial.available()) processOneFile();
}
