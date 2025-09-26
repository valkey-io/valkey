start_cluster 3 0 {tags {external:skip cluster shutdown}} {
    test "Test shutdown safe is working" {
        assert_error {ERR Errors trying to SHUTDOWN*} {R 0 shutdown safe}
        assert_error {ERR Errors trying to SHUTDOWN*} {R 1 shutdown safe}
        assert_error {ERR Errors trying to SHUTDOWN*} {R 2 shutdown safe}

        catch {R 0 shutdown safe force}
        wait_for_condition 1000 50 {
            [CI 1 cluster_state] eq {fail} &&
            [CI 2 cluster_state] eq {fail}
        } else {
            fail "Cluster doesn't fail"
        }
    }
}
