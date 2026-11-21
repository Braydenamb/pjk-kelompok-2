"""Host C++ behavioral checks: no ESP32 electrical/radio claims."""
import subprocess
import tempfile
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'Kelompok2_UART_SPI_MQTT_LoRa_2ESP32/kelompok2_uart_spi_mqtt_lora_2esp32'

class ReliabilityTests(unittest.TestCase):
    def test_firmware_uses_session_and_bounded_failure_paths(self):
        a = (SOURCE / 'ESP32_A/ESP32_A.ino').read_text()
        b = (SOURCE / 'ESP32_B/ESP32_B.ino').read_text()
        self.assertIn('frameSession(uartAckBuf) == activeSession', a)
        self.assertIn('mqttPending.poll(millis(), 5000, 2)', b)
        self.assertIn('failTransfer("MQTT_TIMEOUT")', b)
        self.assertIn('failTransfer("LORA_ACK_TIMEOUT")', b)
        self.assertIn('TYPE_ABORT', a)
        self.assertIn('TYPE_ABORT', b)

    def test_bounded_retry_and_session_matching(self):
        code = r'''
#include "reliability.h"
#include <cassert>
int main() {
  Pending p;
  p.begin(12, 0, 100);
  assert(!p.matches(11, 0));
  assert(p.matches(12, 0));
  assert(p.poll(109, 10, 2) == WAIT);
  assert(p.poll(110, 10, 2) == RETRY);
  assert(p.poll(120, 10, 2) == RETRY);
  assert(p.poll(130, 10, 2) == EXHAUSTED);
  assert(!p.active);
  p.begin(13, 0, 140);
  assert(!p.matches(12, 0));
  assert(p.matches(13, 0));
  p.clear();
  assert(!p.active);
  p.begin(14, 1, 0xfffffff0u);
  assert(p.poll(5, 20, 0) == EXHAUSTED); // millis wrap
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'test.cpp').write_text(code)
            result = subprocess.run(['g++', '-std=c++11', '-I', str(SOURCE / 'ESP32_B'), str(path / 'test.cpp'), '-o', str(path / 'test')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(path / 'test')], check=True)

if __name__ == '__main__':
    unittest.main()
