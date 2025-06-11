#!/bin/bash
#
# To verify the clients behaviour in a cluster scaledown scenario.
# The testcase will send commands, all targeting hash slot 12182, while removing
# the cluster members handling this slot.
#
# Test steps:
# 1. At start there are 3 known cluster nodes; 'nodeid1', 'nodeid2' and 'nodeid3'.
#    'nodeid3' is not started in the testcase to simulate that the initial slot
#    owner, i.e. 'nodeid3', is removed before the client has connected to it.
# 2. The first command is sent.
#    Due to the connect failure to 'nodeid3' the slotmap will be updated before
#    successfully sending the command to the new slot owner 'nodeid2'.
# 3. The slot owner 'nodeid2' is now removed, which results in a closed connection.
# 4. The second command is sent.
#    The closed connection will result in a scheduled slotmap update and a
#    returned failure code.
# 5. The third command is sent.
#    Since previous command failed this will perform a slotmap update and then
#    successfully send the command to the new slot owner 'nodeid1'.
#
# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient}
testname=cluster-scale-down-test

# Sync processes waiting for CONT signals.
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid1=$!;
perl -we 'use sigtrap "handler", sub{exit}, "CONT"; sleep 1; die "timeout"' &
syncpid2=$!;

# Start simulated valkey node #1
timeout 5s ./simulated-valkey.pl -p 7401 -d --sigcont $syncpid1 <<'EOF' &
# Initial slotmap.
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 5000, ["127.0.0.1", 7401, "nodeid1"]],[5001, 10000, ["127.0.0.1", 7402, "nodeid2"]],[10001, 16383, ["127.0.0.1", 7403, "nodeid3"]]]
EXPECT CLOSE

# The command "GET {foo}1" is first sent to the slot owner nodeid3, but since this
# node does not exist the failed connection attempt will trigger a slotmap update.
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 8000, ["127.0.0.1", 7401, "nodeid1"]],[8001, 16383, ["127.0.0.1", 7402, "nodeid2"]]]
EXPECT CLOSE

# The send failure of "GET {foo}2" schedules a slotmap update, which is
# performed when (and just before) the next command "GET {foo}3" is sent.
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 16383, ["127.0.0.1", 7401, "nodeid1"]]]
EXPECT CLOSE
EXPECT CONNECT
EXPECT ["GET", "{foo}3"]
SEND "bar3"
EXPECT CLOSE
EOF
server1=$!

# Start simulated valkey node #2
timeout 5s ./simulated-valkey.pl -p 7402 -d --sigcont $syncpid2 <<'EOF' &
EXPECT CONNECT
EXPECT ["GET", "{foo}1"]
SEND "bar1"
# Forced close. The next command "GET {foo}2" will fail.
EOF
server2=$!

# Wait until both nodes are ready to accept client connections
wait $syncpid1 $syncpid2;

# Run client
timeout 3s "$clientprog" 127.0.0.1:7401 > "$testname.out" <<'EOF'
GET {foo}1
GET {foo}2
GET {foo}3
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
# Client sends the command 'GET {foo}2' just after nodeid2 closes its socket.
expected1="bar1
error: Server closed the connection
bar3"

# Client sends the command 'GET {foo}2' just before nodeid2 closes its socket.
expected2="bar1
error: Connection reset by peer
bar3"

diff -u "$testname.out" <(echo "$expected1") || \
    diff -u "$testname.out" <(echo "$expected2") || \
    exit 99

# Clean up
rm "$testname.out"
