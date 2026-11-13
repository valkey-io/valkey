start_server {tags {"repl external:skip"}} {
    set primary [srv 0 client]
    start_server {} {
        set replica [srv 0 client]
        $replica replicaof [srv -1 host] [srv -1 port]
        wait_for_sync $replica

        test {XTRIM that only frees an empty stream node keeps the replica layout in sync} {
            # CONFIG SET is not replicated, so set the node size on both.
            $primary config set stream-node-max-entries 2
            $replica config set stream-node-max-entries 2
            $primary xadd s 1-0 f v
            $primary xadd s 2-0 f v
            $primary xdel s 2-0
            # Removes 1-0 and leaves the node holding only the deleted 2-0.
            assert_equal 1 [$primary xtrim s minid 2-0]
            # Frees that node on the primary, but removes no entry.
            assert_equal 0 [$primary xtrim s minid 3-0]
            $primary xadd s 3-0 f v
            wait_for_ofs_sync $primary $replica
            assert_equal [$primary xlen s] [$replica xlen s]
            assert_equal [dict get [$primary xinfo stream s] radix-tree-keys] \
                         [dict get [$replica xinfo stream s] radix-tree-keys]
        }
    }
}
