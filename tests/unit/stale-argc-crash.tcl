start_server {tags {"regression"} overrides {io-threads 4 events-per-io-thread 0}} {

    test "Stale argc with DONT_PARSE should not crash" {
        set host [srv 0 host]
        set port [srv 0 port]

        set fd [socket $host $port]
        fconfigure $fd -translation binary -buffering full

        # Get client ID.
        puts -nonewline $fd "CLIENT ID\r\n"
        flush $fd
        after 100
        fconfigure $fd -blocking 0
        set resp [gets $fd]
        fconfigure $fd -blocking 1
        set cid [string trim [string range $resp 1 end]]

        # Send PING + incomplete SET.
        # IO thread parses PING, SET is incomplete → stale argc after consumeCommandQueue.
        puts -nonewline $fd "PING\r\n*3\r\n\$3\r\nSET\r\n\$1\r\na\r\n\$10\r\nhello"
        flush $fd
        after 100
        fconfigure $fd -blocking 0
        read $fd
        fconfigure $fd -blocking 1

        # Simulate an event that marks the client for closure.
        r DEBUG SET-CLOSE-AFTER-REPLY $cid
        after 100

        # Send more data → DONT_PARSE → stale argc > 0 → pending_command → crash.
        puts -nonewline $fd "world\r\n"
        flush $fd
        after 300

        assert_equal [r PING] PONG
        catch {close $fd}
    }
}
