#!/bin/sh
#
# Connect to a cluster which is in the processes of starting up.
#
# The first attempt to get the slotmap will receive a reply without any
# slot information and this should result in a retry.
#
# The client is configured to use the CLUSTER NODES command.
#
# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient_async}
testname=connect-during-cluster-startup-using-cluster-nodes-test

# Sync process just waiting for server to be ready to accept connection.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid=$!

# Start simulated server.
timeout 5s ./simulated-valkey.pl -p 7400 -d --sigcont $syncpid <<'EOF' &
# The initial slotmap is not covering any slots, expect a retry.
EXPECT CONNECT
EXPECT ["CLUSTER", "NODES"]
SEND "8adca41945787ad1c9e725a40a43cf72bd4c6ad4 :7400@17400 myself,master - 0 0 0 connected\n"

# The node has now been delegated slots.
EXPECT ["CLUSTER", "NODES"]
SEND "8adca41945787ad1c9e725a40a43cf72bd4c6ad4 :7400@17400 myself,master - 0 0 1 connected 0-16383\n"

EXPECT ["SET", "foo", "bar"]
SEND +OK
EXPECT CLOSE
EOF
server=$!

# Wait until server is ready to accept client connection.
wait $syncpid;

# Run client which will fetch the initial slotmap asynchronously using CLUSTER NODES.
timeout 3s "$clientprog" --events --use-cluster-nodes 127.0.0.1:7400 > "$testname.out" <<'EOF'
SET foo bar
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
OK
Event: free-context"

echo "$expected" | diff -u - "$testname.out" || exit 99

# Clean up.
rm "$testname.out"
