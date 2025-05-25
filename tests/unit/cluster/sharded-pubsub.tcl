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
        
        # Get client 1 ID
        $rd1 client id
        set rd1_id [$rd1 read]
        # Client1 subscribes to a shard channel
        $rd1 ssubscribe channel0
        
        # Wait for the subscription to be acknowledged
        assert_equal {ssubscribe channel0 1} [$rd1 read]

        # Client2 starts a transaction
        assert_equal {OK} [$primary multi]

        # sets a key so that its slot will be set to the slot of that key.
        assert_equal {QUEUED} [$primary set k v]
        # Kill client1 inside client2's transaction
        assert_equal {QUEUED} [$primary client kill id $rd1_id]
    
        # Execute the transaction
        assert_equal {OK 1} [$primary exec] "Transaction execution should return OK and kill count"
    }
}