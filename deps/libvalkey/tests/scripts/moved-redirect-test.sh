#!/bin/bash
#
# Test of MOVED redirect handling.
#
# Test 1: Handle a common MOVED redirect.
# Test 2: Handle a MOVED redirect with an empty endpoint.
#         "The next request should be sent to the same endpoint as the
#          current request but with the provided port."
#
# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient}
testname=moved-redirect-test

# Sync processes waiting for CONT signals.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid1=$!;
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid2=$!;

# Start simulated valkey node #1
timeout 5s ./simulated-valkey.pl -p 7403 -d --sigcont $syncpid1 <<'EOF' &
# Setup initial slotmap
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 16383, ["127.0.0.1", 7403, "nodeid7403"]]]
EXPECT ["GET", "foo"]
SEND "bar"

# Test 1: Handle MOVED redirect.
EXPECT ["GET", "foo"]
SEND -MOVED 12182 127.0.0.1:7404
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 16383, ["127.0.0.1", 7404, "nodeid7404"]]]
EXPECT CLOSE

# Test 2: Handle empty endpoint.
EXPECT CONNECT
EXPECT ["GET", "foo"]
SEND "bar"
EXPECT CLOSE
EOF
server1=$!

# Start simulated valkey node #2
timeout 5s ./simulated-valkey.pl -p 7404 -d --sigcont $syncpid2 <<'EOF' &
# Test 1: Handle MOVED redirect.
EXPECT CONNECT
EXPECT ["GET", "foo"]
SEND "bar"

# Test 2: Handle empty endpoint.
EXPECT ["GET", "foo"]
SEND -MOVED 9718 :7403
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 16383, ["127.0.0.1", 7403, "nodeid7403"]]]

EXPECT CLOSE
EOF
server2=$!

# Wait until both nodes are ready to accept client connections
wait $syncpid1 $syncpid2;

# Run client
timeout 3s "$clientprog" --events 127.0.0.1:7403 > "$testname.out" <<'EOF'
GET foo
!sleep
GET foo
!sleep
GET foo
EOF
clientexit=$?

# Wait for servers to exit
wait $server1; server1exit=$?
wait $server2; server2exit=$?

# Check exit statuses
if [ $server1exit -ne 0 ]; then
    echo "Simulated server #1 exited with status $server1exit"
    exit $server1exit
fi
if [ $server2exit -ne 0 ]; then
    echo "Simulated server #2 exited with status $server2exit"
    exit $server2exit
fi
if [ $clientexit -ne 0 ]; then
    echo "$clientprog exited with status $clientexit"
    exit $clientexit
fi

# Check the output from clusterclient
expected="Event: slotmap-updated
Event: ready
bar
Event: slotmap-updated
bar
Event: slotmap-updated
bar
Event: free-context"

echo "$expected" | diff -u - "$testname.out" || exit 99

# Clean up
rm "$testname.out"
