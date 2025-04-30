#!/bin/bash

# Usage: $0 /path/to/clusterclient-async

clientprog=${1:-./clusterclient-async}
testname=slots-not-served-test-async

# Sync processes waiting for CONT signals.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid1=$!;

# Start simulated valkey node #1
timeout 5s ./simulated-valkey.pl -p 7401 -d --sigcont $syncpid1 <<'EOF' &
# The initial slotmap is not covering all slots.
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 1, ["127.0.0.1", 7401, "nodeid7401"]]]
EXPECT CLOSE

# Slotmap update due to slot not served.
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 16383, ["127.0.0.1", 7401, "nodeid7401"]]]

EXPECT ["GET", "foo"]
SEND "bar"
EXPECT CLOSE
EOF
server1=$!

# Wait until node is ready to accept client connections
wait $syncpid1;

# Run client
timeout 3s "$clientprog" --events 127.0.0.1:7401 > "$testname.out" <<'EOF'
GET foo
# Allow slotmap update to finish.
!sleep
GET foo
EOF
clientexit=$?

# Wait for server to exit
wait $server1; server1exit=$?

# Check exit statuses
if [ $server1exit -ne 0 ]; then
    echo "Simulated server #1 exited with status $server1exit"
    exit $server1exit
fi
if [ $clientexit -ne 0 ]; then
    echo "$clientprog exited with status $clientexit"
    exit $clientexit
fi

# Check the output from clusterclient
expected="Event: slotmap-updated
Event: ready
error: slot not served by any node
Event: slotmap-updated
bar
Event: free-context"

echo "$expected" | diff -u - "$testname.out" || exit 99

# Clean up
rm "$testname.out"
