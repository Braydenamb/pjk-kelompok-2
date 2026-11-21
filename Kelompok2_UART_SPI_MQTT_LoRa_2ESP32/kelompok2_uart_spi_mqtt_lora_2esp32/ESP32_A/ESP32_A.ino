#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32DMASPISlave.h>
#include <vector>
#include "protocol.h"

// ===== EDIT THESE SETTINGS =====
const char *WIFI_SSID = "GANTI_NAMA_WIFI";
const char *WIFI_PASS = "GANTI_PASSWORD_WIFI";
IPAddress BROKER_IP(192, 168, 1, 10);       // IPv4 Laptop A
IPAddress LAPTOP_B_IP(192, 168, 1, 11);     // IPv4 Laptop B
const uint16_t TCP_PORT = 5001;
// ===============================

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

    // Queue and trigger first, then tell ESP32 B that the slave is ready.
    if (!spiSlave.trigger()) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    digitalWrite(PIN_SPI_READY, HIGH);
    std::vector<size_t> sizes = spiSlave.wait(5000);
    digitalWrite(PIN_SPI_READY, LOW);

    if (sizes.size() > 0 && sizes[0] >= FRAME_SIZE && validFrame(rxBuf)) {
      uint8_t frame[FRAME_SIZE];
      memcpy(frame, rxBuf, FRAME_SIZE);
      xQueueSend(spiQueue, frame, pdMS_TO_TICKS(1000));
    }
  }
}

void processHostSerial() {
  while (Serial.available()) {
    hostBuf[hostCount++] = (uint8_t)Serial.read();

    if (hostCount == FRAME_SIZE) {
      hostCount = 0;

      if (!validFrame(hostBuf) || !isImageFrame(hostBuf)) {
        Serial.println("ERR:HOST_FRAME");
        continue;
      }

      LinkUART.write(hostBuf, FRAME_SIZE);
      LinkUART.flush();
    }
  }
}

void processUartAck() {
  while (LinkUART.available()) {
    uartAckBuf[uartAckCount++] = (uint8_t)LinkUART.read();

    if (uartAckCount == FRAME_SIZE) {
      uartAckCount = 0;

      if (validFrame(uartAckBuf) &&
          frameType(uartAckBuf) == TYPE_ACK &&
          waitingUartAck &&
          frameSeq(uartAckBuf) == waitingUartAckSeq) {
        Serial.printf("ACK:%lu\n", (unsigned long)waitingUartAckSeq);
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

  if (hasMqttPending && mqtt.connected() &&
      peerSeen && (millis() - lastPeerSeenMs < 6000)) {
    if (mqtt.publish(TOPIC_DATA, mqttPendingFrame, FRAME_SIZE, false)) {
      hasMqttPending = false;
    }
  }
}

void sendLoRaAck(uint32_t seq) {
  uint8_t ack[FRAME_SIZE];
  makeFrame(TYPE_ACK, seq, nullptr, 0, ack);

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
  if (!isImageFrame(frame)) return;

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
      unsigned long seq = 0;
      if (sscanf(tcpLine, "ACK:%lu", &seq) == 1 &&
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
  if (!spiSlave.begin(VSPI, PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CS)) {
    Serial.println("ERR:SPI_SLAVE_BEGIN");
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  mqtt.setServer(BROKER_IP, 1883);
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

  if (hostReady) processHostSerial();

  processSPIQueue();
  processLoRaInput();
  serviceTCP();
  serviceLoRaAck();
  processUartAck();

  delay(1);
}
