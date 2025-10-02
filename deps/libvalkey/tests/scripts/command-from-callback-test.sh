#!/bin/bash
#
# Simulate a 3 node cluster.
# - Node 1 will handle topology requests.
# - Node 2 and 3 are removed, which results in moved slots.
#
# The client is configured to resend commands for which no response is
# received, and this is done directly from the reply callback.
# This verifies correct handling of topology changes when commands are sent
# from a reply callback.
#
# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient_async_sequence}
testname=command-from-callback-test

# Sync process just waiting for server to be ready to accept connection.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid1=$!
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid2=$!
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid3=$!

# Start simulated valkey node #1
timeout 5s ./simulated-valkey.pl -p 7401 -d --sigcont $syncpid1 <<'EOF' &
# Initial slotmap update
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 6000, ["127.0.0.1", 7401, "nodeid1"]],[6001, 12000, ["127.0.0.1", 7402, "nodeid2"]],[12001, 16383, ["127.0.0.1", 7403, "nodeid3"]]]
EXPECT CLOSE

# After slot map update, this node is now handling node2's old slots.
EXPECT CONNECT
EXPECT ["GET", "fee"]
SEND "bee"
EXPECT CLOSE
EOF
server1=$!

# Start simulated valkey node #2
timeout 5s ./simulated-valkey.pl -p 7402 -d --sigcont $syncpid2 <<'EOF' &
EXPECT CONNECT
EXPECT ["GET", "fee"]
SEND -MOVED 8471 127.0.0.1:7401
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 12000, ["127.0.0.1", 7401, "nodeid1"]], [12001, 16383, ["127.0.0.1", 7402, "nodeid2"]]]

# After update, this node is now handling node3's old slots.
EXPECT ["GET", "foo"]
SEND "boo"
EXPECT ["SET", "foo", "done"]
SEND +OK
EXPECT CLOSE
EOF
server2=$!

# Start simulated valkey node #3
timeout 5s ./simulated-valkey.pl -p 7403 -d --sigcont $syncpid3 <<'EOF' &
EXPECT CONNECT
EXPECT ["GET", "foo"]
# Sleep to make sure the command is aborted when node is removed from the slot map.
SLEEP 1
SEND -ERR some error not reached
EXPECT CLOSE
EOF
server3=$!

# Wait until all nodes are ready to accept client connections
wait $syncpid1 $syncpid2 $syncpid3;

# Run client which resends failed commands in the reply callback
timeout 4s "$clientprog" --blocking-initial-update 127.0.0.1:7401 > "$testname.out" <<'EOF'
!resend
!async
GET foo
GET fee
!sync

SET foo done
EOF
clientexit=$?

# Wait for servers to exit
wait $server1; server1exit=$?
wait $server2; server2exit=$?
wait $server3; server3exit=$?

# Check exit statuses
if [ $server1exit -ne 0 ]; then
    echo "Simulated server #1 exited with status $server1exit"
    exit $server1exit
fi
if [ $server2exit -ne 0 ]; then
    echo "Simulated server #2 exited with status $server2exit"
    exit $server2exit
fi
if [ $server3exit -ne 0 ]; then
    echo "Simulated server #3 exited with status $server3exit"
    exit $server3exit
fi
if [ $clientexit -ne 0 ]; then
    echo "$clientprog exited with status $clientexit"
    exit $clientexit
fi

# Check the output from clusterclient.
expected="unknown error
resend 'GET foo'
bee
boo
OK"

# There's a race so there are three acceptable outcomes. The reply "bee" can
# come at any point except after "OK". Therefore, we sort before comparing.
diff -u <(echo "$expected" | sort) <(sort "$testname.out") || exit 99

# Clean up
rm "$testname.out"
