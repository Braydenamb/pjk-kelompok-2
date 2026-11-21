"""Version 2 fixed-size wire format shared by sender and receiver."""
import struct
import zlib
MAGIC = b"K2I2"
FRAME_SIZE = 200
HEADER_SIZE = 19
PAYLOAD_SIZE = FRAME_SIZE - HEADER_SIZE
TYPE_START, TYPE_DATA, TYPE_END, TYPE_ACK, TYPE_ABORT, TYPE_ERROR = range(1, 7)

def make_frame(frame_type, seq, payload=b"", *, session):
    if not 0 < session <= 0xffffffff:
        raise ValueError("Session must be nonzero uint32")
    if len(payload) > PAYLOAD_SIZE:
        raise ValueError("Payload terlalu besar")
    head = struct.pack("<4sBIHI", MAGIC, frame_type, seq, len(payload), session)
    crc = zlib.crc32(head[4:] + payload) & 0xffffffff
    return (head + struct.pack("<I", crc) + payload).ljust(FRAME_SIZE, b"\0")

def parse_frame(frame):
    if len(frame) != FRAME_SIZE:
        raise ValueError("Panjang frame salah")
    magic, kind, seq, length, session = struct.unpack("<4sBIHI", frame[:15])
    if magic != MAGIC or not session or length > PAYLOAD_SIZE:
        raise ValueError("Header tidak valid")
    payload = frame[HEADER_SIZE:HEADER_SIZE + length]
    if zlib.crc32(frame[4:15] + payload) & 0xffffffff != struct.unpack("<I", frame[15:19])[0]:
        raise ValueError("CRC frame tidak cocok")
    return kind, session, seq, payload
