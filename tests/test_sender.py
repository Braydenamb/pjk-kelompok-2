"""Fake serial for host sender unit tests ONLY; no hardware transfer evidence."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'Kelompok2_UART_SPI_MQTT_LoRa_2ESP32/kelompok2_uart_spi_mqtt_lora_2esp32'
spec = importlib.util.spec_from_file_location('sender_unit', SOURCE / 'Laptop_A/send_image.py')
assert spec is not None and spec.loader is not None
sender = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sender)

class FakeSerial:
    def __init__(self, lines):
        self.lines = iter(lines)
        self.written = []
    def write(self, data):
        self.written.append(data)
        return len(data)
    def flush(self):
        pass
    def readline(self):
        return next(self.lines, b'')

class SenderUnitTests(unittest.TestCase):
    def test_stale_session_ack_does_not_complete_current_packet(self):
        port = FakeSerial([b'ACK:10:0\n'])
        frame = sender.make_frame(1, 0, b'fixture', session=11)
        with self.assertRaises(TimeoutError):
            sender.send_and_wait_ack(port, frame, 0, session=11, timeout=0.01)
        self.assertEqual(port.written, [frame])

    def test_matching_ack_and_stage_error(self):
        frame = sender.make_frame(1, 0, b'fixture', session=11)
        port = FakeSerial([b'ACK:10:0\n', b'ACK:11:0\n'])
        sender.send_and_wait_ack(port, frame, 0, session=11, timeout=0.1)
        self.assertEqual(port.written, [frame])
        with self.assertRaisesRegex(RuntimeError, 'MQTT_TIMEOUT'):
            sender.send_and_wait_ack(FakeSerial([b'ERR:11:0:MQTT_TIMEOUT\n']), frame, 0, session=11, timeout=0.1)

if __name__ == '__main__':
    unittest.main()
