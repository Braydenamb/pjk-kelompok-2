#include <Arduino.h>

// Jalankan sketch ini satu kali pada MASING-MASING modul E220.
// Wiring: E220 TXD->GPIO26, RXD->GPIO27, M0->GPIO32, M1->GPIO33,
// AUX->GPIO34, VCC->supply 5V stabil, GND->GND.
// Mode konfigurasi E220: M1=1 dan M0=1; UART konfigurasi 9600 8N1.

HardwareSerial E220(2);
const int PIN_M0 = 32;
const int PIN_M1 = 33;
const int PIN_AUX = 34;
const int PIN_RX = 26;
const int PIN_TX = 27;

// Register:
// ADDH=00, ADDL=00
// REG0=E7: UART 115200, 8N1, air data rate 62.5 kbps
// REG1=02: sub-packet 200 byte, TX power 13 dBm
// REG2=46: CH70 -> 920.125 MHz (pastikan penggunaan kanal sesuai izin/aturan)
// REG3=10: transparent mode + LBT enabled
const uint8_t CONFIG_CMD[] = {
  0xC0, 0x00, 0x06,
  0x00, 0x00, 0xE7, 0x02, 0x46, 0x10
};

void setup() {
  Serial.begin(115200);
  pinMode(PIN_M0, OUTPUT);
  pinMode(PIN_M1, OUTPUT);
  pinMode(PIN_AUX, INPUT);

  digitalWrite(PIN_M0, HIGH);
  digitalWrite(PIN_M1, HIGH);

  E220.begin(9600, SERIAL_8N1, PIN_RX, PIN_TX);
  delay(300);

  Serial.println("Mengirim konfigurasi E220...");
  E220.write(CONFIG_CMD, sizeof(CONFIG_CMD));
  E220.flush();
  delay(500);

  Serial.print("Respons E220 (hex): ");
  while (E220.available()) {
    uint8_t b = E220.read();
    if (b < 0x10) Serial.print('0');
    Serial.print(b, HEX);
    Serial.print(' ');
  }
  Serial.println();
  Serial.println("Jika respons C1 00 06 00 00 E7 02 46 10 muncul, konfigurasi tersimpan.");
  Serial.println("Matikan daya, lalu upload ESP32_A.ino / ESP32_B.ino.");
}

void loop() {}
