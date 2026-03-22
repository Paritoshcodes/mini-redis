# mini-redis

A Redis clone built from scratch in C++, following the internals of how Redis actually works — TCP sockets, the RESP wire protocol, in-memory storage, key expiry, and an event-driven I/O loop.

## Why I'm building this

I wanted to understand what happens *below* the API — how a server actually accepts connections, reads bytes off the wire, parses a protocol, and responds. This repo documents that learning process commit by commit.

## Build roadmap

| Part | What gets built | Status |
|------|----------------|--------|
| 1 | TCP server — socket, bind, listen, accept | ✅ Done |
| 2 | Read loop — recv(), raw RESP bytes visible | ✅ Done |
| 3 | RESP parser — decode arrays, bulk strings | ✅ Done |
| 4 | Commands — PING, ECHO | 🔄 In progress |
| 5 | In-memory store — SET, GET, DEL | ⏳ Upcoming |
| 6 | Key expiry — EX, PX, TTL | ⏳ Upcoming |
| 7 | Concurrent clients — epoll event loop | ⏳ Upcoming |

## How to build and run
```bash
g++ -o mini-redis server.cpp
./mini-redis
```

Then in a second terminal:
```bash
redis-cli ping     # should return PONG
redis-cli set name paritosh
redis-cli get name
```

## Tech

- Language: C++
- OS: Linux (uses POSIX socket APIs)
- Tested on: Kali Linux

## What I'm learning

- How TCP servers work at the syscall level (`socket`, `bind`, `listen`, `accept`, `recv`, `send`)
- The RESP (Redis Serialization Protocol) wire format
- How `std::unordered_map` backs a key-value store
- Non-blocking I/O with `epoll`
- How `std::chrono` enables TTL/expiry logic
