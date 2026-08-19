#!/usr/bin/env bash
set -euo pipefail

for tool in ss g++ redis-benchmark redis-cli; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "missing required tool: $tool" >&2
    exit 1
  }
done

if ss -ltn | awk '$4 ~ /:6379$/ { found = 1 } END { exit found ? 0 : 1 }'; then
  echo "port 6379 is already in use" >&2
  exit 1
fi

g++ -O2 -Wall -Wextra -std=c++17 -o mini-redis server.cpp

nohup ./mini-redis > /tmp/server.log 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT

sleep 0.5

check_rps_floor() {
  local output=$1
  printf '%s\n' "$output" | awk '
    /requests per second/ {
      if ($2 + 0 < 1000) {
        printf("throughput floor failed: %s\n", $0) > "/dev/stderr"
        exit 1
      }
    }
  '
}

echo "== context =="
uname -srm
nproc
g++ --version | head -1
date

echo "== baseline =="
baseline_output=$(timeout 60 redis-benchmark -t ping,set,get -n 100000 -c 50 -q)
printf '%s\n' "$baseline_output"
check_rps_floor "$baseline_output"

echo "== pipelined =="
pipelined_output=$(timeout 60 redis-benchmark -t ping,set,get -n 100000 -c 50 -P 16 -q)
printf '%s\n' "$pipelined_output"
check_rps_floor "$pipelined_output"

echo "== pubsub =="
sub_out=$(mktemp)
stdbuf -oL -eL timeout 6 redis-cli subscribe bench > "$sub_out" 2>&1 &
SUB_PID=$!
sleep 0.5

for i in $(seq 1 1000); do
  redis-cli publish bench "msg-$i" >/dev/null
done

wait "$SUB_PID" 2>/dev/null || true
msg_count=$(grep -c '^message$' "$sub_out" || true)
echo "messages delivered: $msg_count"

if [ "$msg_count" -lt 1000 ]; then
  echo "pub/sub delivery floor failed" >&2
  exit 1
fi

echo "benchmark run complete"