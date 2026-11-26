"""Regression for the final guide's host script, not a radio test."""
import contextlib
import importlib.util
import io
from pathlib import Path
import unittest

PATH = (Path(__file__).resolve().parents[1] /
        'Panduan_Proyek_Jarkom_Kelompok2/jarkom_kelompok2/Laptop_A/send_image.py')
spec = importlib.util.spec_from_file_location('final_sender', PATH)
assert spec and spec.loader
sender = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sender)

class ReadyTests(unittest.TestCase):
    def test_serial_control_lines_are_inactive_before_open(self):
        from unittest.mock import patch
        events = []
        class Port:
            def __init__(self, *args, **kwargs):
                self.dtr = True
                self.rts = True
                self.port = kwargs.get('port')
            def open(self):
                events.append((self.port, self.dtr, self.rts))
        with patch.object(sender.serial, 'Serial', Port):
            sender.open_sender_serial('/dev/ttyACM0')
        self.assertEqual(events, [('/dev/ttyACM0', False, False)])

    def test_boot_noise_attached_to_ready_is_recognized(self):
        serial_stream = io.BytesIO(b'x\x80\x00\xf8xREADY\r\n')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sender.wait_for_line(serial_stream, 'READY', timeout=.01), 'READY')

    def test_printable_notready_is_not_accepted(self):
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(TimeoutError):
                sender.wait_for_line(io.BytesIO(b'NOTREADY\n'), 'READY', timeout=.01)

    def test_normal_ack_and_ready_data_still_work(self):
        with contextlib.redirect_stdout(io.StringIO()):
            for token in ('READY', 'READY_DATA', 'ACK|1', 'DONE'):
                self.assertEqual(sender.wait_for_line(io.BytesIO((token+'\n').encode()), token, timeout=.01), token)

if __name__ == '__main__':
    unittest.main()
