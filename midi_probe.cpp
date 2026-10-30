// midi_probe.cpp
// List MIDI input ports, let the user choose one, and print incoming MIDI
// messages with their raw bytes, a monotonic timestamp, and a basic decoding.
//
// Build on macOS with Homebrew RtMidi:
//   brew install rtmidi
//   clang++ -std=c++17 -Wall -Wextra midi_probe.cpp -o midi_probe \
//       $(pkg-config --cflags --libs rtmidi)
// Run:
//   ./midi_probe
// Stop with Ctrl+C.

#include <RtMidi.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t keepRunning = 1;

void handleSignal(int) {
    keepRunning = 0;
}

struct CallbackContext {
    std::string portName;
    std::mutex outputMutex;
};

// Uses the same monotonic-clock basis as central_sender.cpp's now_seconds().
double monotonicSeconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

std::string noteName(int note) {
    static const char* names[] = {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"
    };
    std::ostringstream out;
    out << names[note % 12] << (note / 12 - 1);
    return out.str();
}

std::string rawBytes(const std::vector<unsigned char>& message) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < message.size(); ++i) {
        if (i != 0) out << ' ';
        out << std::setw(2) << static_cast<unsigned int>(message[i]);
    }
    return out.str();
}

std::string describeMessage(const std::vector<unsigned char>& message) {
    if (message.empty()) return "empty MIDI message";

    const unsigned char status = message[0];

    // System messages (including SysEx) do not use MIDI channels in the same
    // way as channel-voice messages, so report their status and payload length.
    if (status >= 0xF0) {
        if (status == 0xF0) {
            return "System Exclusive (SysEx), " +
                   std::to_string(message.size()) + " bytes";
        }
        std::ostringstream out;
        out << "System message, status=0x" << std::uppercase << std::hex
            << static_cast<unsigned int>(status);
        return out.str();
    }

    const unsigned char type = status & 0xF0;
    const int channel = (status & 0x0F) + 1;  // Show MIDI channels as 1..16.

    // Most channel-voice messages have two data bytes; Program Change and
    // Channel Pressure have one. Guard each decode against short messages.
    switch (type) {
        case 0x80:  // Note Off
            if (message.size() >= 3) {
                const int note = message[1] & 0x7F;
                const int velocity = message[2] & 0x7F;
                return "Note Off, channel=" + std::to_string(channel) +
                       ", note=" + std::to_string(note) + " (" + noteName(note) +
                       "), release_velocity=" + std::to_string(velocity);
            }
            break;

        case 0x90:  // Note On (velocity zero is conventionally Note Off)
            if (message.size() >= 3) {
                const int note = message[1] & 0x7F;
                const int velocity = message[2] & 0x7F;
                if (velocity == 0) {
                    return "Note Off (Note On with velocity 0), channel=" +
                           std::to_string(channel) + ", note=" +
                           std::to_string(note) + " (" + noteName(note) + ")";
                }
                return "Note On, channel=" + std::to_string(channel) +
                       ", note=" + std::to_string(note) + " (" + noteName(note) +
                       "), velocity=" + std::to_string(velocity);
            }
            break;

        case 0xA0:  // Polyphonic Key Pressure / Aftertouch
            if (message.size() >= 3) {
                return "Polyphonic Aftertouch, channel=" + std::to_string(channel) +
                       ", note=" + std::to_string(message[1] & 0x7F) +
                       ", pressure=" + std::to_string(message[2] & 0x7F);
            }
            break;

        case 0xB0:  // Control Change
            if (message.size() >= 3) {
                return "Control Change, channel=" + std::to_string(channel) +
                       ", controller=" + std::to_string(message[1] & 0x7F) +
                       ", value=" + std::to_string(message[2] & 0x7F);
            }
            break;

        case 0xC0:  // Program Change
            if (message.size() >= 2) {
                return "Program Change, channel=" + std::to_string(channel) +
                       ", program=" + std::to_string(message[1] & 0x7F);
            }
            break;

        case 0xD0:  // Channel Pressure / Aftertouch
            if (message.size() >= 2) {
                return "Channel Pressure, channel=" + std::to_string(channel) +
                       ", pressure=" + std::to_string(message[1] & 0x7F);
            }
            break;

        case 0xE0:  // Pitch Bend: two 7-bit bytes form a 14-bit value.
            if (message.size() >= 3) {
                const int lsb = message[1] & 0x7F;
                const int msb = message[2] & 0x7F;
                const int value14 = lsb | (msb << 7);
                return "Pitch Bend, channel=" + std::to_string(channel) +
                       ", value14=" + std::to_string(value14) +
                       " (center=8192, range=0..16383)";
            }
            break;

        default:
            break;
    }

    return "Unrecognized or incomplete MIDI message (status=" +
           std::to_string(static_cast<unsigned int>(status)) + ")";
}

void midiCallback(double deltaTime,
                  std::vector<unsigned char>* message,
                  void* userData) {
    if (message == nullptr || message->empty() || userData == nullptr) return;

    auto* context = static_cast<CallbackContext*>(userData);
    std::lock_guard<std::mutex> lock(context->outputMutex);

    std::cout << std::fixed << std::setprecision(6)
              << "t=" << monotonicSeconds() << "s"
              << "  delta=" << deltaTime << "s"
              << "  source=\"" << context->portName << "\"\n"
              << "  raw:     " << rawBytes(*message) << "\n"
              << "  decoded: " << describeMessage(*message) << "\n"
              << std::flush;
}

}  // namespace

int main() {
    try {
        // Construct the callback context before RtMidiIn so the context remains
        // alive until after the MIDI input object has been destroyed.
        CallbackContext context;
        RtMidiIn midiIn;

        const unsigned int portCount = midiIn.getPortCount();
        if (portCount == 0) {
            std::cerr << "No MIDI input ports found. Connect the device, make sure "
                         "its MIDI software is running, and restart this program.\n";
            return 1;
        }

        std::cout << "Available MIDI input ports:\n";
        for (unsigned int i = 0; i < portCount; ++i) {
            std::cout << "  [" << i << "] " << midiIn.getPortName(i) << '\n';
        }

        std::cout << "\nEnter the number of the MIDI input to monitor: ";
        std::string input;
        if (!std::getline(std::cin, input)) {
            std::cerr << "No selection received.\n";
            return 1;
        }

        std::size_t parsedChars = 0;
        unsigned long selected = 0;
        try {
            selected = std::stoul(input, &parsedChars);
        } catch (const std::exception&) {
            std::cerr << "Please enter a port number from the list.\n";
            return 1;
        }

        if (parsedChars != input.size() || selected >= portCount) {
            std::cerr << "Port number must be between 0 and " << (portCount - 1)
                      << ".\n";
            return 1;
        }

        context.portName = midiIn.getPortName(static_cast<unsigned int>(selected));

        // Include SysEx as well as normal MIDI messages. This diagnostic tool
        // also displays MIDI timing and active-sensing messages if the device
        // sends them, so output can be very busy on some setups.
        midiIn.ignoreTypes(false, false, false);
        midiIn.setCallback(&midiCallback, &context);
        midiIn.openPort(static_cast<unsigned int>(selected), "midi_probe");

        std::cout << "\nListening to: " << context.portName << '\n'
                  << "Timestamps use the monotonic clock (seconds since the "
                     "system's steady-clock epoch), like now_seconds() in "
                     "central_sender.cpp.\n"
                  << "Move/press the device to generate events. Press Ctrl+C to stop.\n\n"
                  << std::flush;

        std::signal(SIGINT, handleSignal);
        while (keepRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nStopping MIDI probe...\n";
        midiIn.cancelCallback();
        midiIn.closePort();
        return 0;
    } catch (const RtMidiError& error) {
        std::cerr << "RtMidi error: " << error.getMessage() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
