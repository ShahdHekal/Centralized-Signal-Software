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
#include <algorithm>
#include <memory>
#include <sstream>

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
        // Create an object to list available MIDI devices.
        RtMidiIn portEnumerator;

        const unsigned int portCount = portEnumerator.getPortCount();

        if (portCount == 0) {
            std::cerr << "No MIDI input ports found.\n";
            return 1;
        }

        // Display all available MIDI input ports.
        std::cout << "Available MIDI input ports:\n";

        for (unsigned int i = 0; i < portCount; ++i) {
            std::cout << "  [" << i << "] "
                      << portEnumerator.getPortName(i) << '\n';
        }

        // Ask the user which devices to monitor.
        std::cout << "\nEnter the port numbers to monitor, separated by commas "
                     "(example: 0, 1): ";

        std::string input;
        std::getline(std::cin, input);

        // Parse the selected port numbers.
        std::vector<unsigned int> selectedPorts;
        std::stringstream selections(input);
        std::string token;

        while (std::getline(selections, token, ',')) {
            const std::size_t first = token.find_first_not_of(" \t");

            if (first == std::string::npos) {
                std::cerr << "Invalid selection.\n";
                return 1;
            }

            const std::size_t last = token.find_last_not_of(" \t");
            token = token.substr(first, last - first + 1);

            std::size_t parsedChars = 0;
            unsigned long selected;

            try {
                selected = std::stoul(token, &parsedChars);
            } catch (const std::exception&) {
                std::cerr << "Invalid port number: " << token << '\n';
                return 1;
            }

            if (parsedChars != token.size() || selected >= portCount) {
                std::cerr << "Port number out of range.\n";
                return 1;
            }

            unsigned int port = static_cast<unsigned int>(selected);

            // Prevent selecting the same device twice.
            if (std::find(selectedPorts.begin(),
                          selectedPorts.end(), port) != selectedPorts.end()) {
                std::cerr << "Port selected more than once.\n";
                return 1;
            }

            selectedPorts.push_back(port);
        }

        // Create separate MIDI input objects for each device.
        std::vector<std::unique_ptr<CallbackContext>> contexts;
        std::vector<std::unique_ptr<RtMidiIn>> midiInputs;

        std::signal(SIGINT, handleSignal);

        for (unsigned int port : selectedPorts) {

            // Create a separate context for each device.
            auto context = std::make_unique<CallbackContext>();
            context->portName = portEnumerator.getPortName(port);

            // Create a separate MIDI input for each device.
            auto midiIn = std::make_unique<RtMidiIn>();

            // Receive all MIDI message types.
            midiIn->ignoreTypes(false, false, false);

            // Assign the MIDI callback.
            midiIn->setCallback(&midiCallback, context.get());

            try {
                midiIn->openPort(port, "midi_probe");

                std::cout << "Listening to: "
                          << context->portName << '\n';

                contexts.push_back(std::move(context));
                midiInputs.push_back(std::move(midiIn));

            } catch (const RtMidiError& error) {
                midiIn->cancelCallback();

                std::cerr << "Could not open "
                          << context->portName << ": "
                          << error.getMessage() << '\n';
            }
        }

        if (midiInputs.empty()) {
            std::cerr << "No MIDI inputs could be opened.\n";
            return 1;
        }

        std::cout << "\nMonitoring "
                  << midiInputs.size()
                  << " MIDI input(s).\n";

        std::cout << "Move/press the devices to generate events. "
                     "Press Ctrl+C to stop.\n\n";

        // Keep both devices running simultaneously.
        while (keepRunning) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(100)
            );
        }

        // Shut down both MIDI inputs.
        std::cout << "\nStopping MIDI probe...\n";

        for (auto& midiIn : midiInputs) {
            midiIn->cancelCallback();

            if (midiIn->isPortOpen()) {
                midiIn->closePort();
            }
        }

        return 0;

    } catch (const RtMidiError& error) {
        std::cerr << "RtMidi error: "
                  << error.getMessage() << '\n';
        return 1;

    } catch (const std::exception& error) {
        std::cerr << "Error: "
                  << error.what() << '\n';
        return 1;
    }
}

