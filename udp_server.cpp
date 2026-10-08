#include <arpa/inet.h>   // inet_ntop, htons, ntohs
#include <netinet/in.h>  // sockaddr_in, INADDR_ANY
#include <sys/socket.h>  // socket, bind, recvfrom
#include <unistd.h>      // close

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

constexpr uint16_t kDefaultPort = 9125;

struct Config {
    uint16_t port = kDefaultPort;
    std::string out_dir = "sessions";  // where recordings will be written
};

// Program entry point. argc = number of command-line words (including the
// program name); argv = the words themselves as C strings.
int main(int argc, char* argv[]) {
    // Create the settings object with its defaults (port 9125, dir "sessions").
    Config cfg;

    // If a first argument was given, convert its text to a number and use it
    // as the port. The cast narrows int to the 16 bits a port number has.
    if (argc > 1) cfg.port = static_cast<uint16_t>(std::atoi(argv[1]));

    // If a second argument was given, use it as the output directory.
    if (argc > 2) cfg.out_dir = argv[2];

    // Creating new socket.
    // Returns a file descriptor.
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    // check if socket creation sucess
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    // TEMPORARY: show the descriptor number and the port we plan to use. testing purposes
    std::printf("Socket created (fd %d), port will be %u\n", sock, cfg.port);

    close(sock);
    
    // Exit with 0, which signals success.
    return 0;
}