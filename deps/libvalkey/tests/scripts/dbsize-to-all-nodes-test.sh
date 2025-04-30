#!/bin/sh

# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient}
testname=dbsize-to-all-nodes-test

# Sync processes waiting for CONT signals.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid1=$!;
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid2=$!;

# Start simulated valkey node #1
timeout 5s ./simulated-valkey.pl -p 7403 -d --sigcont $syncpid1 <<'EOF' &
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 8383, ["127.0.0.1", 7403, "nodeid7403"]], [8384, 16383, ["127.0.0.1", 7404, "nodeid7404"]]]
EXPECT CLOSE
EXPECT CONNECT
EXPECT ["DBSIZE"]
SEND 11
EXPECT CLOSE
EOF
server1=$!

# Start simulated valkey node #2
timeout 5s ./simulated-valkey.pl -p 7404 -d --sigcont $syncpid2 <<'EOF' &
EXPECT CONNECT
EXPECT ["DBSIZE"]
SEND 88
EXPECT CLOSE
EOF
server2=$!

# Wait until both nodes are ready to accept client connections
wait $syncpid1 $syncpid2;

# Run client
timeout 3s "$clientprog" 127.0.0.1:7403 > "$testname.out" <<'EOF'
!all
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

# Check the output from clusterclient
printf '11\n88\n' | cmp "$testname.out" - || exit 99

# Clean up
rm "$testname.out"
