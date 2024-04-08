start_cluster 1 1 {tags {external:skip cluster}} {
    set primary_id 0
    set replica1_id 1

    set primary [Rn $primary_id]
    set replica [Rn $replica1_id]

    test "Sharded pubsub publish behavior on a primary" {
        assert_equal 0 [$primary spublish ch1 "hello"]
    }

    test "Sharded pubsub publish behavior on a replica" {
        assert_error "*MOVED*" {$replica spublish ch1 "hello"}
    }


    test "Sharded pubsub publish behavior within multi/exec" {
        $primary MULTI
        $primary SPUBLISH ch1 "hello"
        $primary EXEC
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

    test "Sharded pubsub publish behavior within multi/exec on replica" {
        $replica MULTI
        catch {[$replica SPUBLISH foo "hello"]} err
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
}
