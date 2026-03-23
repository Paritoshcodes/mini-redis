#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

const int PORT        = 6379;
const int BACKLOG     = 10;
const int BUFFER_SIZE = 4096;

// ── In-memory key-value store ────────────────────────────────────────────────
// This IS the database. One global map, lives in RAM for the lifetime
// of the server process. When the server stops, all data is gone.
// Real Redis has persistence (RDB/AOF) to survive restarts — we don't, yet.
std::unordered_map<std::string, std::string> store;

// ── RESP response builders ───────────────────────────────────────────────────

std::string resp_simple(const std::string& msg) {
    return "+" + msg + "\r\n";
}

std::string resp_error(const std::string& msg) {
    return "-ERR " + msg + "\r\n";
}

std::string resp_bulk(const std::string& msg) {
    return "$" + std::to_string(msg.size()) + "\r\n" + msg + "\r\n";
}

std::string resp_null() {
    return "$-1\r\n";
}

std::string resp_integer(int n) {
    return ":" + std::to_string(n) + "\r\n";
}

// ── RESP Parser ──────────────────────────────────────────────────────────────

std::string read_line(const std::string& buf, size_t& pos) {
    size_t end = buf.find("\r\n", pos);
    if (end == std::string::npos) return "";
    std::string line = buf.substr(pos, end - pos);
    pos = end + 2;
    return line;
}

std::vector<std::string> parse_resp(const std::string& buf) {
    std::vector<std::string> result;
    size_t pos = 0;

    std::string first_line = read_line(buf, pos);
    if (first_line.empty() || first_line[0] != '*') return result;

    int num_elements = std::stoi(first_line.substr(1));

    for (int i = 0; i < num_elements; i++) {
        std::string len_line = read_line(buf, pos);
        if (len_line.empty() || len_line[0] != '$') return result;

        int str_len = std::stoi(len_line.substr(1));
        if (pos + str_len > buf.size()) return result;

        std::string value = buf.substr(pos, str_len);
        pos += str_len + 2;
        result.push_back(value);
    }

    return result;
}

// ── Command dispatcher ───────────────────────────────────────────────────────

std::string handle_command(const std::vector<std::string>& cmd) {

    if (cmd.empty()) return resp_error("empty command");

    std::string name = cmd[0];
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);

    // ── PING ────────────────────────────────────────────────────────────
    if (name == "PING") {
        return cmd.size() == 1 ? resp_simple("PONG") : resp_bulk(cmd[1]);
    }

    // ── ECHO ────────────────────────────────────────────────────────────
    if (name == "ECHO") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'echo'");
        return resp_bulk(cmd[1]);
    }

    // ── SET ─────────────────────────────────────────────────────────────
    // SET key value
    // Stores key→value in the map. Overwrites silently if key exists.
    if (name == "SET") {
        if (cmd.size() < 3) return resp_error("wrong number of arguments for 'set'");

        store[cmd[1]] = cmd[2];   // ← THE actual storage

        std::cout << "  [SET] \"" << cmd[1] << "\" = \"" << cmd[2] << "\""
                  << " (store size: " << store.size() << ")\n";
        return resp_simple("OK");
    }

    // ── GET ─────────────────────────────────────────────────────────────
    // GET key
    // Looks up key in the map.
    // Returns the value as a bulk string, or $-1 (null) if not found.
    if (name == "GET") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'get'");

        auto it = store.find(cmd[1]);  // iterator — points to the key-value pair
                                        // or store.end() if not found

        if (it == store.end()) {
            std::cout << "  [GET] \"" << cmd[1] << "\" → (nil)\n";
            return resp_null();         // key doesn't exist
        }

        std::cout << "  [GET] \"" << cmd[1] << "\" → \"" << it->second << "\"\n";
        return resp_bulk(it->second);   // it->second is the value
    }

    // ── DEL ─────────────────────────────────────────────────────────────
    // DEL key [key2 key3 ...]
    // Deletes one or more keys. Returns how many were actually deleted.
    // Real Redis DEL accepts multiple keys — we support that too.
    if (name == "DEL") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'del'");

        int deleted = 0;
        for (size_t i = 1; i < cmd.size(); i++) {
            deleted += store.erase(cmd[i]);  // erase() returns 1 if deleted, 0 if not found
        }

        std::cout << "  [DEL] deleted " << deleted << " key(s)\n";
        return resp_integer(deleted);
    }

    // ── EXISTS ──────────────────────────────────────────────────────────
    // EXISTS key
    // Returns :1 if key exists, :0 if not.
    // Bonus command — not in our original plan but trivial to add now.
    if (name == "EXISTS") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'exists'");
        int exists = store.count(cmd[1]);  // count() returns 1 or 0 for unordered_map
        std::cout << "  [EXISTS] \"" << cmd[1] << "\" → " << exists << "\n";
        return resp_integer(exists);
    }

    // ── KEYS ────────────────────────────────────────────────────────────
    // KEYS *
    // Returns all keys currently in the store as a RESP array.
    // Real Redis supports pattern matching — we just support * (all keys) for now.
    if (name == "KEYS") {
        // Build a RESP array manually
        // *<count>\r\n then each key as a bulk string
        std::string response = "*" + std::to_string(store.size()) + "\r\n";
        for (auto& pair : store) {
            response += resp_bulk(pair.first);
        }
        std::cout << "  [KEYS] returning " << store.size() << " key(s)\n";
        return response;
    }

    // ── FLUSHALL ────────────────────────────────────────────────────────
    // FLUSHALL
    // Wipes the entire store. Useful for testing.
    if (name == "FLUSHALL") {
        store.clear();
        std::cout << "  [FLUSHALL] store wiped\n";
        return resp_simple("OK");
    }

    // ── DBSIZE ──────────────────────────────────────────────────────────
    // DBSIZE
    // Returns how many keys are in the store.
    if (name == "DBSIZE") {
        return resp_integer(store.size());
    }

    return resp_error("unknown command '" + cmd[0] + "'");
}

// ── Main ─────────────────────────────────────────────────────────────────────

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
        int client_fd = accept(server_fd,
                               (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) { std::cerr << "accept() failed\n"; continue; }

        std::cout << "\n[+] Client connected (fd=" << client_fd << ")\n";

        char buffer[BUFFER_SIZE];

        while (true) {
            memset(buffer, 0, sizeof(buffer));
            int bytes_read = recv(client_fd, buffer,
                                  sizeof(buffer) - 1, 0);

            if (bytes_read < 0) { std::cerr << "recv() error\n"; break; }
            if (bytes_read == 0) {
                std::cout << "[-] Client disconnected\n";
                break;
            }

            std::string raw(buffer, bytes_read);
            std::vector<std::string> cmd = parse_resp(raw);
            std::string response = handle_command(cmd);
            send(client_fd, response.c_str(), response.size(), 0);
        }

        close(client_fd);
    }

    close(server_fd);
    return 0;
}