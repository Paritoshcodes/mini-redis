#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

const int PORT        = 6379;
const int BACKLOG     = 10;
const int BUFFER_SIZE = 4096;

// ── RESP Parser ─────────────────────────────────────────────────────────────
//
// Takes the raw buffer received from the client and parses it into a vector
// of strings representing the command and its arguments.
//
// Example input : "*3\r\n$3\r\nset\r\n$4\r\nname\r\n$8\r\nparitosh\r\n"
// Example output: {"set", "name", "paritosh"}
//
// 'pos' is our cursor — it tracks where we are in the buffer as we parse.
// We pass it by reference so each helper function advances it for the next.

// ── Read one line (up to \r\n), return it without the \r\n ─────────────────
std::string read_line(const std::string& buf, size_t& pos) {
    size_t end = buf.find("\r\n", pos);
    if (end == std::string::npos) return "";   // malformed — no \r\n found
    std::string line = buf.substr(pos, end - pos);
    pos = end + 2;                             // advance past the \r\n
    return line;
}

// ── Parse the full RESP array into a vector of strings ─────────────────────
std::vector<std::string> parse_resp(const std::string& buf) {
    std::vector<std::string> result;
    size_t pos = 0;

    // Step 1: Read the first line — must start with '*'
    std::string first_line = read_line(buf, pos);
    if (first_line.empty() || first_line[0] != '*') {
        // Not a RESP array — could be inline command, ignore for now
        return result;
    }

    // The number after '*' tells us how many elements to expect
    int num_elements = std::stoi(first_line.substr(1));

    // Step 2: Read each element
    for (int i = 0; i < num_elements; i++) {

        // Each element starts with a '$' line giving the byte length
        std::string len_line = read_line(buf, pos);
        if (len_line.empty() || len_line[0] != '$') {
            std::cerr << "Expected '$' line, got: " << len_line << "\n";
            return result;
        }

        int str_len = std::stoi(len_line.substr(1));

        // Read exactly str_len bytes as the value
        if (pos + str_len > buf.size()) {
            std::cerr << "Buffer too short for declared length\n";
            return result;
        }

        std::string value = buf.substr(pos, str_len);
        pos += str_len + 2;   // +2 to skip the trailing \r\n after the value

        result.push_back(value);
    }

    return result;
}

// ── Main ────────────────────────────────────────────────────────────────────
int main() {

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { std::cerr << "socket() failed\n"; return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons(PORT);

    if (bind(server_fd, (sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "bind() failed\n"; return 1;
    }

    if (listen(server_fd, BACKLOG) < 0) {
        std::cerr << "listen() failed\n"; return 1;
    }

    std::cout << "Mini-Redis listening on port " << PORT << "\n";

    while (true) {

        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) { std::cerr << "accept() failed\n"; continue; }

        std::cout << "\n[+] Client connected (fd=" << client_fd << ")\n";

        char buffer[BUFFER_SIZE];

        while (true) {
            memset(buffer, 0, sizeof(buffer));
            int bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

            if (bytes_read < 0) { std::cerr << "recv() error\n"; break; }
            if (bytes_read == 0) {
                std::cout << "[-] Client disconnected\n";
                break;
            }

            // ── Parse the incoming RESP data ────────────────────────────
            std::string raw(buffer, bytes_read);
            std::vector<std::string> cmd = parse_resp(raw);

            // ── Print what we parsed ────────────────────────────────────
            std::cout << "\nParsed command (" << cmd.size() << " parts):\n";
            for (size_t i = 0; i < cmd.size(); i++) {
                std::cout << "  [" << i << "] \"" << cmd[i] << "\"\n";
            }

            // ── Reply +PONG for everything still ────────────────────────
            // Part 4 will route these to real command handlers
            const char* response = "+PONG\r\n";
            send(client_fd, response, strlen(response), 0);
        }

        close(client_fd);
    }

    close(server_fd);
    return 0;
}