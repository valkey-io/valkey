#!/bin/bash
#
# Verify that a configured connection timeout is used when
# connecting to a new node indicated in a ASK redirect.
#
# The ASK redirect response will in this test give a non-reachable
# black hole address which should trigger a connection timeout.
#
# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient}
testname=ask-redirect-connection-error-test

# Sync processes waiting for CONT signals.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid1=$!;

# Start simulated valkey node #1
timeout 5s ./simulated-valkey.pl -p 7401 -d --sigcont $syncpid1 <<'EOF' &
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 16383, ["127.0.0.1", 7401, "nodeid123"]]]
EXPECT CLOSE

EXPECT CONNECT
EXPECT ["SET", "foo", "initial"]
SEND +OK

EXPECT ["SET", "foo", "timeout1"]
EXPECT ["SET", "foo", "timeout2"]
EXPECT ["SET", "foo", "timeout3"]
EXPECT ["SET", "foo", "timeout4"]
# ASK redirect to a non-reachable node
SEND -ASK 12182 192.168.254.254:9999
SEND -ASK 12182 192.168.254.254:9999
SEND -ASK 12182 192.168.254.254:9999
SEND -ASK 12182 192.168.254.254:9999

# The failed connection attempt triggers an update
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 16383, ["127.0.0.1", 7401, "nodeid123"]]]

EXPECT CLOSE
EOF
server1=$!

# Wait until node is ready to accept client connections
wait $syncpid1;

# Run client
timeout 4s "$clientprog" 127.0.0.1:7401 > "$testname.out" <<'EOF'
SET foo initial

!async
SET foo timeout1
SET foo timeout2
SET foo timeout3
SET foo timeout4
!sync
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

expected="OK
error: Timeout
error: Timeout
error: Timeout
error: Timeout"

cmp "$testname.out" <(echo "$expected") || exit 99

# Clean up
rm "$testname.out"
