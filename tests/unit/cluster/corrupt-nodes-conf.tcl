# A server that panics while loading a corrupt nodes.conf must still log a
# complete crash report. The report builds INFO, which reads cluster state,
# so every field it reads must be set before clusterLoadConfig() runs.

# Start a cluster-mode server on the nodes.conf content `conf_lines` (a list of
# lines), wait for the end of its crash report, and check the report.
proc check_corrupt_nodes_conf_crash_report {name conf_lines} {
    test $name {
        set dir [file normalize [tmpdir corrupt-nodes-conf]]
        write_file [file join $dir nodes.conf] [join $conf_lines "\n"]

        set logfile [file join $dir server.log]
        set pid [exec $::VALKEY_SERVER_BIN --port 0 \
                     --unixsocket [file join $dir server.sock] \
                     --cluster-enabled yes \
                     --dir $dir \
                     --cluster-config-file nodes.conf \
                     --crash-memcheck-enabled no \
                     --logfile $logfile &]
        # Wait for the end of the report instead of the exit: with core dumps
        # enabled, the exit after a panic can take a long time.
        wait_for_condition 100 100 {
            [file exists $logfile] &&
            [string match "*BUG REPORT END*" [read_file $logfile]]
        } else {
            catch {exec kill -9 $pid}
            fail "no end of crash report in $logfile"
        }
        catch {exec kill -9 $pid}
        set log [read_file $logfile]
        assert_match "*Unrecoverable error: corrupted cluster config file*" $log
        if {[regexp {crashed by signal[^\n]*} $log crash]} {
            set where unknown
            regexp {EIP:\n[^(]*\(([^+)]+)} $log -> where
            fail "the crash report itself $crash, in $where"
        }
        assert_match "*------ INFO OUTPUT ------*" $log
        assert_match "*------ CLUSTER NODES OUTPUT ------*" $log
        assert_match "*------ CLIENT LIST OUTPUT ------*" $log
        assert_match "*------ MODULES INFO OUTPUT ------*" $log
        assert_match "*------ CONFIG DEBUG OUTPUT ------*" $log
        assert_match "*BUG REPORT END*" $log
    }
}

tags {external:skip cluster} {
    check_corrupt_nodes_conf_crash_report \
        "Crash report on a corrupt nodes.conf completes without a second crash" \
        [list \
            "this is not a nodes.conf line" \
            "vars currentEpoch 1 lastVoteEpoch 0"]

    # A replicaof cycle (A -> B -> A) is refused by the same fmterr path, so
    # the report must be complete there too. Both nodes are written as
    # replicas, and the first line is myself.
    set cycle_a [string repeat a 40]
    set cycle_b [string repeat b 40]
    check_corrupt_nodes_conf_crash_report \
        "Crash report on a replicaof cycle in nodes.conf completes without a second crash" \
        [list \
            "$cycle_a 127.0.0.1:30001@40001 myself,slave $cycle_b 0 0 1 connected" \
            "$cycle_b 127.0.0.1:30002@40002 slave $cycle_a 0 0 2 connected" \
            "vars currentEpoch 2 lastVoteEpoch 0"]
}
