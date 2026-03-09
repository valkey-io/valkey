proc restart_killed_instances {} {
    foreach type {valkey sentinel} {
        foreach_${type}_id id {
            if {[get_instance_attrib $type $id pid] == -1} {
                puts -nonewline "$type/$id "
                flush stdout
                restart_instance $type $id
            }
        }
    }
}

proc verify_sentinel_connect_sentinels {id} {
    foreach sentinel [S $id SENTINEL SENTINELS mymaster] {
        if {[string match "*disconnected*" [dict get $sentinel flags]]} {
            return 0
        }
    }
    return 1
}

proc verify_sentinel_auto_discovery {} {
    set sentinels [llength $::sentinel_instances]
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [dict get [S $id SENTINEL PRIMARY mymaster] num-other-sentinels] == ($sentinels-1)
        } else {
            fail "At least some sentinel can't detect some other sentinel"
        }
        wait_for_condition 1000 50 {
            [verify_sentinel_connect_sentinels $id] == 1
        } else {
            fail "At least some sentinel can't connect to other sentinel"
        }
    }
}

proc verify_sentinel_connect_replicas {id} {
    foreach replica [S $id SENTINEL REPLICAS mymaster] {
        if {[string match "*disconnected*" [dict get $replica flags]]} {
            return 0
        }
    }
    return 1
}

proc wait_for_sentinels_connect_servers { {is_connect 1} } {
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [string match "*disconnected*" [dict get [S $id SENTINEL PRIMARY mymaster] flags]] != $is_connect
        } else {
            fail "At least some sentinel can't connect to master"
        }

        wait_for_condition 1000 50 {
            [verify_sentinel_connect_replicas $id] == $is_connect
        } else {
            fail "At least some sentinel can't connect to replica"
        }
    }
}

proc configure_sentinel_user_acl {user password {allow_failover 1}} {
    foreach_valkey_id id {
        R $id ACL SETUSER $user >$password +subscribe +publish [expr {$allow_failover ? "+failover" : "-failover"}] +script|kill +ping +info +multi +slaveof +config +client +exec &__sentinel__:hello ON
        # Ensure default user cannot be used for failover
        R $id ACL SETUSER default -failover -slaveof
    }
    foreach_sentinel_id id {
        S $id SENTINEL SET mymaster auth-user $user
        S $id SENTINEL SET mymaster auth-pass $password
        S $id SENTINEL FLUSHCONFIG
    }
    foreach_valkey_id id {
        R $id CLIENT KILL USER default SKIPME yes
    }
    wait_for_sentinels_connect_servers
}

proc reset_sentinel_user_acl {user} {
    foreach_sentinel_id id {
        S $id SENTINEL SET mymaster auth-user ""
        S $id SENTINEL SET mymaster auth-pass ""
        S $id SENTINEL FLUSHCONFIG
    }
    foreach_valkey_id id {
        R $id ACL SETUSER default +failover +slaveof
        R $id ACL DELUSER $user
        R $id CONFIG REWRITE
    }
    wait_for_sentinels_connect_servers
}

