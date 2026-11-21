"""Cross-language wire contract, compiled natively; NOT hardware evidence."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'Kelompok2_UART_SPI_MQTT_LoRa_2ESP32/kelompok2_uart_spi_mqtt_lora_2esp32'
sys.path.insert(0, str(SOURCE))
import protocol

class ProtocolTests(unittest.TestCase):
    def test_cpp_python_identical_session_crc_and_payload_bound(self):
        self.assertEqual((SOURCE / 'ESP32_A/protocol.h').read_bytes(), (SOURCE / 'ESP32_B/protocol.h').read_bytes())
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp)
            (target / 'Arduino.h').write_text('#include <cstdint>\n#include <cstddef>\n#include <cstring>\n')
            (target / 'test.cpp').write_text(r'''
#include "protocol.h"
#include <cstdio>
#include <cassert>
int main() {
  uint8_t f[FRAME_SIZE];
  const uint8_t payload[] = {0, 255, 42, 13};
  makeFrame(TYPE_DATA, 0x1234abcd, 7, payload, sizeof(payload), f);
  assert(validFrame(f)); assert(frameSession(f) == 0x1234abcd);
  assert(frameSeq(f) == 7); assert(frameLen(f) == sizeof(payload));
  fwrite(f, 1, FRAME_SIZE, stdout);
  f[11] ^= 1; assert(!validFrame(f)); // identity is CRC-protected
}
''')
            result = subprocess.run(['g++', '-std=c++11', '-I', str(target), '-I', str(SOURCE / 'ESP32_A'), str(target / 'test.cpp'), '-o', str(target / 'test')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            frame = subprocess.check_output([str(target / 'test')])
            self.assertEqual(frame, protocol.make_frame(2, 7, bytes([0, 255, 42, 13]), session=0x1234abcd))
            self.assertEqual(protocol.parse_frame(frame), (2, 0x1234abcd, 7, bytes([0, 255, 42, 13])))
            with self.assertRaises(ValueError):
                protocol.make_frame(2, 7, b'x' * 182, session=1)
            with self.assertRaises(ValueError):
                protocol.make_frame(2, 7, session=0)
            with self.assertRaises(ValueError):
                protocol.parse_frame(frame[:11] + bytes([frame[11] ^ 1]) + frame[12:])

if __name__ == '__main__':
    unittest.main()
