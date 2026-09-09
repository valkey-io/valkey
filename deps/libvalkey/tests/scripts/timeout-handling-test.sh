#!/bin/bash
#
# Simulate a 2 node cluster.
# - Node 1 will handle topology requests.
# - Node 2 will have a problem which results in timed out requests.
#
# The client will first send a successful command, followed by
# 4 pipelined commands which are all timed out.
#
# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient_async_sequence}
testname=timeout-handling-test

# Sync process just waiting for server to be ready to accept connection.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid1=$!
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid2=$!

# Start simulated valkey node #1
timeout 5s ./simulated-valkey.pl -p 7401 -d --sigcont $syncpid1 <<'EOF' &
# Initial slotmap update
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 6000, ["127.0.0.1", 7401, "nodeid1"]],[6001, 16383, ["127.0.0.1", 7402, "nodeid2"]]]
EXPECT CLOSE

EXPECT CONNECT
EXPECT ["SET", "bar", "initial"]
SEND +OK

# Topology changed, nodeid2 is now gone
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 16383, ["127.0.0.1", 7401, "nodeid1"]]]

EXPECT CLOSE
EOF
server1=$!

# Start simulated valkey node #2
timeout 5s ./simulated-valkey.pl -p 7402 -d --sigcont $syncpid2 <<'EOF' &
EXPECT CONNECT
EXPECT ["SET", "foo", "initial"]
SEND +OK

# Timeout triggers a new topology fetch
EXPECT ["SET", "foo", "timeout1"]
EXPECT ["SET", "foo", "timeout2"]
EXPECT ["SET", "foo", "timeout3"]
EXPECT ["SET", "foo", "timeout4"]
SLEEP 1

EXPECT CLOSE
EOF
server2=$!

# Wait until both nodes are ready to accept client connections
wait $syncpid1 $syncpid2;

# Run client
timeout 4s "$clientprog" --blocking-initial-update 127.0.0.1:7401 > "$testname.out" <<'EOF'
SET foo initial
SET bar initial

!async
SET foo timeout1
SET foo timeout2
SET foo timeout3
SET foo timeout4
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

expected="OK
OK
error: Timeout
error: Timeout
error: Timeout
error: Timeout"

cmp "$testname.out" <(echo "$expected") || exit 99

# Clean up
rm "$testname.out"
