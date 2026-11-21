#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32DMASPIMaster.h>
#include "protocol.h"

// ===== EDIT THESE SETTINGS (must match ESP32 A) =====
const char *WIFI_SSID = "GANTI_NAMA_WIFI";
const char *WIFI_PASS = "GANTI_PASSWORD_WIFI";
IPAddress BROKER_IP(192, 168, 1, 10);  // IPv4 Laptop A
// ====================================================

static const char *TOPIC_DATA    = "kelompok2/image/spi_to_mqtt";
static const char *TOPIC_READY   = "kelompok2/control/ready";
static const char *TOPIC_CONTROL = "kelompok2/control/ack";

static const int PIN_LINK_RX = 16;
static const int PIN_LINK_TX = 17;
static const int PIN_LORA_RX = 26;
static const int PIN_LORA_TX = 27;
static const int PIN_LORA_AUX = 34;
static const int PIN_LORA_M0 = 32;
static const int PIN_LORA_M1 = 33;

static const int PIN_SPI_SCK = 18;
static const int PIN_SPI_MISO = 19;
static const int PIN_SPI_MOSI = 23;
static const int PIN_SPI_CS = 5;
static const int PIN_SPI_READY = 4;

static const size_t SPI_SIZE = 256;
static const uint32_t LORA_MIN_GAP_MS = 10000; // initial conservative pacing; verify actual airtime/regulatory limit

HardwareSerial LinkUART(1);
HardwareSerial LoRaUART(2);

WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);

ESP32DMASPI::Master spiMaster;
uint8_t *spiTxBuf = nullptr;
uint8_t *spiRxBuf = nullptr;

uint8_t uartBuf[FRAME_SIZE];
size_t uartCount = 0;

uint8_t mqttFrame[FRAME_SIZE];
bool haveMqttFrame = false;
bool waitingMqtt = false;
uint32_t waitingSeq = 0;

uint8_t loraRxBuf[FRAME_SIZE];
size_t loraRxCount = 0;

bool loraWaitingAck = false;
unsigned long lastLoRaSendMs = 0;
unsigned long lastLoRaTxMs = 0;
uint8_t loraRetries = 0;

bool mqttReady = false;
unsigned long lastReadyPublishMs = 0;
unsigned long lastMqttConnectMs = 0;
unsigned long lastWifiRetryMs = 0;

bool waitAuxHigh(uint32_t timeoutMs) {
  unsigned long start = millis();
  while (digitalRead(PIN_LORA_AUX) == LOW) {
    if (millis() - start > timeoutMs) return false;
    delay(1);
  }
  return true;
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  if (strcmp(topic, TOPIC_CONTROL) == 0 &&
      length == 9 && memcmp(payload, "READY_ACK", 9) == 0) {
    mqttReady = true;
    return;
  }

  if (strcmp(topic, TOPIC_DATA) == 0 &&
      length == FRAME_SIZE && waitingMqtt) {
    if (validFrame(payload) && isImageFrame(payload) &&
        frameSeq(payload) == waitingSeq) {
      memcpy(mqttFrame, payload, FRAME_SIZE);
      haveMqttFrame = true;
    }
  }
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiRetryMs > 5000) {
    lastWifiRetryMs = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

void ensureMQTT() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (millis() - lastMqttConnectMs < 3000) return;
  lastMqttConnectMs = millis();

  if (mqtt.connect("K2_ESP32_B")) {
    mqtt.subscribe(TOPIC_DATA);
    mqtt.subscribe(TOPIC_CONTROL);
    mqttReady = false;
  }
}

bool waitForSPISlaveReady(uint32_t timeoutMs) {
  unsigned long start = millis();
  while (digitalRead(PIN_SPI_READY) == LOW) {
    if (millis() - start > timeoutMs) return false;
    delay(1);
  }
  return true;
}

void processUARTInput() {
  if (!mqttReady || waitingMqtt || haveMqttFrame || loraWaitingAck) return;

  while (LinkUART.available()) {
    uartBuf[uartCount++] = (uint8_t)LinkUART.read();

    if (uartCount == FRAME_SIZE) {
      uartCount = 0;

      if (!validFrame(uartBuf) || !isImageFrame(uartBuf)) {
        Serial.println("ERR:UART_FRAME");
        continue;
      }

      waitingSeq = frameSeq(uartBuf);
      waitingMqtt = true;

      memset(spiTxBuf, 0, SPI_SIZE);
      memset(spiRxBuf, 0, SPI_SIZE);
      memcpy(spiTxBuf, uartBuf, FRAME_SIZE);

      if (!waitForSPISlaveReady(5000)) {
        Serial.println("ERR:SPI_SLAVE_NOT_READY");
        waitingMqtt = false;
        return;
      }

      size_t received = spiMaster.transfer(spiTxBuf, spiRxBuf, SPI_SIZE, 5000);
      if (received < FRAME_SIZE) {
        Serial.println("ERR:SPI_TRANSFER");
        waitingMqtt = false;
      }
      return;
    }
  }
}

void sendLoRaFrame() {
  if (millis() - lastLoRaTxMs < LORA_MIN_GAP_MS) return;
  if (!waitAuxHigh(3000)) return;

  LoRaUART.write(mqttFrame, FRAME_SIZE);
  LoRaUART.flush();

  lastLoRaTxMs = millis();
  lastLoRaSendMs = millis();
  loraWaitingAck = true;
}

void handleLoRaAck(const uint8_t *frame) {
  if (frameType(frame) == TYPE_ACK &&
      loraWaitingAck &&
      frameSeq(frame) == waitingSeq) {
    uint8_t uartAck[FRAME_SIZE];
    makeFrame(TYPE_ACK, waitingSeq, nullptr, 0, uartAck);

    LinkUART.write(uartAck, FRAME_SIZE);
    LinkUART.flush();

    loraWaitingAck = false;
    waitingMqtt = false;
    haveMqttFrame = false;
    loraRetries = 0;

    Serial.printf("DONE:%lu\n", (unsigned long)waitingSeq);
  }
}

void processLoRaAckInput() {
  while (LoRaUART.available()) {
    uint8_t b = (uint8_t)LoRaUART.read();

    if (loraRxCount < FRAME_SIZE) {
      loraRxBuf[loraRxCount++] = b;
    }

    if (loraRxCount == FRAME_SIZE) {
      if (validFrame(loraRxBuf)) {
        handleLoRaAck(loraRxBuf);
        loraRxCount = 0;
      } else {
        memmove(loraRxBuf, loraRxBuf + 1, FRAME_SIZE - 1);
        loraRxCount = FRAME_SIZE - 1;
      }
    }
  }
}

void serviceLoRa() {
  if (haveMqttFrame && !loraWaitingAck) {
    sendLoRaFrame();
  }

  if (loraWaitingAck && millis() - lastLoRaSendMs > 15000) {
    if (loraRetries < 2) {
      loraRetries++;
      loraWaitingAck = false;
      sendLoRaFrame();
    } else {
      Serial.println("ERR:LORA_ACK_TIMEOUT");
      lastLoRaSendMs = millis();
    }
  }
}

void setup() {
  Serial.begin(115200);

  LinkUART.begin(460800, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);

  pinMode(PIN_LORA_M0, OUTPUT);
  pinMode(PIN_LORA_M1, OUTPUT);
  digitalWrite(PIN_LORA_M0, LOW);
  digitalWrite(PIN_LORA_M1, LOW);
  pinMode(PIN_LORA_AUX, INPUT);
  LoRaUART.begin(115200, SERIAL_8N1, PIN_LORA_RX, PIN_LORA_TX);

  pinMode(PIN_SPI_READY, INPUT);

  spiTxBuf = spiMaster.allocDMABuffer(SPI_SIZE);
  spiRxBuf = spiMaster.allocDMABuffer(SPI_SIZE);
  if (!spiTxBuf || !spiRxBuf) {
    Serial.println("ERR:SPI_DMA_ALLOC");
  }

  spiMaster.setDataMode(SPI_MODE1);
  spiMaster.setDutyCyclePos(96);
  spiMaster.setFrequency(250000);
  spiMaster.setMaxTransferSize(SPI_SIZE);
  spiMaster.setQueueSize(1);
  if (!spiMaster.begin(VSPI, PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CS)) {
    Serial.println("ERR:SPI_MASTER_BEGIN");
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  mqtt.setServer(BROKER_IP, 1883);
  mqtt.setBufferSize(512);
  mqtt.setCallback(mqttCallback);
}

void loop() {
  ensureWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) ensureMQTT();
    else mqtt.loop();

    // Heartbeat is kept periodic so ESP32 A knows the subscriber is still online.
    if (mqtt.connected() && millis() - lastReadyPublishMs > 2000) {
      mqtt.publish(TOPIC_READY, "READY");
      lastReadyPublishMs = millis();
    }
  }

  processLoRaAckInput();
  serviceLoRa();
  processUARTInput();

  delay(1);
}
