"""Real local TCP tests; these do NOT exercise ESP32 or radio hardware."""
import importlib.util
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'Kelompok2_UART_SPI_MQTT_LoRa_2ESP32/kelompok2_uart_spi_mqtt_lora_2esp32'
spec = importlib.util.spec_from_file_location('sender', SOURCE / 'Laptop_A/send_image.py')
assert spec is not None and spec.loader is not None
sender = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sender)


class ReceiverTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.log = tempfile.TemporaryFile()
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            self.port = probe.getsockname()[1]
        self.process = subprocess.Popen(
            [sys.executable, '-u', str(SOURCE / 'Laptop_B/receive_image.py'),
             '--bind', '127.0.0.1', '--port', str(self.port), '--output', 'received_images'],
            cwd=self.temp.name, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 5
        while True:
            if self.process.poll() is not None:
                self.fail('Receiver exited before accepting a connection')
            try:
                self.conn = socket.create_connection(('127.0.0.1', self.port), timeout=0.2)
                self.conn.settimeout(0.5)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    self.fail('Receiver did not start')
                time.sleep(0.05)

    def tearDown(self):
        if hasattr(self, 'conn'):
            self.conn.close()
        self.process.terminate()
        self.process.wait(timeout=5)
        self.log.close()
        self.temp.cleanup()

    def exchange(self, kind, seq, payload):
        self.conn.sendall(sender.make_frame(kind, seq, payload, session=1))
        return self.conn.recv(128)

    def start_and_data(self, data=b'test image bytes'):
        name = b'test.jpg'
        meta = struct.pack('<IBB', len(data), 1, len(name)) + name
        self.assertEqual(self.exchange(1, 0, meta), b'ACK:1:0\n')
        self.assertEqual(self.exchange(2, 1, data), b'ACK:1:1\n')
        return data

    def test_valid_png_round_trip_and_duplicate_ack(self):
        # Construct a real one-pixel RGB PNG using only the standard library.
        def chunk(kind, data):
            return (struct.pack('>I', len(data)) + kind + data +
                    struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))
        data = (b'\x89PNG\r\n\x1a\n' +
                chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(b'\x00\x00\xff\x00')) + chunk(b'IEND', b''))
        name = b'pixel.png'
        self.assertEqual(self.exchange(1, 0, struct.pack('<IBB', len(data), 2, len(name)) + name), b'ACK:1:0\n')
        self.assertEqual(self.exchange(2, 1, data), b'ACK:1:1\n')
        self.assertEqual(self.exchange(2, 1, data), b'ACK:1:1\n')
        ending = struct.pack('<I', zlib.crc32(data) & 0xffffffff)
        self.assertEqual(self.exchange(3, 2, ending), b'ACK:1:2\n')
        self.assertEqual(self.exchange(3, 2, ending), b'ACK:1:2\n')
        output = Path(self.temp.name) / 'received_images/received_pixel.png'
        self.assertEqual(output.read_bytes(), data)
        self.assertFalse(output.with_suffix('.png.part').exists())

    def test_new_session_sequence_zero_replaces_incomplete_start(self):
        name = b'old.jpg'
        meta = struct.pack('<IBB', 3, 1, len(name)) + name
        self.conn.sendall(sender.make_frame(1, 0, meta, session=101))
        self.assertEqual(self.conn.recv(128), b'ACK:101:0\n')
        name = b'new.jpg'
        meta = struct.pack('<IBB', 3, 1, len(name)) + name
        self.conn.sendall(sender.make_frame(1, 0, meta, session=102))
        self.assertEqual(self.conn.recv(128), b'ACK:102:0\n')
        self.conn.sendall(sender.make_frame(2, 1, b'new', session=102))
        self.assertEqual(self.conn.recv(128), b'ACK:102:1\n')
        self.conn.sendall(sender.make_frame(3, 2, struct.pack('<I', zlib.crc32(b'new')), session=102))
        self.assertEqual(self.conn.recv(128), b'ACK:102:2\n')
        self.assertEqual((Path(self.temp.name) / 'received_images/received_new.jpg').read_bytes(), b'new')

    def test_abort_retires_session_and_old_start_cannot_replace_new_session(self):
        self.start_and_data()
        self.conn.sendall(sender.make_frame(5, 0, session=1))
        self.assertEqual(self.conn.recv(128), b'ACK:1:0\n')
        name = b'next.png'
        meta = struct.pack('<IBB', 0, 2, len(name)) + name
        self.conn.sendall(sender.make_frame(1, 0, meta, session=2))
        self.assertEqual(self.conn.recv(128), b'ACK:2:0\n')
        self.conn.sendall(sender.make_frame(1, 0, meta, session=1))
        with self.assertRaises(socket.timeout):
            self.conn.recv(128)
        self.conn.sendall(sender.make_frame(3, 1, struct.pack('<I', 0), session=2))
        self.assertEqual(self.conn.recv(128), b'ACK:2:1\n')

    def test_same_sequence_but_different_payload_is_not_duplicate(self):
        self.start_and_data()
        self.conn.sendall(sender.make_frame(2, 1, b'changed', session=1))
        with self.assertRaises(socket.timeout):
            self.conn.recv(128)

    def test_wrong_size_is_not_acknowledged(self):
        data = self.start_and_data()
        self.assertEqual(self.exchange(2, 2, b'extra'), b'ACK:1:2\n')
        crc = zlib.crc32(data + b'extra') & 0xffffffff
        self.conn.sendall(sender.make_frame(3, 3, struct.pack('<I', crc), session=1))
        with self.assertRaises(socket.timeout):
            self.conn.recv(128)

    def test_out_of_order_data_is_not_acknowledged(self):
        self.start_and_data()
        self.conn.sendall(sender.make_frame(2, 4, b'skipped sequence', session=1))
        with self.assertRaises(socket.timeout):
            self.conn.recv(128)

    def test_corrupt_transfer_is_not_exposed_as_finished_image(self):
        data = self.start_and_data()
        bad_crc = (zlib.crc32(data) ^ 1) & 0xffffffff
        self.conn.sendall(sender.make_frame(3, 2, struct.pack('<I', bad_crc), session=1))
        with self.assertRaises(socket.timeout):
            self.conn.recv(128)
        self.assertFalse((Path(self.temp.name) / 'received_images/received_test.jpg').exists())

    def test_bad_whole_file_crc_is_not_acknowledged(self):
        data = self.start_and_data()
        bad_crc = (zlib.crc32(data) ^ 1) & 0xffffffff
        self.conn.sendall(sender.make_frame(3, 2, struct.pack('<I', bad_crc), session=1))
        with self.assertRaises(socket.timeout):
            self.conn.recv(128)


if __name__ == '__main__':
    unittest.main()
