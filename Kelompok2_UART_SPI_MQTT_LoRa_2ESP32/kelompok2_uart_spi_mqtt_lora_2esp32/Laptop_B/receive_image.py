import socket
import struct
import zlib
from pathlib import Path

HOST = "0.0.0.0"
PORT = 5001
MAGIC = b"K2IM"
FRAME_SIZE = 200
HEADER_SIZE = 15
PAYLOAD_SIZE = FRAME_SIZE - HEADER_SIZE
TYPE_START, TYPE_DATA, TYPE_END = 1, 2, 3

OUT_DIR = Path("received_images")
OUT_DIR.mkdir(exist_ok=True)

def read_exact(conn, n):
    chunks = []
    remaining = n
    while remaining:
        part = conn.recv(remaining)
        if not part:
            return None
        chunks.append(part)
        remaining -= len(part)
    return b"".join(chunks)

def parse_frame(frame):
    if len(frame) != FRAME_SIZE:
        raise ValueError("Panjang frame salah")
    magic, frame_type, seq, length = struct.unpack("<4sBIH", frame[:11])
    crc_expected = struct.unpack("<I", frame[11:15])[0]
    if magic != MAGIC or length > PAYLOAD_SIZE:
        raise ValueError("Header tidak valid")
    payload = frame[HEADER_SIZE:HEADER_SIZE + length]
    crc_actual = zlib.crc32(frame[4:11] + payload) & 0xFFFFFFFF
    if crc_actual != crc_expected:
        raise ValueError("CRC frame tidak cocok")
    return frame_type, seq, payload

def main():
    current_file = None
    current_path = None
    expected_size = 0
    written_size = 0
    whole_crc = 0
    last_seq = -1
    completed = False

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((HOST, PORT))
    server.listen(1)

    print(f"TCP receiver aktif di {HOST}:{PORT}")
    print(f"Folder hasil: {OUT_DIR.resolve()}")

    while True:
        conn, addr = server.accept()
        print("ESP32 A terhubung dari:", addr)
        with conn:
            while True:
                frame = read_exact(conn, FRAME_SIZE)
                if frame is None:
                    print("Koneksi terputus.")
                    break
                try:
                    frame_type, seq, payload = parse_frame(frame)
                except ValueError as exc:
                    print("Frame ditolak:", exc)
                    continue

                # Duplicate frame: do not write twice, but ACK it again.
                if seq == last_seq:
                    conn.sendall(f"ACK:{seq}\n".encode())
                    continue

                if frame_type == TYPE_START:
                    if len(payload) < 6:
                        print("START tidak valid")
                        continue
                    expected_size, ext_code, name_len = struct.unpack("<IBB", payload[:6])
                    if name_len > len(payload) - 6:
                        print("Nama file START tidak valid")
                        continue
                    name = payload[6:6 + name_len].decode("utf-8", errors="replace")
                    safe_name = Path(name).name
                    suffix = ".jpg" if ext_code == 1 else ".png" if ext_code == 2 else ".bin"
                    if Path(safe_name).suffix.lower() not in (".jpg", ".jpeg", ".png"):
                        safe_name = "received_image" + suffix

                    if current_file:
                        current_file.close()
                    current_path = OUT_DIR / ("received_" + safe_name)
                    current_file = open(current_path.with_suffix(current_path.suffix + ".part"), "wb")
                    expected_size = int(expected_size)
                    written_size = 0
                    whole_crc = 0
                    completed = False
                    last_seq = seq
                    print(f"Mulai menerima {safe_name}, target {expected_size} byte")

                elif frame_type == TYPE_DATA:
                    if current_file is None or seq != last_seq + 1:
                        print("DATA di luar urutan; tidak di-ACK:", seq)
                        continue
                    current_file.write(payload)
                    written_size += len(payload)
                    whole_crc = zlib.crc32(payload, whole_crc)
                    last_seq = seq

                elif frame_type == TYPE_END:
                    if current_file is None or current_path is None or len(payload) != 4 or seq != last_seq + 1:
                        print("END tidak valid/di luar urutan")
                        continue
                    expected_crc = struct.unpack("<I", payload)[0]
                    current_file.flush()
                    current_file.close()
                    current_file = None
                    actual_crc = whole_crc & 0xFFFFFFFF
                    completed = (written_size == expected_size and actual_crc == expected_crc)
                    print("File:", current_path)
                    print("Ukuran diterima:", written_size, "/", expected_size)
                    print("CRC file:", "COCOK" if actual_crc == expected_crc else "TIDAK COCOK")
                    print("Status:", "BERHASIL" if completed else "GAGAL")
                    if not completed:
                        # Never report end-to-end success for a corrupt/truncated file.
                        continue
                    current_path.with_suffix(current_path.suffix + ".part").replace(current_path)
                    last_seq = seq
                else:
                    print("Jenis frame tidak dikenal:", frame_type)
                    continue

                conn.sendall(f"ACK:{seq}\n".encode())

if __name__ == "__main__":
    main()
