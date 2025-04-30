#!/bin/sh

# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient}
testname=ask-redirect-test

# Sync processes waiting for CONT signals.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid1=$!;
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid2=$!;

# Start simulated valkey node #1
timeout 5s ./simulated-valkey.pl -p 7401 -d --sigcont $syncpid1 <<'EOF' &
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 16383, ["127.0.0.1", 7401, "nodeid123"]]]
EXPECT CLOSE
EXPECT CONNECT
EXPECT ["GET", "foo"]
SEND -ASK 12182 127.0.0.1:7402
# A second redirect is needed to test reuse of the new cluster node
EXPECT ["GET", "foo"]
SEND -ASK 12182 127.0.0.1:7402
EXPECT CLOSE
EOF
server1=$!

# Start simulated valkey node #2
timeout 5s ./simulated-valkey.pl -p 7402 -d --sigcont $syncpid2 <<'EOF' &
EXPECT CONNECT
EXPECT ["ASKING"]
SEND +OK
EXPECT ["GET", "foo"]
SEND "bar"
EXPECT ["ASKING"]
SEND +OK
EXPECT ["GET", "foo"]
SEND "bar"
EXPECT CLOSE
EOF
server2=$!

# Wait until both nodes are ready to accept client connections
wait $syncpid1 $syncpid2;

# Run client
timeout 3s "$clientprog" 127.0.0.1:7401 > "$testname.out" <<'EOF'
GET foo
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
printf 'bar\nbar\n' | cmp "$testname.out" - || exit 99

# Clean up
rm "$testname.out"
