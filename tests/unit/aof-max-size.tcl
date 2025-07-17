proc setup {{size 1}} {
    r set k v
    r config set aof-max-size $size
    r set k2 v2
}

proc cleanup {} {
    r config set aof-max-size 0
    r flushall
}

start_server {tags {"external:skip"}} {
    r config set auto-aof-rewrite-percentage 0 ; # disable auto-rewrite
    r config set appendonly yes ; # enable AOF

    set master_host [srv 0 host]
    set master_port [srv 0 port]

    test "Low aof-max-size starts AOF rewrite" {
        setup
        wait_for_log_messages 0 {"*Background append only file rewriting started*"} 0 100 10
        r set k3 v3
        cleanup
    }
}

