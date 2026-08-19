# Benchmark Notes

Reproducible benchmark harness: [bench/run-bench.sh](bench/run-bench.sh).

## Methodology

- Build: `g++ -O2 -Wall -Wextra -std=c++17 -o mini-redis server.cpp`
- Server: `./mini-redis` on 127.0.0.1:6379
- Baseline: `redis-benchmark -t ping,set,get -n 100000 -c 50 -q`
- Pipelined: `redis-benchmark -t ping,set,get -n 500000 -c 50 -P 16 -q`
- Peak: `redis-benchmark -t ping,set,get -n 1000000 -c 50 -P 128 -q`
- Pub/sub: 1000 messages published to one subscriber; the script asserts all 1000 delivered
- The script asserts a portable 1,000 req/sec floor on every reported figure and exits non-zero on failure

## Captured results

Environment: Linux 6.18.5 x86_64, 1 core, g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0, captured 2026-08-19 by running `./bench/run-bench.sh` from a fresh clone of this branch. Harness exit code 0; every floor assertion passed.

Baseline (`-c 50`):

| Test | Requests/sec | p50 latency |
|------|-------------:|------------:|
| PING_INLINE | 152,905 | 0.055 msec |
| PING_MBULK  | 154,798 | 0.047 msec |
| SET         | 151,745 | 0.207 msec |
| GET         | 159,489 | 0.199 msec |

Pipelined (`-c 50 -P 16`):

| Test | Requests/sec | p50 latency |
|------|-------------:|------------:|
| PING_INLINE | 1,779,359 | 0.319 msec |
| PING_MBULK  | 1,607,717 | 0.087 msec |
| SET         | 1,079,913 | 0.671 msec |
| GET         | 1,333,333 | 0.535 msec |

Peak (`-c 50 -P 128`):

| Test | Requests/sec | p50 latency |
|------|-------------:|------------:|
| PING_INLINE | 4,255,591 | 1.487 msec |
| PING_MBULK  | 3,921,819 | 1.623 msec |
| SET         | 2,487,721 | 0.535 msec |
| GET         | 2,754,997 | 2.279 msec |

Pub/sub: 1000 / 1000 messages delivered.

Headline: 150K+ commands/sec baseline, 1M+ pipelined (P16), peaking at 4.2M+ with deep pipelining (P128) — measured on a single core.

## Reproduction

Run `./bench/run-bench.sh`.