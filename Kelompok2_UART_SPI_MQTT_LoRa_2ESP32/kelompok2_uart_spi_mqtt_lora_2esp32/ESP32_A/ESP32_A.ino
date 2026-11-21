#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32DMASPISlave.h>
#include <vector>
#include "protocol.h"

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
WiFiClient tcpClient;

ESP32DMASPI::Slave spiSlave;
QueueHandle_t spiQueue = nullptr;

uint32_t activeSession = 0;
uint8_t hostBuf[FRAME_SIZE];
size_t hostCount = 0;

uint8_t uartAckBuf[FRAME_SIZE];
size_t uartAckCount = 0;

uint8_t loraRxBuf[FRAME_SIZE];
size_t loraRxCount = 0;

uint8_t mqttPendingFrame[FRAME_SIZE];
bool hasMqttPending = false;

uint8_t tcpPendingFrame[FRAME_SIZE];
bool hasTcpPending = false;
bool tcpFrameSent = false;
uint32_t tcpPendingSeq = 0;
uint32_t tcpRetryCount = 0;
unsigned long lastTcpSendMs = 0;
unsigned long lastTcpConnectMs = 0;

bool tcpAckReceived = false;
uint32_t tcpAckSeq = 0;

bool loraAckPending = false;
uint32_t loraAckSeq = 0;
uint32_t lastLoRaAckSeq = 0xFFFFFFFFUL;
unsigned long lastLoRaTxMs = 0;

bool waitingUartAck = false;
uint32_t waitingUartAckSeq = 0;

bool peerSeen = false;
unsigned long lastPeerSeenMs = 0;
bool needReadyAck = false;
unsigned long lastReadyPrintMs = 0;
unsigned long lastMqttConnectMs = 0;
unsigned long lastWifiRetryMs = 0;

char tcpLine[48];
size_t tcpLineLen = 0;

bool waitAuxHigh(uint32_t timeoutMs) {
  unsigned long start = millis();
  while (digitalRead(PIN_LORA_AUX) == LOW) {
    if (millis() - start > timeoutMs) return false;
    delay(1);
  }
  return true;
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  if (strcmp(topic, TOPIC_READY) == 0 &&
      length == 5 && memcmp(payload, "READY", 5) == 0) {
    peerSeen = true;
    lastPeerSeenMs = millis();
    needReadyAck = true;
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

  if (mqtt.connect("K2_ESP32_A")) {
    mqtt.subscribe(TOPIC_READY);
  }
}

void ensureTCP() {
  if (WiFi.status() != WL_CONNECTED || tcpClient.connected()) return;
  if (millis() - lastTcpConnectMs < 3000) return;
  lastTcpConnectMs = millis();
  tcpClient.stop();
  tcpClient.connect(LAPTOP_B_IP, TCP_PORT);
}

void IRAM_ATTR spiTransactionArmed(spi_slave_transaction_t *transaction) {
  digitalWrite(PIN_SPI_READY, HIGH);
}

void IRAM_ATTR spiTransactionFinished(spi_slave_transaction_t *transaction) {
  digitalWrite(PIN_SPI_READY, LOW);
}

void spiSlaveTask(void *parameter) {
  uint8_t *txBuf = spiSlave.allocDMABuffer(SPI_SIZE);
  uint8_t *rxBuf = spiSlave.allocDMABuffer(SPI_SIZE);

  if (!txBuf || !rxBuf) {
    Serial.println("ERR:SPI_DMA_ALLOC");
    vTaskDelete(nullptr);
    return;
  }

  memset(txBuf, 0, SPI_SIZE);

  for (;;) {
    memset(rxBuf, 0, SPI_SIZE);

    if (!spiSlave.queue(txBuf, rxBuf, SPI_SIZE)) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    // Dispatch asynchronously; the hardware post-setup callback asserts READY.
    if (!spiSlave.trigger()) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    // trigger() already dispatched this transaction. wait() would trigger a
    // second, empty queue and allow the DMA buffer to be reused too soon.
    // Keep ownership until a result is available, even if the peer is absent.
    while (spiSlave.numTransactionsCompleted() == 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    digitalWrite(PIN_SPI_READY, LOW);
    const size_t received = spiSlave.numBytesReceived();
    const esp_err_t spiError = spiSlave.error();

    if (spiError == ESP_OK && received >= FRAME_SIZE && validFrame(rxBuf)) {
      uint8_t frame[FRAME_SIZE];
      memcpy(frame, rxBuf, FRAME_SIZE);
      xQueueSend(spiQueue, frame, pdMS_TO_TICKS(1000));
    }
  }
}

void resetTransfer() {
  hasMqttPending = false;
  hasTcpPending = false;
  tcpFrameSent = false;
  tcpAckReceived = false;
  loraAckPending = false;
  waitingUartAck = false;
  lastLoRaAckSeq = 0xFFFFFFFFUL;
  tcpLineLen = 0;
  tcpClient.stop(); // discard old ACK stream and any partial TCP frame
  xQueueReset(spiQueue);
}

void processHostSerial() {
  while (Serial.available()) {
    hostBuf[hostCount++] = (uint8_t)Serial.read();
    if (hostCount != FRAME_SIZE) continue;
    if (!validFrame(hostBuf)) {
      memmove(hostBuf, hostBuf + 1, FRAME_SIZE - 1);
      hostCount = FRAME_SIZE - 1;
      continue;
    }
    hostCount = 0;
    uint32_t session = frameSession(hostBuf);
    uint8_t type = frameType(hostBuf);
    if (type == TYPE_START && frameSeq(hostBuf) == 0 && session != activeSession) {
      resetTransfer();
      activeSession = session;
    }
    if (session != activeSession) continue;
    if (type == TYPE_ABORT) {
      // Control cancellation is best effort; next START is authoritative.
      if (tcpClient.connected()) tcpClient.write(hostBuf, FRAME_SIZE);
      resetTransfer();
      activeSession = 0;
    } else if (!isImageFrame(hostBuf)) continue;
    LinkUART.write(hostBuf, FRAME_SIZE);
    LinkUART.flush();
  }
}

void processUartAck() {
  while (LinkUART.available()) {
    uartAckBuf[uartAckCount++] = (uint8_t)LinkUART.read();
    if (uartAckCount != FRAME_SIZE) continue;
    if (!validFrame(uartAckBuf)) {
      memmove(uartAckBuf, uartAckBuf + 1, FRAME_SIZE - 1);
      uartAckCount = FRAME_SIZE - 1;
      continue;
    }
    uartAckCount = 0;
    if (frameSession(uartAckBuf) == activeSession) {
      if (frameType(uartAckBuf) == TYPE_ERROR) {
        Serial.printf("ERR:%lu:%lu:%.*s\n", (unsigned long)activeSession,
          (unsigned long)frameSeq(uartAckBuf), (int)frameLen(uartAckBuf),
          (const char *)(uartAckBuf + HEADER_SIZE));
        resetTransfer();
      } else if (frameType(uartAckBuf) == TYPE_ACK && waitingUartAck &&
                 frameSeq(uartAckBuf) == waitingUartAckSeq) {
        Serial.printf("ACK:%lu:%lu\n", (unsigned long)activeSession,
                      (unsigned long)waitingUartAckSeq);
        waitingUartAck = false;
      }
    }
  }
}

void processSPIQueue() {
  if (!hasMqttPending) {
    if (xQueueReceive(spiQueue, mqttPendingFrame, 0) == pdTRUE) {
      hasMqttPending = true;
    }
  }

  if (hasMqttPending && frameSession(mqttPendingFrame) != activeSession) hasMqttPending = false;

  if (hasMqttPending && mqtt.connected() &&
      peerSeen && (millis() - lastPeerSeenMs < 6000)) {
    if (mqtt.publish(TOPIC_DATA, mqttPendingFrame, FRAME_SIZE, false)) {
      hasMqttPending = false;
    }
  }
}

void sendLoRaAck(uint32_t seq) {
  uint8_t ack[FRAME_SIZE];
  makeFrame(TYPE_ACK, activeSession, seq, nullptr, 0, ack);

  if (millis() - lastLoRaTxMs < LORA_MIN_GAP_MS) return;
  if (!waitAuxHigh(3000)) return;

  LoRaUART.write(ack, FRAME_SIZE);
  LoRaUART.flush();
  lastLoRaTxMs = millis();

  lastLoRaAckSeq = seq;
  waitingUartAck = true;
  waitingUartAckSeq = seq;
  loraAckPending = false;
}

void handleLoRaFrame(const uint8_t *frame) {
  if (!isImageFrame(frame) || frameSession(frame) != activeSession) return;

  uint32_t seq = frameSeq(frame);

  // If the final ACK was lost, resend it without forwarding the same image
  // packet to Laptop B a second time.
  if (seq == lastLoRaAckSeq || (loraAckPending && seq == loraAckSeq)) {
    loraAckSeq = seq;
    loraAckPending = true;
    return;
  }

  // Stop-and-wait means only one image frame should be in flight.
  if (hasTcpPending) {
    if (seq == tcpPendingSeq) return;
    return;
  }

  memcpy(tcpPendingFrame, frame, FRAME_SIZE);
  tcpPendingSeq = seq;
  hasTcpPending = true;
  tcpFrameSent = false;
  tcpRetryCount = 0;
  tcpAckReceived = false;
}

void processLoRaInput() {
  while (LoRaUART.available()) {
    uint8_t b = (uint8_t)LoRaUART.read();

    if (loraRxCount < FRAME_SIZE) {
      loraRxBuf[loraRxCount++] = b;
    }

    if (loraRxCount == FRAME_SIZE) {
      if (validFrame(loraRxBuf)) {
        handleLoRaFrame(loraRxBuf);
        loraRxCount = 0;
      } else {
        memmove(loraRxBuf, loraRxBuf + 1, FRAME_SIZE - 1);
        loraRxCount = FRAME_SIZE - 1;
      }
    }
  }
}

void processTcpAckLines() {
  while (tcpClient.connected() && tcpClient.available()) {
    char c = (char)tcpClient.read();

    if (c == '\n') {
      tcpLine[tcpLineLen] = '\0';
      unsigned long session = 0, seq = 0;
      if (sscanf(tcpLine, "ACK:%lu:%lu", &session, &seq) == 2 && session == activeSession &&
          hasTcpPending && (uint32_t)seq == tcpPendingSeq) {
        tcpAckReceived = true;
        tcpAckSeq = (uint32_t)seq;
      }
      tcpLineLen = 0;
    } else if (tcpLineLen < sizeof(tcpLine) - 1) {
      tcpLine[tcpLineLen++] = c;
    }
  }
}

void serviceTCP() {
  if (hasTcpPending && tcpClient.connected() &&
      (!tcpFrameSent || millis() - lastTcpSendMs > 5000)) {
    size_t n = tcpClient.write(tcpPendingFrame, FRAME_SIZE);
    if (n == FRAME_SIZE) {
      tcpFrameSent = true;
      lastTcpSendMs = millis();
      tcpRetryCount++;
    } else {
      tcpClient.stop();
      tcpLineLen = 0;
    }
  }

  processTcpAckLines();

  if (tcpAckReceived && hasTcpPending && tcpAckSeq == tcpPendingSeq) {
    loraAckSeq = tcpPendingSeq;
    loraAckPending = true;
    tcpAckReceived = false;
    hasTcpPending = false;
    tcpFrameSent = false;
  }
}

void serviceLoRaAck() {
  if (loraAckPending) sendLoRaAck(loraAckSeq);
}

void setup() {
  Serial.begin(460800);

  LinkUART.begin(460800, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);

  pinMode(PIN_LORA_M0, OUTPUT);
  pinMode(PIN_LORA_M1, OUTPUT);
  digitalWrite(PIN_LORA_M0, LOW);  // E220 transparent mode
  digitalWrite(PIN_LORA_M1, LOW);
  pinMode(PIN_LORA_AUX, INPUT);
  LoRaUART.begin(115200, SERIAL_8N1, PIN_LORA_RX, PIN_LORA_TX);

  pinMode(PIN_SPI_READY, OUTPUT);
  digitalWrite(PIN_SPI_READY, LOW);

  spiQueue = xQueueCreate(3, FRAME_SIZE);

  spiSlave.setDataMode(SPI_MODE1);
  spiSlave.setMaxTransferSize(SPI_SIZE);
  spiSlave.setQueueSize(1);
  spiSlave.setPostSetupCb(spiTransactionArmed);
  spiSlave.setPostTransCb(spiTransactionFinished);
  if (!spiSlave.begin(VSPI, PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CS)) {
    Serial.println("ERR:SPI_SLAVE_BEGIN");
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  mqtt.setServer(BROKER_IP, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setCallback(mqttCallback);

  xTaskCreatePinnedToCore(spiSlaveTask, "spiSlaveTask", 6144, nullptr, 2, nullptr, 1);
}

void loop() {
  ensureWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) ensureMQTT();
    else mqtt.loop();

    ensureTCP();
  }

  if (needReadyAck && mqtt.connected()) {
    const uint8_t msg[] = "READY_ACK";
    mqtt.publish(TOPIC_CONTROL, msg, sizeof(msg) - 1, false);
    needReadyAck = false;
  }

  bool hostReady = mqtt.connected() && peerSeen &&
                   (millis() - lastPeerSeenMs < 6000) &&
                   tcpClient.connected();

  if (hostReady && millis() - lastReadyPrintMs > 2000) {
    Serial.println("READY");
    lastReadyPrintMs = millis();
  }

  processHostSerial(); // cancellation/new sessions must work even if network is down

  processSPIQueue();
  processLoRaInput();
  serviceTCP();
  serviceLoRaAck();
  processUartAck();

  delay(1);
}
