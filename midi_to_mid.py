"""Record ROLI keyboard + Airwave MIDI into a standard MIDI file (.mid).

Each device gets its own track, so the keyboard and Airwave stay separate
when you open the file in Ableton, Logic, GarageBand or any MIDI editor.

Setup (no admin needed):
    python3 -m venv midi-env
    source midi-env/bin/activate
    pip install mido python-rtmidi

Run:
    python3 midi_to_mid.py              # records whichever devices are connected
    python3 midi_to_mid.py airwave      # records only the Airwave
    python3 midi_to_mid.py keyboard     # records only the keyboard
It prints the MIDI inputs it can see, then records until you press Ctrl+C.
The file is also saved every few seconds, so a crash loses at most the last
few seconds of playing.
"""
import os
import queue
import sys
import time
from datetime import datetime

import mido

# Part of each device's MIDI port name (case-insensitive).
# Airwave is matched first so its port isn't mistaken for the keyboard's.
# Change "keyboard" to match your keyboard's name in the printed list.
PORTS = {
    "airwave": "Airwave Expression",
    "keyboard": "Seaboard",  # e.g. "ROLI Piano", "Piano M", "Seaboard"
}

# Order of the tracks in the saved file.
TRACK_ORDER = ["keyboard", "airwave"]

# 120 BPM is the default tempo in most DAWs, so the file plays back in real
# time without changing the project tempo. 960 ticks per beat at 120 BPM
# gives a timing resolution of about 0.5 ms.
TEMPO = mido.bpm2tempo(120)
TICKS_PER_BEAT = 960

# Message types not worth keeping. Remove "sysex" to keep ROLI's system messages.
SKIP = {"clock", "active_sensing", "sysex"}

AUTOSAVE_SECONDS = 10


def find_ports(wanted):
    """Find the MIDI input for each wanted device. Devices that aren't
    connected are skipped; the script only stops if none are found."""
    names = mido.get_input_names()
    print("MIDI inputs found:")
    for name in names:
        print("   ", name)
    print()

    chosen = {}
    for device, part in PORTS.items():
        if device not in wanted:
            continue
        match = next((n for n in names
                      if part.lower() in n.lower() and n not in chosen.values()),
                     None)
        if match is None:
            print(f"No input matching '{part}', so {device} won't be recorded.")
        else:
            chosen[device] = match

    if not chosen:
        sys.exit("No devices to record. Check the connections, or edit PORTS "
                 "at the top of this script to match the names listed above.")
    return chosen


def build_file(recorded):
    """Turn {device: [(seconds since start, message), ...]} into a MIDI file.

    MIDI files store the gap since the previous event in ticks, so each
    event's absolute time is converted to ticks and then to a gap.
    """
    mid = mido.MidiFile(type=1, ticks_per_beat=TICKS_PER_BEAT)
    devices = [d for d in TRACK_ORDER if d in recorded]  # one track per recorded device
    for i, device in enumerate(devices):
        track = mido.MidiTrack()
        track.append(mido.MetaMessage("track_name", name=device, time=0))
        if i == 0:  # the tempo goes in the first track
            track.append(mido.MetaMessage("set_tempo", tempo=TEMPO, time=0))

        last_tick = 0
        for t, msg in recorded[device]:
            tick = round(mido.second2tick(t, TICKS_PER_BEAT, TEMPO))
            track.append(msg.copy(time=max(0, tick - last_tick)))
            last_tick = max(last_tick, tick)
        mid.tracks.append(track)
    return mid


def save(recorded, filename):
    # Write to a temporary file first, so an interrupted save never leaves
    # a half-written .mid behind.
    tmp = filename + ".tmp"
    build_file(recorded).save(tmp)
    os.replace(tmp, filename)


def main():
    # Devices named on the command line, or every device if none are named.
    wanted = sys.argv[1:] or TRACK_ORDER
    unknown = [d for d in wanted if d not in PORTS]
    if unknown:
        sys.exit(f"Unknown device {', '.join(unknown)}. "
                 f"Choose from: {', '.join(TRACK_ORDER)}")

    chosen = find_ports(wanted)
    events = queue.Queue()
    t0 = time.perf_counter()

    ports = []
    for device, name in chosen.items():
        # Timestamp inside the callback, the moment the message arrives.
        def callback(msg, device=device):
            events.put((time.perf_counter() - t0, device, msg))
        ports.append(mido.open_input(name, callback=callback))
        print(f"Recording {device}: {name}")

    recorded = {device: [] for device in TRACK_ORDER if device in chosen}
    filename = datetime.now().strftime("midi_session_%Y%m%d_%H%M%S.mid")
    print(f"\nWriting to {filename}. Press Ctrl+C to stop.")

    def take(item):
        t, device, msg = item
        if msg.type not in SKIP:
            recorded[device].append((t, msg))

    next_save = time.monotonic() + AUTOSAVE_SECONDS
    try:
        while True:
            try:
                take(events.get(timeout=0.5))
            except queue.Empty:
                pass
            if time.monotonic() >= next_save:
                save(recorded, filename)
                next_save = time.monotonic() + AUTOSAVE_SECONDS
    except KeyboardInterrupt:
        pass
    finally:
        for port in ports:
            port.close()
        while not events.empty():  # keep anything still queued
            take(events.get_nowait())
        save(recorded, filename)

    counts = ", ".join(f"{len(events)} {d}" for d, events in recorded.items())
    print(f"\nSaved {counts} events to {filename}")


if __name__ == "__main__":
    main()