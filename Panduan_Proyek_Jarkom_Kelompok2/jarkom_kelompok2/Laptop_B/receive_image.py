"""Terima file gambar dari ESP32-B melalui USB-UART dan simpan ke disk."""
import argparse
import sys
import time
from pathlib import Path

import serial

BAUD = 460800


def main():
    parser = argparse.ArgumentParser(description="Terima JPG/PNG dari ESP32-B")
    parser.add_argument("port", help="Contoh Windows COM6, macOS /dev/cu.SLAB_USBtoUART")
    parser.add_argument("--output", default="received_images", help="Folder output")
    args = parser.parse_args()

    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"Membuka {args.port} @ {BAUD}. Menunggu frame START dari ESP32-B...")
    with serial.Serial(args.port, BAUD, timeout=60, write_timeout=30) as ser:
        ser.reset_input_buffer()
        time.sleep(1.5)

        # Abaikan boot text atau baris lain; tunggu metadata START.
        header = None
        deadline = time.monotonic() + 600
        while time.monotonic() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if line.startswith("START|"):
                header = line
                break

        if header is None:
            raise TimeoutError("Tidak menerima header START selama 10 menit")

        parts = header.split("|", 2)
        if len(parts) != 3:
            raise ValueError(f"Header tidak valid: {header}")

        filename = Path(parts[1]).name
        file_size = int(parts[2])
        if file_size <= 0:
            raise ValueError("Ukuran file tidak valid")

        output_path = output_dir / f"received_{filename}"
        print(f"Nama file : {filename}")
        print(f"Ukuran    : {file_size:,} byte")
        print(f"Disimpan  : {output_path.resolve()}")

        remaining = file_size
        with output_path.open("wb") as target:
            while remaining > 0:
                amount = min(4096, remaining)
                block = ser.read(amount)
                if not block:
                    raise TimeoutError(
                        f"Timeout saat menerima data. Sisa {remaining:,} byte."
                    )
                target.write(block)
                remaining -= len(block)
                received = file_size - remaining
                print(f"Progress: {received:,}/{file_size:,} byte", end="\r")

        print("\nFile selesai diterima.")
        # END adalah baris kontrol setelah byte gambar terakhir.
        ser.timeout = 5
        end_line = ser.readline().decode("utf-8", errors="replace").strip()
        if end_line:
            print(f"Kontrol akhir: {end_line}")
        print(f"File tersimpan: {output_path.resolve()}")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"GAGAL: {exc}", file=sys.stderr)
        sys.exit(1)
