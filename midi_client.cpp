// midi_client.cpp
// Receives MIDI from the ROLI Seaboard and Airwave,
// then sends the events to the central TCP server.

#include <RtMidi.h>
#include "central_sender.cpp"

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t keepRunning = 1;

void handleSignal(int) {
    keepRunning = 0;
}

// One MIDI event waiting to be sent.
struct MidiEvent {
    uint8_t deviceId;
    double timestamp;
    std::vector<uint8_t> midiBytes;
};

// Shared queue for events from both devices.
struct EventQueue {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<MidiEvent> events;
};

// Each MIDI input has its own context.
struct CallbackContext {
    uint8_t deviceId;
    EventQueue* queue;
};

// Called whenever either device sends a MIDI message.
// Keep this callback short: queue the event instead of
// performing network operations inside the callback.
void midiCallback(double /*deltaTime*/,
                  std::vector<unsigned char>* message,
                  void* userData) {

    if (message == nullptr || message->empty() || userData == nullptr) {
        return;
    }

    auto* context = static_cast<CallbackContext*>(userData);

    MidiEvent event;
    event.deviceId = context->deviceId;
    event.timestamp = now_seconds();
    event.midiBytes.assign(message->begin(), message->end());

    {
        std::lock_guard<std::mutex> lock(context->queue->mutex);
        context->queue->events.push_back(std::move(event));
    }

    context->queue->ready.notify_one();
}

// Ask the user to select a MIDI input port.
bool readPortNumber(const std::string& prompt,
                    unsigned int portCount,
                    unsigned int& selectedPort) {

    std::cout << prompt;

    std::string input;
    if (!std::getline(std::cin, input)) {
        return false;
    }

    try {
        std::size_t charsRead = 0;
        unsigned long value = std::stoul(input, &charsRead);

        if (charsRead != input.size() || value >= portCount) {
            std::cerr << "Invalid MIDI port number.\n";
            return false;
        }

        selectedPort = static_cast<unsigned int>(value);
        return true;

    } catch (const std::exception&) {
        std::cerr << "Invalid input.\n";
        return false;
    }
}

} // namespace

int main(int argc, char* argv[]) {

    // Get the Windows server IP and port from command-line arguments.
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: ./midi_client <server-ip> [port]\n";
        return 1;
    }

    const std::string serverHost = argv[1];
    const std::string serverPort = (argc == 3) ? argv[2] : "9125";

    try {

        // Find available MIDI inputs.
        RtMidiIn portEnumerator;
        unsigned int portCount = portEnumerator.getPortCount();

        if (portCount == 0) {
            std::cerr << "No MIDI input ports found.\n";
            return 1;
        }

        std::cout << "Available MIDI input ports:\n";

        for (unsigned int i = 0; i < portCount; ++i) {
            std::cout << "[" << i << "] "
                      << portEnumerator.getPortName(i) << "\n";
        }

        // Select the two devices separately.
        unsigned int seaboardPort = 0;
        unsigned int airwavePort = 0;

        if (!readPortNumber(
                "Enter the ROLI Seaboard port number: ",
                portCount, seaboardPort)) {
            return 1;
        }

        if (!readPortNumber(
                "Enter the ROLI Airwave Expression port number: ",
                portCount, airwavePort)) {
            return 1;
        }

        if (seaboardPort == airwavePort) {
            std::cerr << "Choose two different MIDI ports.\n";
            return 1;
        }

        // Create ONE shared TCP connection for both devices.
        //
        // FORMAT_BYTES means the payload contains raw bytes.
        // This is an event-driven stream, so rate is 0.0.
        ServerLink link{
            serverHost,
            serverPort,
            "ROLIMIDI",
            1,
            0.0,
            FORMAT_BYTES
        };

        std::cout << "\nSending MIDI to "
                  << serverHost << ":" << serverPort << "\n";

        // Device IDs:
        // 1 = Seaboard
        // 2 = Airwave Expression
        EventQueue queue;

        CallbackContext seaboardContext{1, &queue};
        CallbackContext airwaveContext{2, &queue};

        // Create two independent MIDI input connections.
        RtMidiIn seaboardIn;
        RtMidiIn airwaveIn;

        // Receive every MIDI message type.
        seaboardIn.ignoreTypes(false, false, false);
        airwaveIn.ignoreTypes(false, false, false);

        // Register callbacks.
        seaboardIn.setCallback(
            &midiCallback, &seaboardContext
        );

        airwaveIn.setCallback(
            &midiCallback, &airwaveContext
        );

        // Open both devices.
        seaboardIn.openPort(
            seaboardPort, "midi_client_seaboard"
        );

        std::cout << "Opened Seaboard: "
                  << portEnumerator.getPortName(seaboardPort)
                  << "\n";

        airwaveIn.openPort(
            airwavePort, "midi_client_airwave"
        );

        std::cout << "Opened Airwave: "
                  << portEnumerator.getPortName(airwavePort)
                  << "\n";

        std::signal(SIGINT, handleSignal);

        std::cout << "\nStreaming MIDI events.\n";
        std::cout << "Press Ctrl+C to stop.\n\n";

        unsigned long long totalEvents = 0;
        unsigned long long eventsThisSecond = 0;

        double nextReport = now_seconds() + 1.0;

        // Main thread handles all network transmission.
        // MIDI callbacks only add events to the queue.
        while (keepRunning) {

            MidiEvent event;
            bool haveEvent = false;

            {
                std::unique_lock<std::mutex> lock(queue.mutex);

                queue.ready.wait_for(
                    lock,
                    std::chrono::milliseconds(100),
                    [&queue]() {
                        return !queue.events.empty() || !keepRunning;
                    }
                );

                if (!queue.events.empty()) {
                    event = std::move(queue.events.front());
                    queue.events.pop_front();
                    haveEvent = true;
                }
            }

            if (haveEvent) {

                // Payload format:
                //
                // [1 byte device ID][original MIDI bytes]
                //
                // 1 = Seaboard
                // 2 = Airwave

                std::vector<uint8_t> payload;

                payload.reserve(1 + event.midiBytes.size());

                payload.push_back(event.deviceId);

                payload.insert(
                    payload.end(),
                    event.midiBytes.begin(),
                    event.midiBytes.end()
                );

                // ServerLink adds the sequence number,
                // timestamp, and TCP packet framing.
                link.send(
                    event.timestamp,
                    payload.data(),
                    payload.size()
                );

                ++totalEvents;
                ++eventsThisSecond;
            }

            // Print a status update once per second.
            double now = now_seconds();

            if (now >= nextReport) {

                std::cerr
                    << "MIDI events processed: "
                    << eventsThisSecond
                    << "/sec | Total: "
                    << totalEvents
                    << " | Next sequence: "
                    << link.seq
                    << "\n";

                eventsThisSecond = 0;
                nextReport = now + 1.0;
            }
        }

        // Stop both MIDI inputs before exiting.
        seaboardIn.cancelCallback();
        airwaveIn.cancelCallback();

        if (seaboardIn.isPortOpen()) {
            seaboardIn.closePort();
        }

        if (airwaveIn.isPortOpen()) {
            airwaveIn.closePort();
        }

        std::cout << "\nStopping MIDI client.\n";
        std::cout << "Total events processed: "
                  << totalEvents << "\n";

        return 0;

    } catch (const RtMidiError& error) {

        std::cerr << "RtMidi error: "
                  << error.getMessage() << "\n";

        return 1;

    } catch (const std::exception& error) {

        std::cerr << "Error: "
                  << error.what() << "\n";

        return 1;
    }
}
