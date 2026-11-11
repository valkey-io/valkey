# Commands whose keys are found by genericGetKeys() must not be
# redirected for a key count the command itself rejects.

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
        # Each command's own error for a count that isn't an integer, and
        # for one that parses but exceeds the number of arguments.
        set not_int_err {*value is not an integer or out of range*}
        set nk_err {*numkeys should be greater than 0*}
        set too_many_err {*Number of keys can't be greater than number of args*}
        set syntax_err {*syntax error*}
        foreach nk {1foo { 1} +1 1.0 4294967297} {
            foreach {cmd bad_err big_err} [list \
                [list sintercard $nk $key] $nk_err $too_many_err \
                [list zunion $nk $key] $not_int_err $syntax_err \
                [list zinterstore $key $nk $key] $not_int_err $syntax_err \
                [list lmpop $nk $key left] $nk_err $syntax_err \
                [list zmpop $nk $key min] $nk_err $syntax_err \
                [list eval {return 1} $nk $key] $not_int_err $too_many_err \
            ] {
                set expected [expr {$nk eq "4294967297" ? $big_err : $bad_err}]
                assert_error $expected {R $owner {*}$cmd}
                assert_error $expected {R $other {*}$cmd}
            }
        }
    }
}
