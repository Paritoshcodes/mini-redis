#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

const int PORT        = 6379;
const int BACKLOG     = 10;
const int BUFFER_SIZE = 4096;

// ── Type alias for brevity ───────────────────────────────────────────────────
// steady_clock::time_point is long to write — alias it
using TimePoint = std::chrono::steady_clock::time_point;

// ── The two stores ───────────────────────────────────────────────────────────
// store      : key → value  (the actual data)
// expiry_map : key → time_point (when it dies, if ever)
// Not every key has an expiry — only keys set with EX or PX
std::unordered_map<std::string, std::string> store;
std::unordered_map<std::string, TimePoint>   expiry_map;

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

// ── Expiry helpers ───────────────────────────────────────────────────────────

// Check if a key is expired RIGHT NOW
// Returns true if the key EXISTS in expiry_map AND its deadline has passed
bool is_expired(const std::string& key) {
    auto it = expiry_map.find(key);
    if (it == expiry_map.end()) return false;  // no expiry set → never expires
    return std::chrono::steady_clock::now() > it->second;
}

// Lazy cleanup — call this before any read operation on a key
// If expired: removes from both store and expiry_map, returns true
// If not expired: does nothing, returns false
bool check_and_expire(const std::string& key) {
    if (is_expired(key)) {
        store.erase(key);
        expiry_map.erase(key);
        std::cout << "  [EXPIRED] key \"" << key << "\" was lazily removed\n";
        return true;
    }
    return false;
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
    // Syntax: SET key value [EX seconds] [PX milliseconds]
    // After storing, scan remaining args for EX or PX
    if (name == "SET") {
        if (cmd.size() < 3) return resp_error("wrong number of arguments for 'set'");

        store[cmd[1]] = cmd[2];
        expiry_map.erase(cmd[1]);  // clear any old expiry first

        // Scan optional arguments after the value
        // cmd: [SET, key, value, EX, 10]
        //        0    1    2     3   4
        for (size_t i = 3; i + 1 < cmd.size(); i += 2) {
            std::string opt = cmd[i];
            std::transform(opt.begin(), opt.end(), opt.begin(), ::toupper);

            if (opt == "EX") {
                // EX = seconds
                int seconds = std::stoi(cmd[i + 1]);
                expiry_map[cmd[1]] = std::chrono::steady_clock::now()
                                   + std::chrono::seconds(seconds);
                std::cout << "  [SET] \"" << cmd[1] << "\" = \"" << cmd[2]
                          << "\" (expires in " << seconds << "s)\n";
                return resp_simple("OK");

            } else if (opt == "PX") {
                // PX = milliseconds
                int ms = std::stoi(cmd[i + 1]);
                expiry_map[cmd[1]] = std::chrono::steady_clock::now()
                                   + std::chrono::milliseconds(ms);
                std::cout << "  [SET] \"" << cmd[1] << "\" = \"" << cmd[2]
                          << "\" (expires in " << ms << "ms)\n";
                return resp_simple("OK");
            }
        }

        std::cout << "  [SET] \"" << cmd[1] << "\" = \"" << cmd[2]
                  << "\" (no expiry)\n";
        return resp_simple("OK");
    }

    // ── GET ─────────────────────────────────────────────────────────────
    // Check expiry BEFORE looking up the value — lazy expiry in action
    if (name == "GET") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'get'");

        // Lazy expiry check — if expired, clean it up and return nil
        if (check_and_expire(cmd[1])) return resp_null();

        auto it = store.find(cmd[1]);
        if (it == store.end()) {
            std::cout << "  [GET] \"" << cmd[1] << "\" → (nil)\n";
            return resp_null();
        }

        std::cout << "  [GET] \"" << cmd[1] << "\" → \"" << it->second << "\"\n";
        return resp_bulk(it->second);
    }

    // ── DEL ─────────────────────────────────────────────────────────────
    if (name == "DEL") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'del'");
        int deleted = 0;
        for (size_t i = 1; i < cmd.size(); i++) {
            deleted += store.erase(cmd[i]);
            expiry_map.erase(cmd[i]);  // always clean up expiry too
        }
        std::cout << "  [DEL] deleted " << deleted << " key(s)\n";
        return resp_integer(deleted);
    }

    // ── TTL ─────────────────────────────────────────────────────────────
    // TTL key → seconds remaining
    // Returns:
    //  -1 = key exists but has no expiry
    //  -2 = key does not exist (or expired)
    //  N  = seconds until expiry
    if (name == "TTL") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'ttl'");

        if (check_and_expire(cmd[1])) return resp_integer(-2);

        if (store.find(cmd[1]) == store.end()) return resp_integer(-2);

        auto exp_it = expiry_map.find(cmd[1]);
        if (exp_it == expiry_map.end()) return resp_integer(-1);  // no expiry

        auto now      = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(
                            exp_it->second - now);
        std::cout << "  [TTL] \"" << cmd[1] << "\" → "
                  << duration.count() << "s remaining\n";
        return resp_integer(duration.count());
    }

    // ── PTTL ────────────────────────────────────────────────────────────
    // Same as TTL but returns milliseconds
    if (name == "PTTL") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'pttl'");

        if (check_and_expire(cmd[1])) return resp_integer(-2);
        if (store.find(cmd[1]) == store.end()) return resp_integer(-2);

        auto exp_it = expiry_map.find(cmd[1]);
        if (exp_it == expiry_map.end()) return resp_integer(-1);

        auto now      = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            exp_it->second - now);
        std::cout << "  [PTTL] \"" << cmd[1] << "\" → "
                  << duration.count() << "ms remaining\n";
        return resp_integer(duration.count());
    }

    // ── PERSIST ─────────────────────────────────────────────────────────
    // PERSIST key → removes expiry from a key
    // Returns :1 if expiry was removed, :0 if key has no expiry
    if (name == "PERSIST") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'persist'");
        int removed = expiry_map.erase(cmd[1]);
        std::cout << "  [PERSIST] \"" << cmd[1] << "\" → "
                  << (removed ? "expiry removed" : "no expiry to remove") << "\n";
        return resp_integer(removed);
    }

    // ── EXISTS ──────────────────────────────────────────────────────────
    if (name == "EXISTS") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'exists'");
        if (check_and_expire(cmd[1])) return resp_integer(0);
        return resp_integer(store.count(cmd[1]));
    }

    // ── KEYS ────────────────────────────────────────────────────────────
    if (name == "KEYS") {
        std::string response = "*" + std::to_string(store.size()) + "\r\n";
        for (auto& pair : store) {
            response += resp_bulk(pair.first);
        }
        return response;
    }

    // ── DBSIZE ──────────────────────────────────────────────────────────
    if (name == "DBSIZE") {
        return resp_integer(store.size());
    }

    // ── FLUSHALL ────────────────────────────────────────────────────────
    if (name == "FLUSHALL") {
        store.clear();
        expiry_map.clear();  // clear expiry map too
        std::cout << "  [FLUSHALL] store wiped\n";
        return resp_simple("OK");
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
            int bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
            if (bytes_read < 0) { std::cerr << "recv() error\n"; break; }
            if (bytes_read == 0) { std::cout << "[-] Client disconnected\n"; break; }

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