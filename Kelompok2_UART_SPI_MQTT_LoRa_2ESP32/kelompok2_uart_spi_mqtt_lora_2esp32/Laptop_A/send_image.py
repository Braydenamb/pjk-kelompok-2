import argparse
import struct
import time
import zlib
from pathlib import Path

import serial

import sys
import secrets
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol import *

def wait_line(ser, wanted: str, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = ser.readline().decode("utf-8", errors="ignore").strip()
        if not line:
            continue
        print("ESP32 A:", line)
        if line.startswith("ERR:"):
            raise RuntimeError(line)
        if line == wanted:
            return True
    return False

def send_and_wait_ack(ser, frame: bytes, seq: int, *, session, timeout: float = 90.0):
    ser.write(frame)
    ser.flush()
    wanted = f"ACK:{session}:{seq}"
    if not wait_line(ser, wanted, timeout):
        raise TimeoutError(
            f"Tidak menerima {wanted}. Jangan langsung mengirim frame berikutnya. "
            "Periksa log ESP32 dan koneksi pada tahap yang gagal."
        )

def main():
    ap = argparse.ArgumentParser(description="Kirim JPG/PNG melalui UART -> SPI -> MQTT -> LoRa")
    ap.add_argument("--port", required=True, help="Contoh: COM6")
    ap.add_argument("--file", required=True, help="Path file JPG/PNG")
    ap.add_argument("--baud", type=int, default=460800)
    args = ap.parse_args()

    session = secrets.randbelow(0xffffffff) + 1
    path = Path(args.file)
    if not path.is_file():
        raise FileNotFoundError(path)
    ext = path.suffix.lower()
    if ext in (".jpg", ".jpeg"):
        ext_code = 1
    elif ext == ".png":
        ext_code = 2
    else:
        raise ValueError("File harus .jpg, .jpeg, atau .png")

    data = path.read_bytes()
    name = path.name.encode("utf-8")
    if len(name) > 170:
        raise ValueError("Nama file terlalu panjang")

    print(f"File: {path.name}")
    print(f"Ukuran: {len(data)} byte")
    print("Membuka serial...")

    with serial.Serial(args.port, args.baud, timeout=1, write_timeout=10) as ser:
        time.sleep(2)
        ser.reset_input_buffer()

        print("Menunggu ESP32 A siap (READY)...")
        if not wait_line(ser, "READY", 180):
            raise TimeoutError("ESP32 A belum READY. Pastikan broker, Laptop B, dan kedua ESP32 aktif.")

        try:
            start_time = time.monotonic()
            start_payload = struct.pack("<IBB", len(data), ext_code, len(name)) + name
            send_and_wait_ack(ser, make_frame(TYPE_START, 0, start_payload, session=session), 0, session=session)

            seq = 1
            for offset in range(0, len(data), PAYLOAD_SIZE):
                chunk = data[offset:offset + PAYLOAD_SIZE]
                send_and_wait_ack(ser, make_frame(TYPE_DATA, seq, chunk, session=session), seq, session=session)
                seq += 1
                sent = min(offset + len(chunk), len(data))
                print(f"Progres data: {sent}/{len(data)} byte")

            end_payload = struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF)
            send_and_wait_ack(ser, make_frame(TYPE_END, seq, end_payload, session=session), seq, session=session)

            elapsed = time.monotonic() - start_time
            rate = (len(data) * 8 / elapsed) if elapsed > 0 else 0
            print(f"Selesai. Waktu: {elapsed:.2f} detik")
            print(f"Throughput end-to-end: {rate:.2f} bit/detik")
        except (Exception, KeyboardInterrupt):
            ser.write(make_frame(TYPE_ABORT, 0, session=session))
            ser.flush()
            raise

if __name__ == "__main__":
    main()
