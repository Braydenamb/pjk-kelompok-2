#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32DMASPIMaster.h>
#include "protocol.h"
#include "reliability.h"

#if __has_include("local_config.h")
#include "local_config.h"
#else
#include "config.example.h"
#endif

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

uint32_t activeSession = 0;
Pending mqttPending;
Pending radioPending;
uint8_t lastAck[FRAME_SIZE];
bool haveLastAck = false;
uint8_t mqttFrame[FRAME_SIZE];
bool haveMqttFrame = false;
bool waitingMqtt = false;
uint32_t waitingSeq = 0;

uint8_t loraRxBuf[FRAME_SIZE];
size_t loraRxCount = 0;

bool loraWaitingAck = false;
unsigned long lastLoRaTxMs = 0;

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
        frameSession(payload) == activeSession && frameSeq(payload) == waitingSeq && !haveMqttFrame) {
      memcpy(mqttFrame, payload, FRAME_SIZE);
      haveMqttFrame = true;
      waitingMqtt = false;
      mqttPending.clear();
      radioPending.begin(activeSession, waitingSeq, millis());
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

void clearTransfer() {
  waitingMqtt = false;
  haveMqttFrame = false;
  loraWaitingAck = false;
  mqttPending.clear();
  radioPending.clear();
}

void failTransfer(const char *reason) {
  uint8_t error[FRAME_SIZE];
  makeFrame(TYPE_ERROR, activeSession, waitingSeq,
            (const uint8_t *)reason, strlen(reason), error);
  LinkUART.write(error, FRAME_SIZE);
  LinkUART.flush();
  Serial.printf("ERR:%s\n", reason);
  clearTransfer();
}

bool forwardSPI() {
  memset(spiTxBuf, 0, SPI_SIZE);
  memset(spiRxBuf, 0, SPI_SIZE);
  memcpy(spiTxBuf, uartBuf, FRAME_SIZE);
  if (!waitForSPISlaveReady(1000)) return false;
  return spiMaster.transfer(spiTxBuf, spiRxBuf, SPI_SIZE, 5000) >= FRAME_SIZE;
}

void processUARTInput() {
  // Always drain control traffic; a fresh START must be able to cancel old work.
  while (LinkUART.available()) {
    uint8_t byteIn = (uint8_t)LinkUART.read();
    // Keep retry payload in uartBuf; assemble separately while busy.
    static uint8_t incoming[FRAME_SIZE];
    incoming[uartCount++] = byteIn;
    if (uartCount != FRAME_SIZE) continue;
    if (!validFrame(incoming)) {
      memmove(incoming, incoming + 1, FRAME_SIZE - 1);
      uartCount = FRAME_SIZE - 1;
      continue;
    }
    uartCount = 0;
    uint32_t session = frameSession(incoming);
    uint8_t type = frameType(incoming);
    if (type == TYPE_START && frameSeq(incoming) == 0 && session != activeSession) {
      clearTransfer();
      activeSession = session;
      haveLastAck = false;
    }
    if (session != activeSession) continue;
    if (type == TYPE_ABORT) {
      clearTransfer(); activeSession = 0; haveLastAck = false;
      continue;
    }
    if (!isImageFrame(incoming)) continue;
    if (haveLastAck && frameSeq(incoming) == frameSeq(lastAck)) {
      LinkUART.write(lastAck, FRAME_SIZE);
      LinkUART.flush();
      continue;
    }
    if (waitingMqtt || haveMqttFrame || loraWaitingAck) continue;
    memcpy(uartBuf, incoming, FRAME_SIZE);
    waitingSeq = frameSeq(incoming);
    waitingMqtt = true;
    mqttPending.begin(activeSession, waitingSeq, millis());
    if (!forwardSPI()) Serial.println("WARN:SPI_RETRY_PENDING");
  }
}

void serviceMqttTimeout() {
  if (!waitingMqtt) return;
  RetryAction action = mqttPending.poll(millis(), 5000, 2);
  if (action == RETRY) forwardSPI();
  else if (action == EXHAUSTED) failTransfer("MQTT_TIMEOUT");
}

void sendLoRaFrame() {
  if (millis() - lastLoRaTxMs < LORA_MIN_GAP_MS) return;
  if (!waitAuxHigh(3000)) return;

  LoRaUART.write(mqttFrame, FRAME_SIZE);
  LoRaUART.flush();

  lastLoRaTxMs = millis();
  loraWaitingAck = true;
  radioPending.since = millis();
}

void handleLoRaAck(const uint8_t *frame) {
  if (frameType(frame) == TYPE_ACK &&
      loraWaitingAck &&
      frameSession(frame) == activeSession && frameSeq(frame) == waitingSeq) {
    uint8_t uartAck[FRAME_SIZE];
    makeFrame(TYPE_ACK, activeSession, waitingSeq, nullptr, 0, uartAck);

    LinkUART.write(uartAck, FRAME_SIZE);
    LinkUART.flush();

    memcpy(lastAck, uartAck, FRAME_SIZE);
    haveLastAck = true;
    clearTransfer();

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
  if (!haveMqttFrame) return;
  RetryAction action = radioPending.poll(millis(), 15000, 2);
  if (action == EXHAUSTED) { failTransfer("LORA_ACK_TIMEOUT"); return; }
  if (action == RETRY) loraWaitingAck = false;
  if (!loraWaitingAck) sendLoRaFrame();
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

  mqtt.setServer(BROKER_IP, MQTT_PORT);
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
  serviceMqttTimeout();

  delay(1);
}
