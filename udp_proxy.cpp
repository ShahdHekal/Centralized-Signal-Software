#include "central_sender.cpp"

const std::string    SERVER_HOST  = "127.0.0.1";    // central receiver server
const std::string    SERVER_PORT  = "9125";
const unsigned short UDP_PORT     = 9127;           // Unicorn Suite's UDP output to this port
const std::string    STREAM_NAME  = "UnicornEEG";   
const double         RATE         = 250.0;          // Hz
const size_t         MAX_DATA     = 65536;          // bytes

int main() {
    asio::io_context io;
    asio::ip::udp::socket udp(io, asio::ip::udp::endpoint(asio::ip::udp::v4(), UDP_PORT));

    std::vector<uint8_t> scan(MAX_DATA);
    size_t n = udp.receive(asio::buffer(scan));   // channel count
    const uint16_t channels = uint16_t(n / sizeof(float));
    ServerLink link{SERVER_HOST, SERVER_PORT, STREAM_NAME, channels, RATE, FORMAT_FLOAT32};

    while (true) {
        link.send(now_seconds(), scan.data(), n);
        n = udp.receive(asio::buffer(scan));
    }
}
