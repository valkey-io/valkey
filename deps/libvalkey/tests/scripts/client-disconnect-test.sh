#!/bin/bash
#
# Verify that a client disconnects from known nodes without following
# redirects, this to avoid reconnecting to already disconnected nodes.
# The client will not accept new commands thereafter.
#
# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient_async}
testname=client-disconnect-test

# Sync processes waiting for CONT signals.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid1=$!;

# Start simulated valkey node
timeout 5s ./simulated-valkey.pl -p 7401 -d --sigcont $syncpid1 <<'EOF' &
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 16383, ["127.0.0.1", 7401, "nodeid1"]]]
EXPECT CLOSE

EXPECT CONNECT
EXPECT ["SET", "foo", "initial"]
SEND +OK

EXPECT ["SET", "foo", "redirect"]
SEND -MOVED 12182 127.0.0.1:7402

EXPECT CLOSE
EOF
server1=$!

# Wait until node is ready to accept client connections
wait $syncpid1;

# Run client
timeout 4s "$clientprog" --connection-events 127.0.0.1:7401 > "$testname.out" <<'EOF'
SET foo initial

# Send a command that is expected to be redirected just before
# requesting a client disconnect.
!async
SET foo redirect
!disconnect
!sync

# Commands are not accepted after a disconnect.
SET foo not-accepted
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

expected="Event: connect to 127.0.0.1:7401
OK
MOVED 12182 127.0.0.1:7402
Event: disconnect from 127.0.0.1:7401
error: disconnecting"

echo "$expected" | diff -u - "$testname.out" || exit 99

# Clean up
rm "$testname.out"
