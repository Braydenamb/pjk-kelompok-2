#pragma once
#include <WiFi.h>
// Copy to local_config.h using ./run_sender.sh configure (ignored by git).
const char *WIFI_SSID = "";
const char *WIFI_PASS = "";
IPAddress BROKER_IP(0, 0, 0, 0);
IPAddress LAPTOP_B_IP(0, 0, 0, 0);
const uint16_t MQTT_PORT = 1883;
const uint16_t TCP_PORT = 5001;
