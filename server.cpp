#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

const int PORT        = 6379;
const int BACKLOG     = 10;
const int BUFFER_SIZE = 4096;

int main() {

    // ── Create socket ───────────────────────────────────────────────────
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // ── Bind ────────────────────────────────────────────────────────────
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons(PORT);

    if (bind(server_fd, (sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed\n";
        return 1;
    }

    // ── Listen ──────────────────────────────────────────────────────────
    if (listen(server_fd, BACKLOG) < 0) {
        std::cerr << "Listen failed\n";
        return 1;
    }

    std::cout << "Mini-Redis listening on port " << PORT << "\n";

    // ── Outer loop: accept clients forever ──────────────────────────────
    while (true) {

        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            std::cerr << "Accept failed\n";
            continue;
        }

        std::cout << "\n[+] Client connected (fd=" << client_fd << ")\n";

        // ── Inner loop: keep reading from this client ───────────────────
        char buffer[BUFFER_SIZE];

        while (true) {
            memset(buffer, 0, sizeof(buffer));

            int bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

            if (bytes_read < 0) {
                std::cerr << "recv() error\n";
                break;
            }

            if (bytes_read == 0) {
                std::cout << "[-] Client disconnected (fd=" << client_fd << ")\n";
                break;
            }

            // ── Print raw bytes so we can see RESP ──────────────────────
            std::cout << "\n--- Received " << bytes_read << " bytes ---\n";
            std::cout << "Raw: ";
            for (int i = 0; i < bytes_read; i++) {
                if      (buffer[i] == '\r') std::cout << "\\r";
                else if (buffer[i] == '\n') std::cout << "\\n";
                else                        std::cout << buffer[i];
            }
            std::cout << "\n";

            // ── Always reply +PONG for now ──────────────────────────────
            const char* response = "+PONG\r\n";
            send(client_fd, response, strlen(response), 0);
            std::cout << "Sent: +PONG\\r\\n\n";
        }

        close(client_fd);
    }

    close(server_fd);
    return 0;
}