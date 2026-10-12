# fake_sender.py
# Pretends to be a device client: connects to the server and sends packets
# in the same format as central_sender.cpp, so the server can be tested
# without real hardware or the C++ sender.
#
# Run:  python3 fake_sender.py [host] [port]

import math
import socket
import struct
import sys
import time

host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 9125

# "<" = little-endian with no padding, matching the C++ sender.
#   H = u16, I = u32, d = f64, B = u8, f = f32
def packet(body):
    return struct.pack("<I", len(body)) + body

sock = socket.create_connection((host, port))

# First packet: the stream description.
# [u16 name length][name][u16 channels][f64 rate][u8 format]
name = b"FakeEEG"
channels, rate, fmt = 17, 250.0, 1          # 1 = FORMAT_FLOAT32
header = struct.pack("<H", len(name)) + name + struct.pack("<HdB", channels, rate, fmt)
pkt = packet(header)

# Send it in two pieces with a pause, to check that the server copes with
# a packet arriving in parts.
sock.sendall(pkt[:3])
time.sleep(0.2)
sock.sendall(pkt[3:])

# Then 10 data packets: [u32 seq][f64 t0][17 x f32 samples]
for seq in range(10):
    t0 = time.monotonic()
    value = math.sin(2 * math.pi * seq / 10)
    samples = struct.pack(f"<{channels}f", *([value] * channels))
    sock.sendall(packet(struct.pack("<Id", seq, t0) + samples))
    time.sleep(0.1)

sock.close()
print("sent 1 header packet and 10 data packets")