start_server {tags {"replication" "external:skip"}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    set primary_rd [valkey_deferring_client]

    start_server {} {
        set replica1 [srv 0 client]
        $replica1 replicaof $primary_host $primary_port
        wait_for_sync $replica1

        $primary set key [string repeat A [expr 1024*1024]]
        wait_for_ofs_sync $primary $replica1

        start_server {} {
            set replica2 [srv 0 client]
            set replica2_pid [srv 0 pid]

            # Arm both pause points so we control the handshake precisely.
            $replica2 debug repl-pause-before-psync 1
            $replica2 debug repl-pause-before-sync 1

            # Start replication. The replica will pause (SIGSTOP) right before
            # sending PSYNC, before the primary is busy.
            $replica2 replicaof $primary_host $primary_port

            # Wait until the replica process is actually stopped.
            wait_process_paused $replica2_pid

            # Put the primary into a BUSY state.
            $primary config set busy-reply-threshold 10
            $primary_rd eval {local a = 1 while true do a = a + 1 end} 0
            $primary_rd flush
            wait_for_condition 50 100 {
                [catch {$primary ping} e] == 1 && [string match {BUSY*} $e]
            } else {
                fail "Primary did not enter $busy_kind BUSY"
            }

            # Resume the replica: it sends PSYNC and receives -BUSY.
            resume_process $replica2_pid
            wait_for_log_messages 0 {"*Retrying with SYNC*"} 0 10 1000

            # Wait until the replica process is actually stopped.
            wait_process_paused $replica2_pid

            # Take the primary out of the BUSY state.
            $primary script kill

            # Resume the replica: it sends SYNC
            resume_process $replica2_pid
            wait_for_sync $replica2

            puts "primary offset: [status $primary master_repl_offset]"
            puts "replica1 offset: [status $replica1 master_repl_offset]"
            puts "replica2 offset: [status $replica2 master_repl_offset]"

            $primary set key [string repeat A [expr 1024*1024]]

            puts "after a set"
            puts "primary offset: [status $primary master_repl_offset]"
            puts "replica1 offset: [status $replica1 master_repl_offset]"
            puts "replica2 offset: [status $replica2 master_repl_offset]"
        }
    }
    $primary_rd close
}
