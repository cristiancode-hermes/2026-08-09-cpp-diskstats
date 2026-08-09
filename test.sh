#!/bin/bash
# Smoke test: compile + selftest + JSON validity
set -e
cd "$(dirname "$0")"
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o /tmp/diskstats-test
/tmp/diskstats-test --selftest
/tmp/diskstats-test --json | python3 -c "import json,sys; d=json.load(sys.stdin); assert d['summary']['disks']>0; print('JSON OK:', d['summary'])"
echo "SMOKE TEST PASSED"
