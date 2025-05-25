start_cluster 1 1 {tags {external:skip cluster}} {
    set primary_id 0
    set replica1_id 1

    set primary [Rn $primary_id]
    set replica [Rn $replica1_id]

    test "Sharded pubsub publish behavior within multi/exec" {
        foreach {node} {primary replica} {
            set node [set $node]
            $node MULTI
            $node SPUBLISH ch1 "hello"
            $node EXEC
        }
    }

    test "Sharded pubsub within multi/exec with cross slot operation" {
        $primary MULTI
        $primary SPUBLISH ch1 "hello"
        $primary GET foo
        catch {[$primary EXEC]} err
        assert_match {CROSSSLOT*} $err
    }

    test "Sharded pubsub publish behavior within multi/exec with read operation on primary" {
        $primary MULTI
        $primary SPUBLISH foo "hello"
        $primary GET foo
        $primary EXEC
    } {0 {}}

    test "Sharded pubsub publish behavior within multi/exec with read operation on replica" {
        $replica MULTI
        $replica SPUBLISH foo "hello"
        catch {[$replica GET foo]} err
        assert_match {MOVED*} $err
        catch {[$replica EXEC]} err
        assert_match {EXECABORT*} $err
    }

    test "Sharded pubsub publish behavior within multi/exec with write operation on primary" {
        $primary MULTI
        $primary SPUBLISH foo "hello"
        $primary SET foo bar
        $primary EXEC
    } {0 OK}

    test "Sharded pubsub publish behavior within multi/exec with write operation on replica" {
        $replica MULTI
        $replica SPUBLISH foo "hello"
        catch {[$replica SET foo bar]} err
        assert_match {MOVED*} $err
        catch {[$replica EXEC]} err
        assert_match {EXECABORT*} $err
    }
    
    test "SSUBSCRIBE client killed during transaction" {
        # Create two clients
        set rd1 [valkey_deferring_client $primary_id]
        set rd2 [valkey_deferring_client $primary_id]
        
        # Get client 1 ID
        $rd1 client id
        set rd1_id [$rd1 read]
        puts "rd1_id $rd1_id"
        # Client1 subscribes to a shard channel
        $rd1 ssubscribe channel0
        
        # Wait for the subscription to be acknowledged
        assert_equal {ssubscribe channel0 1} [$rd1 read]
        # Client2 starts a transaction, sets a key
        $rd2 multi
        set multi_result [$rd2 read]
        assert_equal {OK} $multi_result

        $rd2 set k v
        set result [$rd2 read]
        assert_equal {QUEUED} $result
        # Kill client1 inside client2's transaction
        $rd2 client kill id $rd1_id
        set result [$rd2 read]
        assert_equal {QUEUED} $result

        # Execute the transaction
        $rd2 exec
        set exec_result [$rd2 read]
        assert_equal {OK 1} $exec_result "Transaction execution should return OK and kill count"
    }
}