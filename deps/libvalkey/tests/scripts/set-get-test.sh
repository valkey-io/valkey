#!/bin/sh

# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient}
testname=set-get-test

# Sync process just waiting for server to be ready to accept connection.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid=$!

# Start simulated server
timeout 5s ./simulated-valkey.pl -p 7400 -d --sigcont $syncpid <<'EOF' &
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 16383, ["127.0.0.1", 7400, "nodeid123"]]]
EXPECT CLOSE
EXPECT CONNECT
EXPECT ["SET", "foo", "bar"]
SEND +OK
EXPECT ["GET", "foo"]
SEND "bar"
EXPECT CLOSE
EOF
server=$!

# Wait until server is ready to accept client connection
wait $syncpid;

# Run client
timeout 3s "$clientprog" 127.0.0.1:7400 > "$testname.out" <<'EOF'
SET foo bar
GET foo
EOF
clientexit=$?

# Wait for server to exit
wait $server; serverexit=$?

# Check exit statuses
if [ $serverexit -ne 0 ]; then
    echo "Simulated server exited with status $serverexit"
    exit $serverexit
fi
if [ $clientexit -ne 0 ]; then
    echo "$clientprog exited with status $clientexit"
    exit $clientexit
fi

# Check the output from clusterclient
printf 'OK\nbar\n' | cmp "$testname.out" - || exit 99

# Clean up
rm "$testname.out"
