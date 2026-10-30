import socket, struct, sys, threading
import numpy as np
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore

HOST = "127.0.0.1"     # central server
VIEWER_PORT = 9126
SECONDS = 20           


LABELS = {             
    "UnicornEEG": [f"EEG {i}" for i in range(1, 9)]
                  + ["Acc X", "Acc Y", "Acc Z", "Gyr X", "Gyr Y", "Gyr Z", "Battery", "Counter", "Valid"],
}


def recv_exact(sock, n):
    """Read exactly n bytes; TCP may deliver them in pieces."""
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("server closed the connection")
        data += chunk
    return data


def read_packet(sock):
    """Read one [u32 length][body] packet and return the body."""
    length = struct.unpack("<I", recv_exact(sock, 4))[0]
    return recv_exact(sock, length)

stream = sys.argv[1] if len(sys.argv) > 1 else "UnicornEEG"
sock = socket.create_connection((HOST, VIEWER_PORT))
sock.sendall((stream + "\n").encode())
header = read_packet(sock)
n = struct.unpack_from("<H", header)[0]
name = header[2:2 + n].decode()

channels, rate, fmt = struct.unpack_from("<HdB", header, 2 + n)
print(f"{name}: {channels} channels, {rate:g} Hz")
if fmt != 1:
    raise SystemExit("only float streams are supported")

labels = LABELS.get(name, [])
labels += [f"ch {i + 1}" for i in range(len(labels), channels)]

size = int(SECONDS * rate)
values = np.zeros((size, channels), dtype=np.float32)
times = np.zeros(size)
pos = filled = 0
lock = threading.Lock()


def reader():
    global pos, filled
   
    while True:
        body = read_packet(sock)
        t0 = struct.unpack_from("<d", body, 4)[0]
        block = np.frombuffer(body, "<f4", offset=12).reshape(-1, channels)
        idx = (pos + np.arange(len(block))) % size
        with lock:
            values[idx] = block
            times[idx] = t0 + np.arange(len(block)) / rate   # one time per scan
            pos = (pos + len(block)) % size
            filled = min(size, filled + len(block))


threading.Thread(target=reader, daemon=True).start()

app = pg.mkQApp()
win = pg.GraphicsLayoutWidget(title=f"{name} ({rate:g} Hz)")
win.resize(1000, 900)
curves = []

for ch in range(channels):
    plot = win.addPlot(row=ch, col=0)
    plot.setLabel("left", labels[ch])
    plot.getAxis("left").enableAutoSIPrefix(False)   
    plot.setDownsampling(auto=True, mode="peak")   
    plot.setClipToView(True)
    if ch:
        plot.setXLink(curves[0].getViewBox())
    if ch < channels - 1:
        plot.hideAxis("bottom")
    curves.append(plot.plot(pen=pg.intColor(ch, hues=channels)))


def redraw():
    with lock:
        if filled < 2:
            return
        order = (pos - filled + np.arange(filled)) % size
        t, v = times[order], values[order]
    for ch, curve in enumerate(curves):
        curve.setData(t - t[-1], v[:, ch])   


timer = QtCore.QTimer()
timer.timeout.connect(redraw)
timer.start(33)
win.show()
app.exec()
