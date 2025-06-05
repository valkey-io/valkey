#!/bin/bash
#
# Simulate a 2 node cluster where nodeid2 is unreachable (black hole address).
# Timed out commands, due to the connect error, will trigger a slot map update.
#
# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient_async_sequence}
testname=connection-error-test

# Sync processes waiting for CONT signals.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid1=$!;

# Start simulated valkey node #1
timeout 5s ./simulated-valkey.pl -p 7401 -d --sigcont $syncpid1 <<'EOF' &
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 6000, ["127.0.0.1", 7401, "nodeid1"]],[6001, 16383, ["192.168.254.254", 9999, "nodeid2"]]]
EXPECT CLOSE

EXPECT CONNECT
EXPECT ["SET", "bar", "initial"]
SEND +OK

# Topology changed, nodeid2 is now gone
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 16383, ["127.0.0.1", 7401, "nodeid1"]]]

EXPECT ["SET", "bar", "second"]
SEND +OK

EXPECT ["SET", "foo", "newnode-1"]
SEND +OK

EXPECT ["SET", "foo", "newnode-2"]
SEND +OK

EXPECT CLOSE
EOF
server1=$!

# Wait until node is ready to accept client connections
wait $syncpid1

# Run client
timeout 4s "$clientprog" 127.0.0.1:7401 > "$testname.out" <<'EOF'
SET bar initial

# Send commands aimed for nodeid2
!async
SET foo initial-1
SET foo initial-2
SET foo initial-3
SET foo initial-4
!sync

# Send a command to give time for the slot map update to finish
SET bar second

# Slots should now have moved
!async
SET foo newnode-1
SET foo newnode-2
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
error: Timeout
OK
OK
OK"

cmp "$testname.out" <(echo "$expected") || exit 99

# Clean up
rm "$testname.out"
