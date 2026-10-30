#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>   // inet_ntop, htons, ntohs
#include <netinet/in.h>  // sockaddr_in, INADDR_ANY
#include <sys/socket.h>  // socket, bind, recvfrom
#include <unistd.h>      // close
#include <csignal>
#endif

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <cstring>   // std::memcpy
#include <vector>    // std::vector, to hold a packet body

#ifdef _WIN32
inline void close_socket(int s) { closesocket(s); }
#else
inline void close_socket(int s) { close(s); }
#endif

constexpr uint16_t kDefaultPort = 9125;
constexpr uint16_t kViewerPort  = 9126;

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

// Viewers: connect to kViewerPort, send a stream name ("UnicornEEG\n"),
// then receive that stream's header and every packet after it.
struct Viewer { int fd; std::string wants; };
std::mutex mtx;                                       // lock to protct viewerc thread
std::vector<Viewer> viewers;
std::map<std::string, std::vector<uint8_t>> headers;  

// Send one packet, [u32 length][body]. Returns false if the other side drops the connection.
bool send_packet(int fd, const std::vector<uint8_t>& body) {
    uint32_t len = static_cast<uint32_t>(body.size());
    return send(fd, reinterpret_cast<const char*>(&len), 4, 0) == 4
        && send(fd, reinterpret_cast<const char*>(body.data()), static_cast<int>(len), 0) == static_cast<int>(len);
}

// Pass a stream's packet to every viewer that asked for it, drops dead viewers.
void forward(const std::string& name, const std::vector<uint8_t>& body) {
    std::lock_guard<std::mutex> lock(mtx);
    std::erase_if(viewers, [&](const Viewer& v) {
        if (v.wants != name || send_packet(v.fd, body)) return false;
        close(v.fd);
        return true;
    });
}

// Runs on its own thread: accepts viewers forever.
void viewer_acceptor(int viewer_sock) {
    while (true) {
        int fd = accept(viewer_sock, nullptr, nullptr);
        std::string wants;
        uint8_t c;
        while (read_exact(fd, &c, 1) && c != '\n') wants += char(c);

        std::lock_guard<std::mutex> lock(mtx);
        if (headers.count(wants)) send_packet(fd, headers[wants]);   // stream already running so send headercfirst
        viewers.push_back({fd, wants});
        std::printf("viewer connected, wants %s\n", wants.c_str());
    }
}

// Handles one sender from connect to disconnect. Runs on its own thread.
void handle_stream(int conn, sockaddr_in client) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));

    // First packet is the header; its name is [u16 length][characters].
    std::vector<uint8_t> body;
    read_packet(conn, body);
    uint16_t name_len;
    std::memcpy(&name_len, body.data(), 2);
    std::string name(reinterpret_cast<char*>(body.data()) + 2, name_len);
    std::printf("[%s] connected from %s\n", name.c_str(), ip);
    {
        std::lock_guard<std::mutex> lock(mtx);
        headers[name] = body;
    }
    forward(name, body);

    // Every later packet is data. One status line per second.
    unsigned long long count = 0;
    auto next = std::chrono::steady_clock::now();
    while (read_packet(conn, body)) {
        forward(name, body);
        if (++count, std::chrono::steady_clock::now() >= next) {
            std::printf("[%s] %llu packets\n", name.c_str(), count);
            next += std::chrono::seconds(1);
        }
    }
    std::printf("[%s] disconnected\n", name.c_str());
    close(conn);
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
    #else
       std::signal(SIGPIPE, SIG_IGN);
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
        close_socket(listen_sock);
        return 1;
    }

    // Turn it into a listening socket. 8 = how many connections the OS will
    // hold in a queue before we accept() them (we expect about 6 devices).
    if (listen(listen_sock, 8) < 0) {
        perror("listen");
        close_socket(listen_sock);
        return 1;
    }

    setvbuf(stdout, nullptr, _IONBF, 0);   // print each line immediately
    std::printf("Listening for clients on port %u, viewers on %u\n", cfg.port, kViewerPort);

    // Second listening socket, for viewers.
    int viewer_sock = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(viewer_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    addr.sin_port = htons(kViewerPort);
    bind(viewer_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(viewer_sock, 8);
    std::thread(viewer_acceptor, viewer_sock).detach();

    // Accept senders forever, one thread each.
    while (true) {
        sockaddr_in client{};
        socklen_t client_len = sizeof(client);
        int conn = accept(listen_sock, reinterpret_cast<sockaddr*>(&client), &client_len);
        std::thread(handle_stream, conn, client).detach();
    }
}
