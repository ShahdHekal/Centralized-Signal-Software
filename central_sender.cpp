#pragma once
#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// Formats of different data streams written; EEG and EMG produce float32
// signals, midi sends raw bytes. TODO: Append differing formats for other 
// used modalities.

// https://github.com/axopy/pytrigno/blob/master/pytrigno.py
// https://github.com/unicorn-bi/Unicorn-Suite-Hybrid-Black/blob/master/Unicorn%20Linux%20C%20API/x64/Lib/unicorn.h
const uint8_t FORMAT_FLOAT32 = 1;
const uint8_t FORMAT_BYTES = 2;   


// Time for a packet timestamp in monotonic clock that uses the system booting as refrence since
// wall clocks drift / have longer delays. This is machine specific.
// To synchronize this between different machines, we'll likely do what LSL
// does and match it with a central server (have receiver machines calculate an
// offset from the central server.). This would help in syncing up bytes from 
// different streams.
inline double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Helper method that converts a given generic types 
// into raw bytes so that it can get sent over the network through the socket. 
template <typename T>
void put(std::vector<uint8_t>& buf, const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), p, p + sizeof(T));
}

// This sender's connection to the central server.
struct ServerLink {
    // defining headers
    std::string host, port;
    std::string name;          // such as "TrignoEMG" or "UnicornEEG"
    uint16_t channels;         // values sent per device in a block, for example,
                               // EEG sends per scan 17 values: the data from 
                               // its 8 electrodes, battery level, etc..
    double rate;               // rate of the sample in Hz
    uint8_t format;            // FORMAT_FLOAT32 or FORMAT_BYTES

    uint32_t seq = 0;          // counts every block, sent or not
    double next_try = 0;       // when to attempt the next reconnect if connection drops
    
    asio::io_context io;
    asio::ip::tcp::socket sock{io};

    // defining every packet on the wire as [u32 length][body] except for
    // the first packet.
    void write_packet(const std::vector<uint8_t>& body) {
        // load data into buffer
        std::vector<uint8_t> out;
        put(out, static_cast<uint32_t>(body.size()));
        out.insert(out.end(), body.begin(), body.end());

        // write to wire
        asio::write(sock, asio::buffer(out));
    }

    // defining first packet on every connection as a description of the
    // stream, with format [u16 name length][name][u16 channels][f64 rate][u8 format]

    // so the stream should be as follows [length][header] [length][data] [length][data], etc...
    void write_header() {
        std::vector<uint8_t> body;
        put(body, static_cast<uint16_t>(name.size()));
        body.insert(body.end(), name.begin(), name.end());
        put(body, channels);
        put(body, rate);
        put(body, format);
        write_packet(body);
    }

    // [data] : [u32 seq][f64 t0][signal data]

    // I am not really handelling the case of a dropped connection, it's more so
    // that if a connection drops, oh well we lost the data stream, but we can
    // certainly have it back up data in case connection drops so we don't lose
    // information if that's what we want? otherwise we can just keep it as is
    // and ensure nothing happens while testing.
    void send(double t0, const void* data, size_t bytes) {
        const uint32_t this_seq = seq++;
        try {
            if (!sock.is_open()) {
                // if this is the first message in the stream, send a header
                if (now_seconds() < next_try) return;
                
                asio::connect(sock, asio::ip::tcp::resolver(io).resolve(host, port));
                write_header();
                std::cerr << "connected to server\n";
            }

            // send a packet stream, we start with the identifying packet
            // sequence and timestamp converted to bytes in a buffer, to which
            // we append signal data --> [u32 seq][f64 t0][signal data]
            std::vector<uint8_t> body;
            put(body, this_seq);
            put(body, t0);
            const auto* p = static_cast<const uint8_t*>(data);
            body.insert(body.end(), p, p + bytes);

            // takes the size of the body buffer, converts it to bytes, appends
            // it in the beginning, then sends over the wire -->
            // [length][u32 seq][f64 t0][signal data] where [u32 seq][f64 t0][signal data]
            // is [data]
            write_packet(body);
            
        } catch (const std::exception& e) {
            std::cerr << "server: " << e.what() << "\n";
            asio::error_code ignored;
            sock.close(ignored);
            next_try = now_seconds() + 1.0;   // retry once a second, not on every block
        }
    }
};
