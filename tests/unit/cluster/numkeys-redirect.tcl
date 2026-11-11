# Commands whose keys are found by genericGetKeys() (a getkeys proc that reads
# the key count with atoi()) must not be redirected for a key count the command
# itself rejects: every node should give the same reply.

start_cluster 2 0 {tags {external:skip cluster}} {

    test "A malformed numkeys gets the same reply on the owner and on a non-owner" {
        wait_for_cluster_state ok

        set key foo
        # Find the node that serves the key, the other one is a non-owner.
        if {[catch {R 0 get $key} e]} {
            assert_match {MOVED *} $e
            set owner 1
            set other 0
        } else {
            set owner 0
            set other 1
        }
        foreach nk {1foo { 1} +1 1.0 4294967297} {
            foreach cmd [list \
                [list sintercard $nk $key] \
                [list zunion $nk $key] \
                [list zinterstore $key $nk $key] \
                [list lmpop $nk $key left] \
                [list zmpop $nk $key min] \
                [list eval {return 1} $nk $key] \
            ] {
                catch {R $owner {*}$cmd} owner_reply
                catch {R $other {*}$cmd} other_reply
                assert_equal $other_reply $owner_reply
            }
        }
    }
}
