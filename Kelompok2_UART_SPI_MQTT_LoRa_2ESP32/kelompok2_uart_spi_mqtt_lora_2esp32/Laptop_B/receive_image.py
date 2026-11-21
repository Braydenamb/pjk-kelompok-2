import argparse
import socket
import struct
import zlib
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol import *

def read_exact(conn, n):
    data = bytearray()
    while len(data) < n:
        part = conn.recv(n - len(data))
        if not part:
            return None
        data.extend(part)
    return bytes(data)

def main():
    ap = argparse.ArgumentParser(description="Validated TCP image receiver (v2)")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=5001)
    ap.add_argument("--output", type=Path, default=Path("received_images"))
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    current_file = None
    current_path = None
    active = None
    retired = set()
    last_frame = None
    last_seq = -1
    written = expected = crc = 0
    with socket.socket() as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.bind, args.port))
        server.listen(1)
        print(f"TCP receiver aktif di {args.bind}:{server.getsockname()[1]}", flush=True)
        print(f"Folder hasil: {args.output.resolve()}", flush=True)
        try:
            while True:
                conn, addr = server.accept()
                print("Connected:", addr, flush=True)
                with conn:
                    while True:
                        frame = read_exact(conn, FRAME_SIZE)
                        if frame is None:
                            break
                        try:
                            kind, session, seq, payload = parse_frame(frame)
                        except ValueError as exc:
                            print("Frame ditolak:", exc, flush=True)
                            continue
                        if session in retired:
                            continue
                        if kind == TYPE_ABORT and session == active:
                            if current_file:
                                current_file.close()
                            current_file = None
                            retired.add(session)
                            active = None
                            last_frame = None
                            conn.sendall(f"ACK:{session}:{seq}\n".encode())
                            continue
                        if session == active and frame == last_frame:
                            conn.sendall(f"ACK:{session}:{seq}\n".encode())
                            continue
                        if kind == TYPE_START:
                            if seq != 0 or len(payload) < 6 or session == active:
                                continue
                            size, ext, n = struct.unpack("<IBB", payload[:6])
                            if n != len(payload) - 6 or not n or ext not in (1, 2):
                                continue
                            name = Path(payload[6:].decode("utf-8", errors="replace")).name
                            if not name or "\x00" in name:
                                continue
                            if current_file:
                                current_file.close()
                            if active is not None:
                                retired.add(active)
                            active = session
                            current_path = args.output / ("received_" + name)
                            current_file = open(str(current_path) + ".part", "wb")
                            written = crc = 0
                            expected = size
                        elif session != active or current_file is None or seq != last_seq + 1:
                            continue
                        elif kind == TYPE_DATA:
                            current_file.write(payload)
                            written += len(payload)
                            crc = zlib.crc32(payload, crc)
                        elif kind == TYPE_END:
                            if len(payload) != 4 or written != expected or crc & 0xffffffff != struct.unpack("<I", payload)[0]:
                                print("END rejected: size/CRC mismatch", flush=True)
                                continue
                            current_file.close()
                            current_file = None
                            assert current_path is not None
                            Path(str(current_path) + ".part").replace(current_path)
                            print("BERHASIL:", current_path, flush=True)
                        else:
                            continue
                        last_seq = seq
                        last_frame = frame
                        conn.sendall(f"ACK:{session}:{seq}\n".encode())
        finally:
            if current_file:
                current_file.close()

if __name__ == "__main__":
    main()
