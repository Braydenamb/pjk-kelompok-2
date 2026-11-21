#!/usr/bin/env python3
"""Local operator workflow. Never flashes boards or changes E220 settings."""
import argparse
from contextlib import contextmanager
import getpass
import ipaddress
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'Kelompok2_UART_SPI_MQTT_LoRa_2ESP32/kelompok2_uart_spi_mqtt_lora_2esp32'
CONFIG = SOURCE / 'config.local.json'


def ports_now():
    from serial.tools import list_ports
    return [p for p in list_ports.comports()
            if not (p.device.startswith('/dev/ttyS') and p.hwid == 'n/a')]


def choose_port(ports, selected):
    if not ports:
        raise ValueError('Tidak ada port serial. Sambungkan USB DATA ESP A, cek kabel/izin; jangan menebak board atau flash.')
    if not selected:
        raise ValueError('Pilih eksplisit --port setelah mencocokkan ESP A fisik; port tunggal pun bukan bukti identitas.')
    if selected not in [p.device for p in ports]:
        raise ValueError(f'Port {selected} tidak terdeteksi; jalankan preflight lagi.')
    return selected


def private_write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as out:
        os.chmod(path, 0o600)
        out.write(content)


def save_config(target, ssid, password, broker, receiver, mqtt_port, tcp_port):
    broker = str(ipaddress.IPv4Address(broker))
    receiver = str(ipaddress.IPv4Address(receiver))
    if broker == '0.0.0.0' or receiver == '0.0.0.0':
        raise ValueError('Gunakan IP endpoint nyata, bukan 0.0.0.0')
    if not ssid or any(not 1 <= p <= 65535 for p in (mqtt_port, tcp_port)):
        raise ValueError('SSID/port tidak valid')
    config = dict(broker_ip=broker, receiver_ip=receiver, mqtt_port=mqtt_port, tcp_port=tcp_port)
    header = ('#pragma once\n#include <WiFi.h>\n' +
              f'const char *WIFI_SSID = {json.dumps(ssid, ensure_ascii=False)};\n' +
              f'const char *WIFI_PASS = {json.dumps(password, ensure_ascii=False)};\n' +
              f'IPAddress BROKER_IP({broker.replace(".", ", ")});\n' +
              f'IPAddress LAPTOP_B_IP({receiver.replace(".", ", ")});\n' +
              f'const uint16_t MQTT_PORT = {mqtt_port};\nconst uint16_t TCP_PORT = {tcp_port};\n')
    for node in ('ESP32_A', 'ESP32_B'):
        private_write(target / node / 'local_config.h', header)
    private_write(target / 'config.local.json', json.dumps(config, indent=2) + '\n')
    return config


def local_addresses():
    try:
        interfaces = json.loads(subprocess.check_output(['ip', '-j', '-4', 'addr'], text=True))
        return [f'{iface["ifname"]}: {addr["local"]}' for iface in interfaces
                for addr in iface.get('addr_info', []) if addr.get('scope') == 'global']
    except (OSError, subprocess.SubprocessError, ValueError):
        return ['Tidak dapat autodetect IP. Periksa pengaturan jaringan lokal.']


def configure():
    if not sys.stdin.isatty():
        raise ValueError('configure perlu terminal lokal interaktif agar password tidak terekspos lewat pipe/log.')
    print('IP lokal saat ini (bukan konfigurasi tetap):', ', '.join(local_addresses()))
    print('Input privat lokal; password tidak tampil dan hanya masuk header ignored mode 600.')
    ssid = input('SSID Wi-Fi 2.4 GHz: ').strip()
    password = getpass.getpass('Password Wi-Fi (kosong untuk jaringan terbuka): ')
    broker = input('IPv4 LAN Laptop A / broker: ').strip()
    receiver = input('IPv4 LAN Laptop B / receiver: ').strip()
    mqtt_port = int(input('MQTT port [1883]: ').strip() or 1883)
    tcp_port = int(input('Receiver TCP port [5001]: ').strip() or 5001)
    if CONFIG.exists() and input('Timpa konfigurasi lokal lama? [yes/NO]: ') != 'yes':
        raise ValueError('Dibatalkan; konfigurasi lama dipertahankan')
    save_config(SOURCE, ssid, password, broker, receiver, mqtt_port, tcp_port)
    print('Konfigurasi lokal dibuat. Compile ulang dan upload manual KEDUA board setelah identitas/wiring diverifikasi.')
    print('Tidak ada flash, konfigurasi radio, atau perubahan jaringan yang dilakukan.')


def load_config():
    if not CONFIG.is_file():
        raise ValueError('Konfigurasi lokal belum ada: jalankan ./run_sender.sh configure (jangan kirim password lewat chat).')
    data = json.loads(CONFIG.read_text())
    for key in ('broker_ip', 'receiver_ip'):
        if str(ipaddress.IPv4Address(data[key])) == '0.0.0.0':
            raise ValueError(f'{key} belum diisi')
    for key in ('mqtt_port', 'tcp_port'):
        if not isinstance(data[key], int) or not 1 <= data[key] <= 65535:
            raise ValueError(f'{key} tidak valid')
    return data


def recv_exact(sock, length):
    result = b''
    while len(result) < length:
        data = sock.recv(length - len(result))
        if not data:
            raise OSError('MQTT connection closed')
        result += data
    return result


def mqtt_packet(sock):
    kind = recv_exact(sock, 1)[0]
    length = 0
    for i in range(4):
        digit = recv_exact(sock, 1)[0]
        length += (digit & 127) * (128 ** i)
        if not digit & 128:
            if length > 8192:
                raise OSError('MQTT packet unexpectedly large')
            return kind, recv_exact(sock, length)
    raise OSError('Invalid MQTT remaining length')


def mqtt_string(value):
    return struct.pack('!H', len(value)) + value


def mqtt_probe(host, port):
    """Real CONNECT/SUBSCRIBE/PUBLISH round trip on a unique diagnostics topic."""
    token = uuid.uuid4().hex[:16].encode()
    topic = b'kelompok2/diagnostics/' + token
    with socket.create_connection((host, port), timeout=3) as sock:
        body = b'\x00\x04MQTT\x04\x02\x00\x0a' + mqtt_string(b'probe-' + token)
        sock.sendall(b'\x10' + bytes([len(body)]) + body)
        if mqtt_packet(sock) != (0x20, b'\x00\x00'):
            raise OSError('Broker menolak MQTT CONNECT (auth/protocol)')
        body = b'\x00\x01' + mqtt_string(topic) + b'\x00'
        sock.sendall(b'\x82' + bytes([len(body)]) + body)
        if mqtt_packet(sock) != (0x90, b'\x00\x01\x00'):
            raise OSError('Broker menolak MQTT SUBSCRIBE')
        body = mqtt_string(topic) + token
        sock.sendall(b'\x30' + bytes([len(body)]) + body)
        if mqtt_packet(sock) != (0x30, body):
            raise OSError('MQTT PUBLISH round-trip gagal')
        sock.sendall(b'\xe0\x00')


@contextmanager
def managed_broker(host, port):
    """Own only this child process; never stop/reconfigure a system broker."""
    ipaddress.IPv4Address(host)
    executable = shutil.which('mosquitto')
    if not executable:
        raise ValueError('Mosquitto tidak terpasang. Install paket mosquitto atau gunakan broker external.')
    # Bind probe catches occupied/wrong-interface endpoints before launching.
    with socket.socket() as probe:
        probe.bind((host, port))
    with tempfile.TemporaryDirectory(prefix='pjk-broker-') as tmp:
        config = Path(tmp) / 'mosquitto.conf'
        config.write_text(f'listener {port} {host}\nallow_anonymous true\npersistence false\n')
        with (Path(tmp) / 'broker.log').open('w+') as log:
            process = subprocess.Popen([executable, '-c', str(config)], stdout=log, stderr=log)
            try:
                for _ in range(40):
                    if process.poll() is not None:
                        log.seek(0)
                        raise OSError('Mosquitto gagal: ' + log.read())
                    try:
                        mqtt_probe(host, port)
                        break
                    except OSError:
                        time.sleep(0.1)
                else:
                    raise OSError('Mosquitto startup timeout; periksa alamat bind/port.')
                yield
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


def preflight(args):
    print('Python:', sys.executable)
    print('IP lokal:', ', '.join(local_addresses()))
    print('Tool:', ', '.join(f'{x}={shutil.which(x) or "MISSING"}' for x in ('arduino-cli', 'mosquitto', 'g++')))
    ports = ports_now()
    for p in ports:
        print(f'Port {p.device}: {p.description}; serial={p.serial_number or "unknown"} (bukan konfirmasi ESP A)')
    errors = []
    try:
        choose_port(ports, args.port)
    except ValueError as exc:
        errors.append(str(exc))
    try:
        config = load_config()
        print('Endpoint lokal:', json.dumps(config))
        if args.probe:
            mqtt_probe(config['broker_ip'], config['mqtt_port'])
            with socket.create_connection((config['receiver_ip'], config['tcp_port']), timeout=3):
                pass
            print('MQTT round-trip + TCP listener reachable; bukan uji ESP/radio.')
    except (OSError, ValueError, KeyError) as exc:
        errors.append(str(exc))
    for error in errors:
        print('BLOCKER:', error)
    print('Preflight tidak membuka serial, flash, atau mengonfigurasi radio.')
    return 1 if errors else 0


def send(args):
    ports = ports_now()
    if not args.port and sys.stdin.isatty() and ports:
        for p in ports:
            print(p.device, p.description)
        args.port = input('Port ESP A (cocokkan board fisik): ').strip()
    port = choose_port(ports, args.port)
    config = load_config()
    filename = args.file
    if not filename and sys.stdin.isatty():
        filename = input('Path gambar JPG/PNG kecil: ').strip()
    if not filename:
        raise ValueError('Pilih gambar dengan --file atau prompt interaktif.')
    image = Path(filename).expanduser().resolve()
    if not image.is_file() or image.suffix.lower() not in ('.jpg', '.jpeg', '.png'):
        raise ValueError('File JPG/PNG tidak ditemukan.')
    if not args.lab_approved:
        if not sys.stdin.isatty() or input('Wiring, antena, catu, radio & aturan lab sudah diperiksa? [yes/NO]: ') != 'yes':
            raise ValueError('Belum ada persetujuan keselamatan lab. Tidak mengirim.')
    print('Anonymous MQTT hanya untuk LAN lab tepercaya. Jangan expose ke internet.')
    print('Firmware v2 + konfigurasi terbaru harus sudah di-upload manual ke A dan B.')
    from contextlib import nullcontext
    lifecycle = managed_broker(config['broker_ip'], config['mqtt_port']) if args.broker == 'managed' else nullcontext()
    with lifecycle:
        mqtt_probe(config['broker_ip'], config['mqtt_port'])
        with socket.create_connection((config['receiver_ip'], config['tcp_port']), timeout=3):
            pass
        command = [sys.executable, str(SOURCE / 'Laptop_A/send_image.py'), '--port', port, '--file', str(image)]
        result = subprocess.run(command)
        if result.returncode:
            raise RuntimeError('Transfer gagal/dibatalkan. Broker milik launcher dihentikan; periksa receiver, Wi-Fi, SPI READY, AUX, log B. Ulangi sebagai sesi baru.')
    print('Sender selesai dan broker managed dihentikan (broker external tidak disentuh).')


def main():
    ap = argparse.ArgumentParser(description='PJK guided workflow: no auto-flash, no radio configuration')
    commands = ap.add_subparsers(dest='command')
    check = commands.add_parser('preflight', help='Read-only inventory; nonzero jika blocker')
    check.add_argument('--port')
    check.add_argument('--probe', action='store_true', help='Explicit MQTT round-trip and TCP listener check')
    commands.add_parser('configure', help='Private local prompts; writes ignored headers, never flashes')
    start = commands.add_parser('send', help='Send via ESP A; broker lifecycle belongs to this invocation')
    start.add_argument('--port')
    start.add_argument('--file')
    start.add_argument('--broker', choices=['managed', 'external'], default='managed')
    start.add_argument('--lab-approved', action='store_true', help='Operator has verified wiring/radio/lab safety')
    commands.add_parser('build', help='Compile A/B/E220 only. Never upload or run E220_Config.')
    args = ap.parse_args()
    try:
        if args.command == 'configure':
            configure()
        elif args.command == 'preflight':
            return preflight(args)
        elif args.command == 'build':
            for node in ('ESP32_A', 'ESP32_B', 'E220_Config'):
                subprocess.run(['arduino-cli', 'compile', '--fqbn', 'esp32:esp32:esp32', str(SOURCE / node)], check=True)
        else:
            if args.command is None:
                args = ap.parse_args(['send'])
            send(args)
    except (OSError, ValueError, RuntimeError, KeyError, ImportError, subprocess.SubprocessError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        print('Diagnosis: ./run_sender.sh preflight ; dependensi: .venv/bin/python -m pip install -r requirements.txt ; lihat docs/LOCAL_WORKFLOW.md', file=sys.stderr)
        return 1
    except (KeyboardInterrupt, EOFError):
        print('Dibatalkan.', file=sys.stderr)
        return 130
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
