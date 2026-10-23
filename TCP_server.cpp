#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close closesocket
#else
#include <arpa/inet.h>   // inet_ntop, htons, ntohs
#include <netinet/in.h>  // sockaddr_in, INADDR_ANY
#include <sys/socket.h>  // socket, bind, recvfrom
#include <unistd.h>      // close
#endif

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <cstring>   // std::memcpy
#include <vector>    // std::vector, to hold a packet body

constexpr uint16_t kDefaultPort = 9125;

struct Config {
    uint16_t port = kDefaultPort;
    std::string out_dir = "sessions";  // where recordings will be written
};

// Largest packet body we will accept. A length too long might
// make us try to allocate gigabytes and crasjh our server. 
constexpr uint32_t kMaxBodyBytes = 1 << 20;

// Read exactly n bytes from a connected TCP socket into buf.
// TCP is a stream, so recv() can return fewer bytes than we asked for:
// data arrives in whatever pieces, but we keep running recv() until we have everything
// Returns true on success, false if the sender disconnected or recv failed.
bool read_exact(int fd, uint8_t* buf, size_t n) {
    // How many bytes we have so far.
    size_t got = 0;

    // Keep reading until we have all n bytes.
    while (got < n) {
        // Ask for the bytes still missing, written just after the ones we have.
        ssize_t r = recv(fd, reinterpret_cast<char*>(buf + got), static_cast<int>(n - got), 0);

        // 0 means the sender closed the connection cleanly.
        if (r == 0) return false;

        // Negative means an error, e.g. the connection was reset.
        if (r < 0) {
            perror("recv");
            return false;
        }

        // Count the bytes this call delivered.
        got += static_cast<size_t>(r);
    }

    // All n bytes are in buf.
    return true;
}

// Read one packet in the sender's format: [u32 length][body].
// On success, body holds exactly the body bytes and the function returns true.
// Returns false when the connection ends or the length makes no sense.
bool read_packet(int fd, std::vector<uint8_t>& body) {
    // First the 4-byte length.
    uint8_t len_bytes[4];
    if (!read_exact(fd, len_bytes, sizeof(len_bytes))) return false;

    // The sender writes numbers in the computer's own byte order
    uint32_t len;
    std::memcpy(&len, len_bytes, sizeof(len));

    // Reject impossible lengths instead of trusting them.
    if (len == 0 || len > kMaxBodyBytes) {
        std::fprintf(stderr, "bad packet length %u, closing connection\n", len);
        return false;
    }

    // Make room for the body, then read exactly that many bytes into it.
    body.resize(len);
    return read_exact(fd, body.data(), len);
}

// Program entry point. argc = number of command-line words (including the
// program name); argv = the words themselves as C strings.
int main(int argc, char* argv[]) {
    // Create the settings object with its defaults (port 9125, dir "sessions").
    Config cfg;
    
    #ifdef _WIN32
       WSADATA wsa;
       if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
           std::fprintf(stderr, "WSAStartup failed\n");
           return 1;
       }
    #endif

    // If a first argument was given, convert its text to a number and use it
    // as the port. The cast narrows int to the 16 bits a port number has.
    if (argc > 1) cfg.port = static_cast<uint16_t>(std::atoi(argv[1]));

    // If a second argument was given, use it as the output directory.
    if (argc > 2) cfg.out_dir = argv[2];

    int listen_sock = socket(AF_INET, SOCK_STREAM, 0); //swtiched to TCP from UDP

    // A negative return value means the call failed.
    if (listen_sock < 0) {
        perror("socket");
        return 1;
    }

    // Let the port be reused immediately after the server exits. 
    int yes = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(cfg.port);

    // Attach the socket to that address and port.
    if (bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(listen_sock);
        return 1;
    }

    // Turn it into a listening socket. 8 = how many connections the OS will
    // hold in a queue before we accept() them (we expect about 6 devices).
    if (listen(listen_sock, 8) < 0) {
        perror("listen");
        close(listen_sock);
        return 1;
    }

    // Tell the user the server is ready and on which port.
    std::printf("Listening for TCP on port %u\n", cfg.port);

    // Will be filled in by accept() with the sender's address and port.
    sockaddr_in client{};
    socklen_t client_len = sizeof(client);

    // Block until a sender connects. Returns a NEW socket for talking to that
    // one sender; listen_sock stays free to accept the next one.
    int conn = accept(listen_sock, reinterpret_cast<sockaddr*>(&client), &client_len);

    // A negative return value means the call failed.
    if (conn < 0) {
        perror("accept");
        close(listen_sock);
        return 1;
    }

    // Convert the sender's IP address to text, e.g. "127.0.0.1".
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));

    // Report who connected and which descriptor their socket got.
    std::printf("Connection from %s:%u (fd %d)\n", ip, ntohs(client.sin_port), conn);

    // Read packets from this sender until it disconnects.
    // body is reused for every packet; count is how many we have read.
    std::vector<uint8_t> body;
    unsigned long long count = 0;

    // read_packet returns false when the sender is done or something is wrong.
    while (read_packet(conn, body)) {
        // One more packet received.
        count++;

        // Report its number and body size.
        std::printf("packet %llu: %zu bytes\n", count, body.size());
    }

    // The loop ended, so this sender is gone.
    std::printf("Sender disconnected after %llu packets\n", count);

    // Close the connection socket, then the listening socket.
    close(conn);
    close(listen_sock);

    // Exit with 0, which signals success.
    return 0;
}
