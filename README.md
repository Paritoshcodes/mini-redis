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
| 4 | Commands — PING, ECHO | ✅ Done |
| 5 | In-memory store — SET, GET, DEL | ✅ Done |
| 6 | Key expiry — EX, PX, TTL | ✅ Done |
| 7 | Concurrent clients — epoll event loop | ✅ Done |
| 8 | Robust RESP — buffering, pipelining, inline commands | ✅ Done |
| 9 | Pub/sub — SUBSCRIBE, PUBLISH | ✅ Done |

## How to build and run
```bash
g++ -O2 -Wall -Wextra -std=c++17 -o mini-redis server.cpp
./mini-redis
```

Then in a second terminal:
```bash
redis-cli ping     # should return PONG
redis-cli set name paritosh
redis-cli get name
redis-cli incr hits
redis-cli expire name 60
redis-cli ttl name
```

## Benchmarks

Reproducible throughput checks live in [bench/run-bench.sh](bench/run-bench.sh) and the captured run notes are in [BENCHMARK.md](BENCHMARK.md). Run `./bench/run-bench.sh` on a Linux host with `g++`, `redis-cli`, `redis-benchmark`, and `ss` available to reproduce the published numbers.

Measured on the captured run (single core): 150K+ commands/sec baseline, 1M+ pipelined, peaking at 4.2M+ with deep pipelining — see [BENCHMARK.md](BENCHMARK.md) for the full tables. The benchmark script asserts a portable 1K+ floor on every reported figure so the harness passes on any hardware.

## Supported commands

`PING`, `ECHO`, `QUIT`, `SET` (with `EX`/`PX`), `GET`, `DEL`, `INCR`, `EXPIRE`, `TTL`, `PTTL`, `PERSIST`, `EXISTS`, `KEYS`, `DBSIZE`, `FLUSHALL`, `SUBSCRIBE`, `UNSUBSCRIBE`, `PUBLISH`.

## Pub/sub

```bash
redis-cli subscribe news
redis-cli publish news hello
```

## Tech

- Language: C++
- OS: Linux (uses POSIX socket APIs)
- Tested on: Kali Linux

## What I'm learning

- How TCP servers work at the syscall level (`socket`, `bind`, `listen`, `accept`, `recv`, `send`)
- The RESP (Redis Serialization Protocol) wire format
- How stream buffering and framing keep pipelined protocols correct
- How `std::unordered_map` backs a key-value store
- Non-blocking I/O with `epoll`
- Pub/sub fan-out and subscriber mode restrictions
- How `std::chrono` enables TTL/expiry logic
- Benchmark methodology for reproducible throughput checks
