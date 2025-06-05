#!/bin/sh

# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient}
testname=moved-redirect-using-cluster-nodes-test

# Sync processes waiting for CONT signals.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid1=$!;
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid2=$!;

# Start simulated valkey node #1
timeout 5s ./simulated-valkey.pl -p 7400 -d --sigcont $syncpid1 <<'EOF' &
EXPECT CONNECT
EXPECT ["CLUSTER", "NODES"]
SEND "e495df74528a0946d03bb931cbfc6c9edb975448 127.0.0.1:7400@17400 myself,master - 0 1677668806000 1 connected 0-16383\n"
EXPECT CLOSE

EXPECT CONNECT
EXPECT ["GET", "foo"]
SEND -MOVED 12182 127.0.0.1:7401

EXPECT ["CLUSTER", "NODES"]
SEND "69cf08ee7feac361d98e2ea762c3e39852280045 127.0.0.1:7401@17401 master - 0 1677668806272 2 connected 0-16383\n"

EXPECT CLOSE
EOF
server1=$!

# Start simulated valkey node #2
timeout 5s ./simulated-valkey.pl -p 7401 -d --sigcont $syncpid2 <<'EOF' &
EXPECT CONNECT
EXPECT ["GET", "foo"]
SEND "bar"
EXPECT CLOSE
EOF
server2=$!

# Wait until both nodes are ready to accept client connections
wait $syncpid1 $syncpid2;

# Run client
timeout 3s "$clientprog" --use-cluster-nodes 127.0.0.1:7400 > "$testname.out" <<'EOF'
GET foo
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
echo 'bar' | cmp "$testname.out" - || exit 99

# Clean up
rm "$testname.out"
