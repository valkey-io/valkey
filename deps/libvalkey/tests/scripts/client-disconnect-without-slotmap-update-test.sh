#!/bin/bash
#
# Verify that a client disconnects from known nodes without starting new
# slot map updates. A client shouldn't reconnect or connect to new nodes
# while shutting down.
#
# This testcase starts 2 cluster nodes and the client connects to both.
# The client triggers a disconnect right after a command has been sent,
# which will invoke its callback with a NULL reply. This NULL reply
# should not trigger a slot map update.
#
# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient_async}
testname=client-disconnect-without-slotmap-update-test

# Sync process just waiting for server to be ready to accept connection.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid1=$!
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid2=$!

# Start simulated valkey node #1
timeout 5s ./simulated-valkey.pl -p 7401 -d --sigcont $syncpid1 <<'EOF' &
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 6000, ["127.0.0.1", 7401, "nodeid1"]],[6001, 16383, ["127.0.0.1", 7402, "nodeid2"]]]
EXPECT CLOSE

EXPECT CONNECT
EXPECT ["SET", "bar", "initial"]
SEND +OK

# Normally a slot map update is expected here, but not during disconnects.

EXPECT CLOSE
EOF
server1=$!

# Start simulated valkey node #2
timeout 5s ./simulated-valkey.pl -p 7402 -d --sigcont $syncpid2 <<'EOF' &
EXPECT CONNECT
EXPECT ["SET", "foo", "initial"]
SEND +OK

EXPECT ["SET", "foo", "null-reply"]
# The client will invoke callbacks for outstanding requests with a NULL reply.
# Normally this would trigger a slot map update, but not during disconnects.

EXPECT CLOSE
EOF
server2=$!

# Wait until both nodes are ready to accept client connections
wait $syncpid1 $syncpid2;

# Run client
timeout 4s "$clientprog" --connection-events 127.0.0.1:7401 > "$testname.out" <<'EOF'
SET foo initial
SET bar initial

# Make sure a slot map update is not throttled.
!sleep

# Send a command just before requesting a client disconnect.
# A NULL reply should not trigger a slot map update after a disconnect.
!async
SET foo null-reply
!disconnect
!sync

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

expected="Event: connect to 127.0.0.1:7402
OK
Event: connect to 127.0.0.1:7401
OK
Event: disconnect from 127.0.0.1:7401
error: Timeout
Event: failed to disconnect from 127.0.0.1:7402"

echo "$expected" | diff -u - "$testname.out" || exit 99

# Clean up
rm "$testname.out"
