"""Kirim file gambar dari Laptop A ke ESP32-A melalui USB-UART."""
import argparse
import os
import sys
import time
from pathlib import Path

import serial

BAUD = 460800
CHUNK = 180


def wait_for_line(ser, expected, timeout=90):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        # ESP ROM boot bytes use a different baud and can share a line with
        # the application's first READY. Accept only a final READY preceded
        # by observed non-text boot noise, not a printable status like NOTREADY.
        if expected == "READY":
            boot_line = raw.rstrip(b"\r\n")
            prefix = boot_line[:-5] if boot_line.endswith(b"READY") else b""
            if prefix and any(value < 32 or value > 126 for value in prefix):
                print("ESP32-A: READY (boot noise ignored)")
                return "READY"
        line = raw.decode("utf-8", errors="replace").strip()
        if not line:
            continue
        print(f"ESP32-A: {line}")
        if line.startswith("ERR"):
            raise RuntimeError(line)
        if line == expected or (callable(expected) and expected(line)):
            return line
    raise TimeoutError(f"Timeout menunggu respons: {expected}")


def open_sender_serial(port):
    # Set modem controls before opening, matching the successful READY probe.
    # Avoid asserting EN/BOOT through the USB serial auto-reset circuit.
    connection = serial.Serial(port=None, baudrate=BAUD, timeout=1, write_timeout=30)
    connection.dtr = False
    connection.rts = False
    connection.port = port
    connection.open()
    return connection


def main():
    parser = argparse.ArgumentParser(description="Kirim JPG/PNG melalui UART-SPI-MQTT-LoRa")
    parser.add_argument("port", help="Contoh Windows COM5, macOS /dev/cu.SLAB_USBtoUART")
    parser.add_argument("image", help="Path file .jpg atau .png")
    args = parser.parse_args()

    image_path = Path(args.image)
    if not image_path.is_file():
        raise FileNotFoundError(image_path)
    if image_path.suffix.lower() not in (".jpg", ".jpeg", ".png"):
        raise ValueError("File harus JPG/JPEG atau PNG")

    file_size = image_path.stat().st_size
    filename = image_path.name.replace("|", "_")
    if len(filename.encode("utf-8")) > 63:
        raise ValueError("Nama file maksimal 63 byte UTF-8")

    print(f"File       : {image_path}")
    print(f"Ukuran     : {file_size:,} byte")
    print(f"Serial port: {args.port} @ {BAUD}")

    with open_sender_serial(args.port) as ser:
        # Membuka port dapat mereset ESP32. Bersihkan boot text sebelum menunggu READY.
        ser.reset_input_buffer()
        time.sleep(2.5)
        wait_for_line(ser, "READY", timeout=120)

        header = f"START|{filename}|{file_size}\n".encode("utf-8")
        ser.write(header)
        ser.flush()
        wait_for_line(ser, "READY_DATA", timeout=90)

        sequence = 1
        sent = 0
        with image_path.open("rb") as source:
            while True:
                block = source.read(CHUNK)
                if not block:
                    break
                ser.write(block)
                ser.flush()
                wait_for_line(ser, f"ACK|{sequence}", timeout=90)
                sent += len(block)
                print(f"Progress: {sent:,}/{file_size:,} byte")
                sequence += 1

        wait_for_line(ser, "DONE", timeout=90)
        print("Pengiriman selesai. Periksa file hasil di Laptop B.")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"GAGAL: {exc}", file=sys.stderr)
        sys.exit(1)
