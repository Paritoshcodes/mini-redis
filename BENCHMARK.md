# Benchmark Notes

This repo now includes a reproducible benchmark harness at [bench/run-bench.sh](bench/run-bench.sh).

## Methodology

- Build with `g++ -O2 -Wall -Wextra -std=c++17 -o mini-redis server.cpp`
- Start the server on port 6379 with `nohup ./mini-redis > /tmp/server.log 2>&1 &`
- Run `redis-benchmark -t ping,set,get -n 100000 -c 50 -q`
- Run `redis-benchmark -t ping,set,get -n 100000 -c 50 -P 16 -q`
- Run the pub/sub smoke test that publishes 1000 messages to one subscriber

## Results

The authoritative benchmark results should be captured by running `./bench/run-bench.sh` on a Linux host with the required toolchain installed.

This session could not regenerate local benchmark numbers because the Windows shell in this workspace does not currently expose `g++`, `redis-cli`, `redis-benchmark`, `nc`, or a working WSL environment.

## Reproduction

Run `./bench/run-bench.sh`.