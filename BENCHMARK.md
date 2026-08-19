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

## Results

Pending capture: run `./bench/run-bench.sh` on a Linux host with `g++`, `redis-cli`, `redis-benchmark`, and `ss` available, then record the emitted tables here. The harness asserts a 1,000 req/sec floor on every figure and 1000/1000 pub/sub delivery, and exits non-zero on any failure.

## Reproduction

Run `./bench/run-bench.sh`.