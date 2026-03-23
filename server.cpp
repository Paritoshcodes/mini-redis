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
#include <fcntl.h>         // fcntl() for non-blocking mode
#include <sys/epoll.h>     // epoll_create1, epoll_ctl, epoll_wait

const int PORT        = 6379;
const int BACKLOG     = 10;
const int BUFFER_SIZE = 4096;
const int MAX_EVENTS  = 64;   // max events epoll returns per wait call

// ── Type alias ───────────────────────────────────────────────────────────────
using TimePoint = std::chrono::steady_clock::time_point;

// ── Stores ───────────────────────────────────────────────────────────────────
std::unordered_map<std::string, std::string> store;
std::unordered_map<std::string, TimePoint>   expiry_map;

// ── Helper: set a file descriptor to non-blocking mode ───────────────────────
// In blocking mode: recv() waits forever if no data arrives
// In non-blocking mode: recv() returns immediately with EAGAIN if no data
// epoll tells us when data IS ready so we never block anyway —
// but setting non-blocking is defensive best practice
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);  // get current flags
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);  // add non-blocking flag
}

// ── RESP response builders ───────────────────────────────────────────────────
std::string resp_simple(const std::string& msg) { return "+" + msg + "\r\n"; }
std::string resp_error(const std::string& msg)  { return "-ERR " + msg + "\r\n"; }
std::string resp_null()                          { return "$-1\r\n"; }
std::string resp_integer(int n)                  { return ":" + std::to_string(n) + "\r\n"; }
std::string resp_bulk(const std::string& msg) {
    return "$" + std::to_string(msg.size()) + "\r\n" + msg + "\r\n";
}

// ── Expiry helpers ───────────────────────────────────────────────────────────
bool is_expired(const std::string& key) {
    auto it = expiry_map.find(key);
    if (it == expiry_map.end()) return false;
    return std::chrono::steady_clock::now() > it->second;
}

bool check_and_expire(const std::string& key) {
    if (is_expired(key)) {
        store.erase(key);
        expiry_map.erase(key);
        std::cout << "  [EXPIRED] \"" << key << "\" lazily removed\n";
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

    if (name == "PING") {
        return cmd.size() == 1 ? resp_simple("PONG") : resp_bulk(cmd[1]);
    }
    if (name == "ECHO") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'echo'");
        return resp_bulk(cmd[1]);
    }
    if (name == "SET") {
        if (cmd.size() < 3) return resp_error("wrong number of arguments for 'set'");
        store[cmd[1]] = cmd[2];
        expiry_map.erase(cmd[1]);
        for (size_t i = 3; i + 1 < cmd.size(); i += 2) {
            std::string opt = cmd[i];
            std::transform(opt.begin(), opt.end(), opt.begin(), ::toupper);
            if (opt == "EX") {
                expiry_map[cmd[1]] = std::chrono::steady_clock::now()
                                   + std::chrono::seconds(std::stoi(cmd[i+1]));
            } else if (opt == "PX") {
                expiry_map[cmd[1]] = std::chrono::steady_clock::now()
                                   + std::chrono::milliseconds(std::stoi(cmd[i+1]));
            }
        }
        return resp_simple("OK");
    }
    if (name == "GET") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'get'");
        if (check_and_expire(cmd[1])) return resp_null();
        auto it = store.find(cmd[1]);
        if (it == store.end()) return resp_null();
        return resp_bulk(it->second);
    }
    if (name == "DEL") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'del'");
        int deleted = 0;
        for (size_t i = 1; i < cmd.size(); i++) {
            deleted += store.erase(cmd[i]);
            expiry_map.erase(cmd[i]);
        }
        return resp_integer(deleted);
    }
    if (name == "TTL") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'ttl'");
        if (check_and_expire(cmd[1])) return resp_integer(-2);
        if (!store.count(cmd[1]))     return resp_integer(-2);
        auto it = expiry_map.find(cmd[1]);
        if (it == expiry_map.end())   return resp_integer(-1);
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                        it->second - std::chrono::steady_clock::now());
        return resp_integer(secs.count());
    }
    if (name == "PTTL") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'pttl'");
        if (check_and_expire(cmd[1])) return resp_integer(-2);
        if (!store.count(cmd[1]))     return resp_integer(-2);
        auto it = expiry_map.find(cmd[1]);
        if (it == expiry_map.end())   return resp_integer(-1);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      it->second - std::chrono::steady_clock::now());
        return resp_integer(ms.count());
    }
    if (name == "PERSIST") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'persist'");
        return resp_integer(expiry_map.erase(cmd[1]));
    }
    if (name == "EXISTS") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'exists'");
        if (check_and_expire(cmd[1])) return resp_integer(0);
        return resp_integer(store.count(cmd[1]));
    }
    if (name == "DBSIZE") {
        return resp_integer(store.size());
    }
    if (name == "FLUSHALL") {
        store.clear();
        expiry_map.clear();
        return resp_simple("OK");
    }
    if (name == "KEYS") {
        std::string response = "*" + std::to_string(store.size()) + "\r\n";
        for (auto& pair : store) response += resp_bulk(pair.first);
        return response;
    }
    return resp_error("unknown command '" + cmd[0] + "'");
}

// ── Main — epoll event loop ──────────────────────────────────────────────────
int main() {

    // ── Create and configure server socket ──────────────────────────────
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { std::cerr << "socket() failed\n"; return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblocking(server_fd);

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

    // ── Create epoll instance ────────────────────────────────────────────
    // epoll_create1(0) creates an epoll fd
    // Think of it as a "subscription list" of fds to watch
    int epfd = epoll_create1(0);
    if (epfd < 0) { std::cerr << "epoll_create1() failed\n"; return 1; }

    // ── Register server_fd with epoll ────────────────────────────────────
    // EPOLLIN = notify me when this fd has data to read
    // For server_fd, "data to read" means a new client is connecting
    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

    std::cout << "Mini-Redis listening on port " << PORT
              << " (epoll, concurrent clients)\n";

    // ── Event loop ───────────────────────────────────────────────────────
    epoll_event events[MAX_EVENTS];

    while (true) {

        // epoll_wait blocks until at least one fd is ready
        // -1 = wait forever (no timeout)
        // Returns number of ready fds
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) { std::cerr << "epoll_wait() failed\n"; break; }

        // Process each ready fd
        for (int i = 0; i < n; i++) {
            int ready_fd = events[i].data.fd;

            // ── Case 1: server_fd is ready → new client connecting ───────
            if (ready_fd == server_fd) {
                sockaddr_in client_addr{};
                socklen_t   client_len = sizeof(client_addr);
                int client_fd = accept(server_fd,
                                       (sockaddr*)&client_addr, &client_len);
                if (client_fd < 0) {
                    std::cerr << "accept() failed\n";
                    continue;
                }

                // Set client fd non-blocking and register with epoll
                set_nonblocking(client_fd);
                epoll_event cev{};
                cev.events  = EPOLLIN;
                cev.data.fd = client_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev);

                std::cout << "[+] Client connected (fd=" << client_fd << ")"
                          << " — total watched fds tracked by epoll\n";

            // ── Case 2: a client fd is ready → data arrived ──────────────
            } else {
                char buffer[BUFFER_SIZE];
                memset(buffer, 0, sizeof(buffer));
                int bytes_read = recv(ready_fd, buffer, sizeof(buffer) - 1, 0);

                if (bytes_read <= 0) {
                    // 0 = clean disconnect, <0 = error
                    // Either way: remove from epoll and close
                    if (bytes_read == 0) {
                        std::cout << "[-] Client disconnected (fd="
                                  << ready_fd << ")\n";
                    } else {
                        std::cerr << "recv() error on fd=" << ready_fd << "\n";
                    }
                    epoll_ctl(epfd, EPOLL_CTL_DEL, ready_fd, nullptr);
                    close(ready_fd);

                } else {
                    // Parse and dispatch
                    std::string raw(buffer, bytes_read);
                    std::vector<std::string> cmd = parse_resp(raw);
                    std::string response = handle_command(cmd);
                    send(ready_fd, response.c_str(), response.size(), 0);

                    if (!cmd.empty()) {
                        std::cout << "  [fd=" << ready_fd << "] "
                                  << cmd[0];
                        for (size_t j = 1; j < cmd.size(); j++)
                            std::cout << " " << cmd[j];
                        std::cout << "\n";
                    }
                }
            }
        }
    }

    close(epfd);
    close(server_fd);
    return 0;
}