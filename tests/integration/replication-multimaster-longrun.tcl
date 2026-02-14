start_server {tags {"repl external:skip"}} {
start_server {overrides {save {}}} {
start_server {overrides {save {}}} {
    set nodeA [srv -2 client]
    set nodeA_host [srv -2 host]
    set nodeA_port [srv -2 port]
    set nodeB [srv -1 client]
    set nodeB_host [srv -1 host]
    set nodeB_port [srv -1 port]
    set nodeC [srv 0 client]
    set nodeC_host [srv 0 host]
    set nodeC_port [srv 0 port]

    foreach n [list $nodeA $nodeB $nodeC] {
        $n config set active-replica yes
        $n config set multi-master yes
        $n config set replica-read-only no
    }

    test {3-node chain setup for long-run MM validation} {
        $nodeB replicaof add $nodeA_host $nodeA_port
        $nodeC replicaof add $nodeB_host $nodeB_port
        wait_for_condition 150 100 {
            [s -1 master_link_status] eq {up} &&
            [s 0 master_link_status] eq {up}
        } else {
            fail "3-node chain was not established"
        }
    }

    test {3-node long-run divergence stability} {
        set total_writes 600
        for {set i 1} {$i <= $total_writes} {incr i} {
            $nodeB set mm:longrun:hot "$i"
            if {($i % 50) == 0} {after 2}
        }

        $nodeB set mm:longrun:barrier done
        wait_for_condition 400 100 {
            ([$nodeA get mm:longrun:hot] eq "$total_writes") &&
            ([$nodeB get mm:longrun:hot] eq "$total_writes") &&
            ([$nodeC get mm:longrun:hot] eq "$total_writes") &&
            ([$nodeA get mm:longrun:barrier] eq {done}) &&
            ([$nodeB get mm:longrun:barrier] eq {done}) &&
            ([$nodeC get mm:longrun:barrier] eq {done})
        } else {
            fail "chain did not reconverge after long-run writes"
        }
    }

    test {3-node dedupe-window pressure stays bounded} {
        set pressure_writes 10500
        for {set i 1} {$i <= $pressure_writes} {incr i} {
            $nodeB set "mm:dedupe:$i" "$i"
            if {($i % 250) == 0} {after 2}
        }

        $nodeB set mm:dedupe:barrier done
        wait_for_condition 600 100 {
            ([$nodeA get mm:dedupe:barrier] eq {done}) &&
            ([$nodeC get mm:dedupe:barrier] eq {done})
        } else {
            fail "chain did not recover after dedupe-window pressure"
        }

        set da [s -2 rreplay_dedupe_entries]
        set db [s -1 rreplay_dedupe_entries]
        set dc [s 0 rreplay_dedupe_entries]
        assert {$da <= 10000}
        assert {$db <= 10000}
        assert {$dc <= 10000}
        set dmax [expr {max($da, max($db, $dc))}]
        assert {$dmax >= 2000}
    }
}
}
}
