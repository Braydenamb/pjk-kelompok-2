"""Launcher tests; fake port/serial fixtures are host-only, NOT hardware proof."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import socket
import shutil
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('workflow', ROOT / 'tools/workflow.py')

class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue((ROOT / 'tools/workflow.py').exists(), 'guided workflow missing')
        assert spec is not None and spec.loader is not None
        self.w = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.w)

    @unittest.skipUnless(shutil.which('mosquitto'), 'requires actual Mosquitto')
    def test_actual_loopback_broker_round_trip_and_cleanup_on_failure(self):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        with self.assertRaisesRegex(RuntimeError, 'operator cancelled'):
            with self.w.managed_broker('127.0.0.1', port):
                self.w.mqtt_probe('127.0.0.1', port)
                raise RuntimeError('operator cancelled')
        with socket.socket() as sock:
            self.assertNotEqual(sock.connect_ex(('127.0.0.1', port)), 0)

    def test_no_board_is_clear_and_never_selects_unidentified_port(self):
        with self.assertRaisesRegex(ValueError, 'Tidak ada port'):
            self.w.choose_port([], None)
        ports = [SimpleNamespace(device='/dev/ttyUSB7', description='Unknown')]
        with self.assertRaisesRegex(ValueError, 'Pilih eksplisit'):
            self.w.choose_port(ports, None)
        self.assertEqual(self.w.choose_port(ports, '/dev/ttyUSB7'), '/dev/ttyUSB7')
        with self.assertRaisesRegex(ValueError, 'tidak terdeteksi'):
            self.w.choose_port(ports, '/dev/ttyUSB8')

    def test_configure_refuses_nonprivate_pipe_before_prompting(self):
        with patch.object(self.w.sys.stdin, 'isatty', return_value=False):
            with self.assertRaisesRegex(ValueError, 'terminal lokal'):
                self.w.configure()

    def test_local_config_is_validated_and_password_escaped(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp)
            config = self.w.save_config(target, 'lab', 'quote"back\\slash', '127.0.0.1', '127.0.0.1', 1883, 5001)
            header = target / 'ESP32_A/local_config.h'
            self.assertEqual(header.stat().st_mode & 0o777, 0o600)
            self.assertIn(r'quote\"back\\slash', header.read_text())
            self.assertNotIn('quote', (target / 'config.local.json').read_text())
            self.assertEqual(config['tcp_port'], 5001)
            with self.assertRaises(ValueError):
                self.w.save_config(target, 'lab', '', 'not-ip', '127.0.0.1', 1883, 5001)

if __name__ == '__main__':
    unittest.main()
