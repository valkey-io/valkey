#!/bin/bash
#
# Simulate a 2 node cluster where both nodes are shutdown after some
# initial commands. The client will attempt to reconnect to the nodes
# without success.
#
# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient_async_sequence}
testname=cluster-down-test

# Sync processes waiting for CONT signals.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid1=$!;
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid2=$!;

# Start simulated valkey node #1
timeout 5s ./simulated-valkey.pl -p 7401 -d --sigcont $syncpid1 <<'EOF' &
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 6000, ["127.0.0.1", 7401, "nodeid1"]],[6001, 16383, ["127.0.0.1", 7402, "nodeid2"]]]
EXPECT CLOSE

EXPECT CONNECT
EXPECT ["SET", "bar", "initial"]
SEND +OK
CLOSE
EOF
server1=$!

# Start simulated valkey node #2
timeout 5s ./simulated-valkey.pl -p 7402 -d --sigcont $syncpid2 <<'EOF' &
EXPECT CONNECT
EXPECT ["SET", "foo", "initial"]
SEND +OK
CLOSE
EOF
server2=$!

# Wait until both nodes are ready to accept client connections
wait $syncpid1 $syncpid2;

# Run client
timeout 4s "$clientprog" 127.0.0.1:7401 > "$testname.out" <<'EOF'
!async
SET foo initial
SET bar initial
!sync

# Wait a second to allow servers to shutdown
!sleep

!async
SET foo fail-1
SET bar fail-1
!sync

!async
SET foo fail-2
SET bar fail-2
!sync

!async
SET foo fail-3
SET bar fail-3
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

# Check the output from clusterclient
expected="OK
OK
error: Connection refused
error: Connection refused
error: Connection refused
error: Connection refused
error: Connection refused
error: Connection refused"

cmp "$testname.out" <(echo "$expected") || exit 99

# Clean up
rm "$testname.out"
