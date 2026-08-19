#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <thread>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>         // fcntl() for non-blocking mode
#include <sys/epoll.h>     // epoll_create1, epoll_ctl, epoll_wait

const int PORT        = 6379;
const int BACKLOG     = 10;
const int BUFFER_SIZE = 4096;
const int MAX_EVENTS  = 64;   // max events epoll returns per wait call

// ── Type alias ───────────────────────────────────────────────────────────────
using TimePoint = std::chrono::steady_clock::time_point;

struct Client {
    std::string inbuf;
    std::unordered_set<std::string> channels;
};

// ── Stores ───────────────────────────────────────────────────────────────────
std::unordered_map<std::string, std::string> store;
std::unordered_map<std::string, TimePoint>   expiry_map;
std::unordered_map<int, Client>              clients;
std::unordered_map<std::string, std::unordered_set<int>> channel_subs;
int g_epfd = -1;

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

bool parse_int(const std::string& text, int& value) {
    try {
        size_t consumed = 0;
        int parsed = std::stoi(text, &consumed);
        if (consumed != text.size()) return false;
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

std::string uppercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

enum class ParseStatus { OK, INCOMPLETE, PROTO_ERROR };

ParseStatus try_parse_command(const std::string& buf, std::vector<std::string>& out, size_t& consumed) {
    out.clear();
    consumed = 0;

    if (buf.empty()) return ParseStatus::INCOMPLETE;

    if (buf[0] == '*') {
        size_t pos = 1;
        size_t line_end = buf.find("\r\n", pos);
        if (line_end == std::string::npos) return ParseStatus::INCOMPLETE;

        int num_elements = 0;
        if (!parse_int(buf.substr(pos, line_end - pos), num_elements) || num_elements < 0) {
            return ParseStatus::PROTO_ERROR;
        }

        pos = line_end + 2;
        out.reserve(static_cast<size_t>(num_elements));

        for (int i = 0; i < num_elements; ++i) {
            if (pos >= buf.size()) return ParseStatus::INCOMPLETE;
            if (buf[pos] != '$') return ParseStatus::PROTO_ERROR;
            ++pos;

            line_end = buf.find("\r\n", pos);
            if (line_end == std::string::npos) return ParseStatus::INCOMPLETE;

            int str_len = 0;
            if (!parse_int(buf.substr(pos, line_end - pos), str_len) || str_len < 0) {
                return ParseStatus::PROTO_ERROR;
            }

            pos = line_end + 2;
            if (pos + static_cast<size_t>(str_len) > buf.size()) return ParseStatus::INCOMPLETE;
            out.push_back(buf.substr(pos, static_cast<size_t>(str_len)));
            pos += static_cast<size_t>(str_len);

            if (pos + 2 > buf.size()) return ParseStatus::INCOMPLETE;
            if (buf[pos] != '\r' || buf[pos + 1] != '\n') return ParseStatus::PROTO_ERROR;
            pos += 2;
        }

        consumed = pos;
        return ParseStatus::OK;
    }

    size_t line_end = buf.find("\r\n");
    if (line_end == std::string::npos) return ParseStatus::INCOMPLETE;

    std::string line = buf.substr(0, line_end);
    consumed = line_end + 2;

    size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
            ++pos;
        }
        if (pos >= line.size()) break;

        size_t start = pos;
        while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') {
            ++pos;
        }
        out.push_back(line.substr(start, pos - start));
    }

    return ParseStatus::OK;
}

bool send_all(int fd, const std::string& data) {
    size_t written = 0;
    int retry_count = 0;

    while (written < data.size()) {
        ssize_t n = send(fd, data.data() + written, data.size() - written, MSG_NOSIGNAL);
        if (n > 0) {
            written += static_cast<size_t>(n);
            retry_count = 0;
            continue;
        }

        if (n < 0 && errno == EINTR) continue;

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++retry_count > 1000) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        return false;
    }

    return true;
}

void disconnect(int fd, int epfd) {
    auto client_it = clients.find(fd);
    if (client_it != clients.end()) {
        std::vector<std::string> channel_list(client_it->second.channels.begin(), client_it->second.channels.end());
        for (const std::string& channel : channel_list) {
            auto subs_it = channel_subs.find(channel);
            if (subs_it == channel_subs.end()) continue;
            subs_it->second.erase(fd);
            if (subs_it->second.empty()) {
                channel_subs.erase(subs_it);
            }
        }
        clients.erase(client_it);
    }

    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
}

// ── Command dispatcher ───────────────────────────────────────────────────────
std::string handle_command(int fd, const std::vector<std::string>& cmd) {
    if (cmd.empty()) return resp_error("empty command");

    std::string name = uppercase_copy(cmd[0]);
    auto client_it = clients.find(fd);
    Client* client = client_it == clients.end() ? nullptr : &client_it->second;
    bool subscriber_mode = client != nullptr && !client->channels.empty();

    if (subscriber_mode && name != "SUBSCRIBE" && name != "UNSUBSCRIBE" && name != "PING" && name != "QUIT") {
        return resp_error("only (P)SUBSCRIBE / (P)UNSUBSCRIBE / PING / QUIT allowed in this context");
    }

    if (name == "PING") {
        return cmd.size() == 1 ? resp_simple("PONG") : resp_bulk(cmd[1]);
    }
    if (name == "QUIT") {
        return resp_simple("OK");
    }
    if (name == "ECHO") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'echo'");
        return resp_bulk(cmd[1]);
    }
    if (name == "SET") {
        if (cmd.size() < 3) return resp_error("wrong number of arguments for 'set'");
        store[cmd[1]] = cmd[2];
        expiry_map.erase(cmd[1]);
        auto now = std::chrono::steady_clock::now();
        for (size_t i = 3; i + 1 < cmd.size(); i += 2) {
            std::string opt = uppercase_copy(cmd[i]);
            int duration = 0;
            if (opt == "EX" || opt == "PX") {
                if (!parse_int(cmd[i + 1], duration)) {
                    return resp_error("value is not an integer or out of range");
                }
            }
            if (opt == "EX") {
                expiry_map[cmd[1]] = now + std::chrono::seconds(duration);
            } else if (opt == "PX") {
                expiry_map[cmd[1]] = now + std::chrono::milliseconds(duration);
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
        return resp_integer(static_cast<int>(expiry_map.erase(cmd[1])));
    }
    if (name == "EXPIRE") {
        if (cmd.size() != 3) return resp_error("wrong number of arguments for 'expire'");
        int seconds = 0;
        if (!parse_int(cmd[2], seconds)) {
            return resp_error("value is not an integer or out of range");
        }
        if (check_and_expire(cmd[1])) return resp_integer(0);
        if (!store.count(cmd[1]))     return resp_integer(0);
        expiry_map[cmd[1]] = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        return resp_integer(1);
    }
    if (name == "INCR") {
        if (cmd.size() != 2) return resp_error("wrong number of arguments for 'incr'");
        check_and_expire(cmd[1]);
        long long value = 0;
        auto it = store.find(cmd[1]);
        if (it != store.end()) {
            errno = 0;
            char* end = nullptr;
            value = std::strtoll(it->second.c_str(), &end, 10);
            if (errno != 0 || end == it->second.c_str() || *end != '\0') {
                return resp_error("value is not an integer or out of range");
            }
        }
        value += 1;
        store[cmd[1]] = std::to_string(value);
        return ":" + std::to_string(value) + "\r\n";
    }
    if (name == "EXISTS") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'exists'");
        if (check_and_expire(cmd[1])) return resp_integer(0);
        return resp_integer(static_cast<int>(store.count(cmd[1])));
    }
    if (name == "DBSIZE") {
        return resp_integer(static_cast<int>(store.size()));
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
    if (name == "SUBSCRIBE") {
        if (cmd.size() < 2) return resp_error("wrong number of arguments for 'subscribe'");
        if (client == nullptr) return resp_error("client state missing");

        std::string response;
        for (size_t i = 1; i < cmd.size(); ++i) {
            client->channels.insert(cmd[i]);
            channel_subs[cmd[i]].insert(fd);

            response += "*3\r\n$9\r\nsubscribe\r\n";
            response += resp_bulk(cmd[i]);
            response += ":" + std::to_string(client->channels.size()) + "\r\n";
        }
        return response;
    }
    if (name == "UNSUBSCRIBE") {
        if (client == nullptr) return resp_error("client state missing");

        std::vector<std::string> channels;
        if (cmd.size() == 1) {
            if (client->channels.empty()) {
                return "*3\r\n$11\r\nunsubscribe\r\n$-1\r\n:0\r\n";
            }
            channels.assign(client->channels.begin(), client->channels.end());
        } else {
            channels.assign(cmd.begin() + 1, cmd.end());
        }

        std::string response;
        for (const std::string& channel : channels) {
            client->channels.erase(channel);
            auto subs_it = channel_subs.find(channel);
            if (subs_it != channel_subs.end()) {
                subs_it->second.erase(fd);
                if (subs_it->second.empty()) {
                    channel_subs.erase(subs_it);
                }
            }

            response += "*3\r\n$11\r\nunsubscribe\r\n";
            response += resp_bulk(channel);
            response += ":" + std::to_string(client->channels.size()) + "\r\n";
        }
        return response;
    }
    if (name == "PUBLISH") {
        if (cmd.size() != 3) return resp_error("wrong number of arguments for 'publish'");

        int receivers = 0;
        auto subs_it = channel_subs.find(cmd[1]);
        if (subs_it != channel_subs.end()) {
            std::vector<int> targets(subs_it->second.begin(), subs_it->second.end());
            std::string message = "*3\r\n$7\r\nmessage\r\n" + resp_bulk(cmd[1]) + resp_bulk(cmd[2]);

            for (int target_fd : targets) {
                if (send_all(target_fd, message)) {
                    ++receivers;
                } else {
                    disconnect(target_fd, g_epfd);
                }
            }
        }

        return resp_integer(receivers);
    }
    return resp_error("unknown command '" + cmd[0] + "'");
}

// ── Main — epoll event loop ──────────────────────────────────────────────────
int main() {
    signal(SIGPIPE, SIG_IGN);

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
    g_epfd = epfd;

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
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        std::cerr << "accept() failed\n";
                    }
                    continue;
                }

                // Set client fd non-blocking and register with epoll
                set_nonblocking(client_fd);
                int nd = 1;
                setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
                clients.emplace(client_fd, Client{});
                epoll_event cev{};
                cev.events  = EPOLLIN;
                cev.data.fd = client_fd;
                if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev) < 0) {
                    clients.erase(client_fd);
                    close(client_fd);
                    continue;
                }

                std::cout << "[+] Client connected (fd=" << client_fd << ")"
                          << " — total watched fds tracked by epoll\n";

            // ── Case 2: a client fd is ready → data arrived ──────────────
            } else {
                auto client_it = clients.find(ready_fd);
                if (client_it == clients.end()) {
                    disconnect(ready_fd, epfd);
                    continue;
                }

                Client& client = client_it->second;
                bool close_client = false;

                std::string outbuf;
                while (true) {
                    char buffer[BUFFER_SIZE];
                    ssize_t bytes_read = recv(ready_fd, buffer, sizeof(buffer), 0);

                    if (bytes_read > 0) {
                        client.inbuf.append(buffer, static_cast<size_t>(bytes_read));
                        if (client.inbuf.size() > 1024 * 1024) {
                            send_all(ready_fd, resp_error("Protocol error"));
                            disconnect(ready_fd, epfd);
                            close_client = true;
                            break;
                        }
                        continue;
                    }

                    if (bytes_read == 0) {
                        std::cout << "[-] Client disconnected (fd=" << ready_fd << ")\n";
                        disconnect(ready_fd, epfd);
                        close_client = true;
                        break;
                    }

                    if (errno == EINTR) continue;

                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }

                    std::cerr << "recv() error on fd=" << ready_fd << "\n";
                    disconnect(ready_fd, epfd);
                    close_client = true;
                    break;
                }

                if (close_client) {
                    continue;
                }

                while (true) {
                    std::vector<std::string> cmd;
                    size_t consumed = 0;
                    ParseStatus status = try_parse_command(client.inbuf, cmd, consumed);

                    if (status == ParseStatus::INCOMPLETE) {
                        break;
                    }

                    if (status == ParseStatus::PROTO_ERROR) {
                        send_all(ready_fd, resp_error("Protocol error"));
                        disconnect(ready_fd, epfd);
                        close_client = true;
                        break;
                    }

                    client.inbuf.erase(0, consumed);
                    if (cmd.empty()) {
                        continue;
                    }

                    outbuf += handle_command(ready_fd, cmd);

                    if (uppercase_copy(cmd[0]) == "QUIT") {
                        if (!outbuf.empty()) { send_all(ready_fd, outbuf); outbuf.clear(); }
                        disconnect(ready_fd, epfd);
                        close_client = true;
                        break;
                    }
                }

                if (!close_client && !outbuf.empty()) {
                    if (!send_all(ready_fd, outbuf)) {
                        disconnect(ready_fd, epfd);
                        close_client = true;
                    }
                }

                if (close_client) {
                    continue;
                }
            }
        }
    }

    close(epfd);
    close(server_fd);
    return 0;
}