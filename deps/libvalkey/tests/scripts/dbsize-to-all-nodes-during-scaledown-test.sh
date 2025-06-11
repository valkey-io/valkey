#!/bin/bash
#
# Verify that commands can be sent using the low-level API to specific
# nodes. The testcase will send each command to all known nodes and
# verify the behaviour when a node is removed from the cluster.
#
# First the command DBSIZE is sent to all (two) nodes successfully,
# then the second node is shutdown. The next DBSIZE command that is sent
# triggers a slotmap update due to the lost node and all following commands
# will therefor only be sent to a single node.
#
# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient}
testname=dbsize-to-all-nodes-during-scaledown-test

# Sync processes waiting for CONT signals.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid1=$!;
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid2=$!;

# Start simulated valkey node #1
timeout 5s ./simulated-valkey.pl -p 7401 -d --sigcont $syncpid1 <<'EOF' &
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 8383, ["127.0.0.1", 7401, "nodeid7401"]], [8384, 16383, ["127.0.0.1", 7402, "nodeid7402"]]]
EXPECT CLOSE
EXPECT CONNECT
EXPECT ["DBSIZE"]
SEND 10
EXPECT ["DBSIZE"]
SEND 11
EXPECT ["DBSIZE"]
SEND 12
# The second command to node #2 fails which triggers a slotmap update pipelined
# onto the 3rd DBSIZE to this node.
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 16383, ["127.0.0.1", 7401, "nodeid7401"]]]
EXPECT CLOSE
EOF
server1=$!

# Start simulated valkey node #2
timeout 5s ./simulated-valkey.pl -p 7402 -d --sigcont $syncpid2 <<'EOF' &
EXPECT CONNECT
EXPECT ["DBSIZE"]
SEND 20
# Forced close. The second command to this node should trigger a slotmap update.
EOF
server2=$!

# Wait until both nodes are ready to accept client connections
wait $syncpid1 $syncpid2;

# Run client
timeout 5s "$clientprog" 127.0.0.1:7401 > "$testname.out" <<'EOF'
!all
DBSIZE
DBSIZE
# Allow slotmap update to finish.
!sleep
DBSIZE
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

# Check the output from clusterclient, which depends on timing.
# Client sends the second 'DBSIZE' to node #2 just after node #2 closes its socket.
expected1="10
20
11
error: Server closed the connection
12"

# Client sends the second 'DBSIZE' to node #2 just before node #2 closes its socket.
expected2="10
20
11
error: Connection reset by peer
12"

diff -u "$testname.out" <(echo "$expected1") || \
    diff -u "$testname.out" <(echo "$expected2") || \
    exit 99

# Clean up
rm "$testname.out"
