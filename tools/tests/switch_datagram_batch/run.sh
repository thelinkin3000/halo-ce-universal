#!/bin/sh
# Standalone asset-free test; CC may select clang or gcc.
set -eu
ulimit -c 0
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
awk '/^#define DATAGRAM_BATCH / { copy=1 } /^\/\* ---------- empty sockets watched/ { copy=0 } copy {print}' \
    "$root/port/switch/host/host_net.c" > "$tmp/production.inc"
build_run() {
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
        -I"$tmp" "$root/tools/tests/switch_datagram_batch/test.c" -o "$tmp/test"
    "$tmp/test"
}
cp "$tmp/production.inc" "$tmp/under_test.inc"
build_run
# Each old behavior must independently fail a regression case.
sed 's/vectors\[index\].iov_len = DATAGRAM_SLOT;/vectors[index].iov_len = (size_t)length;/' \
    "$tmp/production.inc" > "$tmp/under_test.inc"
if (build_run) > "$tmp/negative-length.log" 2>&1; then
    echo 'FAIL: first-read-size negative control passed' >&2; exit 1
fi
grep -q 'Assertion\|assertion' "$tmp/negative-length.log"
sed 's/ || queue->datagrams\[queue->next\].size > length//' \
    "$tmp/production.inc" > "$tmp/under_test.inc"
if (build_run) > "$tmp/negative-truncation.log" 2>&1; then
    echo 'FAIL: missing-read-truncation negative control passed' >&2; exit 1
fi
grep -q 'Assertion\|assertion' "$tmp/negative-truncation.log"
echo 'PASS: both old-behavior negative controls rejected'
