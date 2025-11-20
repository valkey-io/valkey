#!/bin/sh
#
# Connect to a cluster which is in the processes of starting up.
#
# The first attempt to get the slotmap will receive a reply without any
# slot information and this should result in a retry.
# The following slotmap updates tests the handling of a nil/empty IP address.
#
# The client is configured to use the CLUSTER SLOTS command.
#
# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient_async}
testname=connect-during-cluster-startup-test

# Sync process just waiting for server to be ready to accept connection.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid=$!

# Start simulated server.
timeout 5s ./simulated-valkey.pl -p 7400 -d --sigcont $syncpid <<'EOF' &
# The initial slotmap is not covering any slots, expect a retry since it's not accepted.
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND []

# The node has now been delegated a few slots and should be accepted.
# Respond with an unknown endpoint (nil) to test that current connection IP is used instead.
EXPECT ["CLUSTER", "SLOTS"]
SEND *1\r\n*3\r\n:0\r\n:10\r\n*3\r\n$-1\r\n:7400\r\n$40\r\nf5378fa2ad1fbd569f01ba2fe29fa8feb36cdfb8\r\n

# The node has now been delegated all slots.
# Use empty address to test that current connection IP is used instead.
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 16383, ["", 7400, "f5378fa2ad1fbd569f01ba2fe29fa8feb36cdfb8"]]]

EXPECT ["SET", "foo", "bar3"]
SEND +OK
EXPECT CLOSE
EOF
server=$!

# Wait until server is ready to accept client connection.
wait $syncpid;

# Run client which will fetch the initial slotmap asynchronously.
timeout 3s "$clientprog" --events 127.0.0.1:7400 > "$testname.out" <<'EOF'
# Slot not yet handled, will trigger a slotmap update which will be throttled.
SET foo bar1

# Wait to avoid slotmap update throttling.
!sleep

# A command will fail directly, but a slotmap update is scheduled.
SET foo bar2

# Allow slotmap update to finish.
!sleep

SET foo bar3
EOF
clientexit=$?

# Wait for server to exit.
wait $server; serverexit=$?

# Check exit status on server.
if [ $serverexit -ne 0 ]; then
    echo "Simulated server exited with status $serverexit"
    exit $serverexit
fi
# Check exit status on client.
if [ $clientexit -ne 0 ]; then
    echo "$clientprog exited with status $clientexit"
    exit $clientexit
fi

# Check the output from the client.
expected="Event: slotmap-updated
Event: ready
error: slot not served by any node
error: slot not served by any node
Event: slotmap-updated
OK
Event: free-context"

echo "$expected" | diff -u - "$testname.out" || exit 99

# Clean up.
rm "$testname.out"
