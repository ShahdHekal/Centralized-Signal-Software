import socket, struct, sys, threading
import numpy as np
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore

HOST = "127.0.0.1"     # central server
VIEWER_PORT = 9126
SECONDS = 20           


LABELS = {             
    "UnicornEEG": ["Fz", "C3", "Cz", "C4", "Pz", "PO7", "Oz", "PO8"]
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


class FloatDisplay:
    """Format 1 --> float samples, one row per channel, EEG, EMG & audio"""

    def __init__(self, win, name, channels, rate):
        self.channels, self.rate = channels, rate
        self.size = int(SECONDS * rate)
        self.values = np.zeros((self.size, channels), dtype=np.float32)
        self.times = np.zeros(self.size)
        self.pos = self.filled = 0
        labels = LABELS.get(name, [])
        labels += [f"ch {i + 1}" for i in range(len(labels), channels)]
        self.curves = []
        for ch in range(channels):
            plot = win.addPlot(row=ch, col=0)
            plot.setLabel("left", labels[ch])
            plot.getAxis("left").enableAutoSIPrefix(False)
            plot.setDownsampling(auto=True, mode="peak")
            plot.setClipToView(True)
            if ch:
                plot.setXLink(self.curves[0].getViewBox())
            if ch < channels - 1:
                plot.hideAxis("bottom")
            self.curves.append(plot.plot(pen=pg.intColor(ch, hues=channels)))

    def add(self, t0, payload):
        block = np.frombuffer(payload, "<f4").reshape(-1, self.channels)
        idx = (self.pos + np.arange(len(block))) % self.size
        self.values[idx] = block
        self.times[idx] = t0 + np.arange(len(block)) / self.rate   
        self.pos = (self.pos + len(block)) % self.size
        self.filled = min(self.size, self.filled + len(block))

    def draw(self):
        if self.filled < 2:
            return
        order = (self.pos - self.filled + np.arange(self.filled)) % self.size
        t, v = self.times[order], self.values[order]
        for ch, curve in enumerate(self.curves):
            curve.setData(t - t[-1], v[:, ch])   


class MidiDisplay:
    """Format 2 --> MIDI messages. Notes as dots, controller values as stepped lines."""

    def __init__(self, win, name, channels, rate):
        self.events = collections.deque(maxlen=20000)   # (sender time, message bytes)
        self.offset = None                              # this machine's clock minus the sender's 
        self.note_plot = win.addPlot(row=0, col=0)
        self.note_plot.setLabel("left", "note")
        self.notes = self.note_plot.plot(pen=None, symbol="o", symbolSize=6, symbolBrush="c")
        self.cc_plot = win.addPlot(row=1, col=0)
        self.cc_plot.setLabel("left", "controller")
        self.cc_plot.setXLink(self.note_plot)
        self.cc_plot.addLegend()
        self.cc_curves = {}

    def add(self, t0, payload):
        offset = time.monotonic() - t0
        if self.offset is None or offset < self.offset:
            self.offset = offset
        self.events.append((t0, bytes(payload)))

    def draw(self):
        if self.offset is None:
            return
        now = time.monotonic() - self.offset   # "now" on the sender's clock
        recent = [(t - now, m) for t, m in self.events if t >= now - SECONDS]
        hits = [(t, m[1]) for t, m in recent if len(m) == 3 and m[0] & 0xF0 == 0x90 and m[2] > 0]
        self.notes.setData([t for t, _ in hits], [n for _, n in hits])
        cc = {}   # controller number, (times, values)
        lines = {}   # (kind, MIDI channel), (times, values)
        
        for t, m in recent:
            kind, ch = m[0] & 0xF0, (m[0] & 0x0F) + 1
            if kind == 0xB0 and len(m) == 3:                       # controller
                key, value = (f"CC {m[1]}", ch), m[2]
            elif kind == 0xD0 and len(m) >= 2:                     # channel pressure
                key, value = ("pressure", ch), m[1]
            elif kind == 0xE0 and len(m) == 3:                     # pitch bend, scaled to 0-127 like the others
                key, value = ("bend", ch), (m[1] + m[2] * 128) / 128
            else:
                continue
            lines.setdefault(key, ([], []))
            lines[key][0].append(t)
            lines[key][1].append(value)
            
        for (label, ch), (ts, vs) in lines.items():
            if (label, ch) not in self.cc_curves:
                self.cc_curves[(label, ch)] = self.cc_plot.plot(
                    pen=pg.intColor(ch - 1, hues=16), name=f"{label} (ch {ch})")
            self.cc_curves[(label, ch)].setData(ts + [0], vs + [vs[-1]], stepMode="left")   # hold to now

        self.note_plot.setXRange(-SECONDS, 0, padding=0)


DISPLAYS = {1: FloatDisplay, 2: MidiDisplay}


stream = sys.argv[1] if len(sys.argv) > 1 else "UnicornEEG"
sock = socket.create_connection((HOST, VIEWER_PORT))
sock.sendall((stream + "\n").encode())
header = read_packet(sock)
n = struct.unpack_from("<H", header)[0]
name = header[2:2 + n].decode()

channels, rate, fmt = struct.unpack_from("<HdB", header, 2 + n)
print(f"{name}: {channels} channels, {rate:g} Hz")

app = pg.mkQApp()
win = pg.GraphicsLayoutWidget(title=name)
win.resize(1000, 900)
display = DISPLAYS[fmt](win, name, channels, rate)
lock = threading.Lock()

def reader():
    while True:
        body = read_packet(sock)
        t0 = struct.unpack_from("<d", body, 4)[0]
        with lock:
             display.add(t0, body[12:])


def redraw():
    with lock:
        display.draw()

threading.Thread(target=reader, daemon=True).start()
timer = QtCore.QTimer()
timer.timeout.connect(redraw)
timer.start(33)
win.show()
app.exec()
