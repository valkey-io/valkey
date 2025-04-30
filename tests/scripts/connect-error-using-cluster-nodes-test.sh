#!/bin/sh
#
# Connect to a Valkey node which have no knowledge of a cluster.
#
# The client is configured to use the CLUSTER NODES command,
# which will receive a reply without slot information or other nodes.
# This kind of reply is returned from a node configured as a cluster node,
# but has not yet been included in a cluster.
#
# The client will return an error and error string.
#
# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient}
testname=connect-error-using-cluster-nodes-test

# Sync process just waiting for server to be ready to accept connection.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid=$!

# Start simulated server
timeout 5s ./simulated-valkey.pl -p 7400 -d --sigcont $syncpid <<'EOF' &
EXPECT CONNECT
EXPECT ["CLUSTER", "NODES"]
SEND "653876bce37106406581ddc05d8629357a223a7e :30001@40001 myself,master - 0 0 0 connected\n"
EXPECT CLOSE
EOF
server=$!

# Wait until server is ready to accept client connection
wait $syncpid;

# Run client and use CLUSTER NODES to get topology
timeout 3s "$clientprog" --use-cluster-nodes 127.0.0.1:7400 > "$testname.out"
clientexit=$?

# Wait for server to exit
wait $server; serverexit=$?

# Check exit status on server.
if [ $serverexit -ne 0 ]; then
    echo "Simulated server exited with status $serverexit"
    exit $serverexit
fi

# Check exit status on client, which SHOULD fail.
if [ $clientexit -ne 2 ]; then
    echo "$clientprog exited with status $clientexit"
    exit $clientexit
fi

# Check the output from clusterclient
printf 'Connect error: No slot information\n' | diff -u - "$testname.out" || exit 99

# Clean up
rm "$testname.out"
