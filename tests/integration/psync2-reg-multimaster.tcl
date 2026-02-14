start_server {tags {"psync2 external:skip"} overrides {save {} active-replica yes multi-master yes replica-read-only no client-output-buffer-limit {replica 200mb 10mb 999999}}} {
start_server {overrides {save {} active-replica yes multi-master yes replica-read-only no client-output-buffer-limit {replica 200mb 10mb 999999}}} {
start_server {overrides {save {} active-replica yes multi-master yes replica-read-only no client-output-buffer-limit {replica 200mb 10mb 999999}}} {
    for {set j 0} {$j < 3} {incr j} {
        set R($j) [srv [expr 0-$j] client]
        set RH($j) [srv [expr 0-$j] host]
        set RP($j) [srv [expr 0-$j] port]
    }

    test {PSYNC2 multi-master setup} {
        $R(1) replicaof add $RH(0) $RP(0)
        $R(2) replicaof add $RH(0) $RP(0)

        $R(0) set mm:seed ok
        wait_for_condition 100 100 {
            [$R(1) get mm:seed] eq {ok} &&
            [$R(2) get mm:seed] eq {ok}
        } else {
            fail "multimaster setup did not converge"
        }
    }

    test {PSYNC2 multi-master survives link churn under writes} {
        for {set i 0} {$i < 80} {incr i} {
            $R(0) incr mm:counter
            if {($i % 8) == 0} {
                catch {$R(1) client kill type master}
            }
            if {($i % 11) == 0} {
                catch {$R(2) client kill type master}
            }
            after 15
        }

        wait_for_condition 200 100 {
            ([$R(0) debug digest] eq [$R(1) debug digest]) &&
            ([$R(1) debug digest] eq [$R(2) debug digest])
        } else {
            fail "dataset diverged after churn"
        }
    }
}}}
