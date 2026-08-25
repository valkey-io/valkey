start_server {tags {"qos"}} {
    set qos_info [r info clients]
    set cluster_conns 0
    regexp {cluster_connections:(\d+)} $qos_info -> cluster_conns
    set prioritized_clients 0
    regexp {connected_clients_prioritized:(\d+)} $qos_info -> prioritized_clients
    set base_clients [lindex [r client list] 0]
    set total_conns [llength [split [r client list] "\n"]]
    if {$total_conns > 0} {
        set base_clients $total_conns
    } else {
        set base_clients 1
    }

    # Helper to get the IP address of the current test client as seen by the server.
    proc get_current_client_ip {} {
        set my_id [r client id]
        set client_list [r client list]
        foreach line [split $client_list "\n"] {
            if {[regexp "id=$my_id " $line]} {
                if {[regexp {addr=([^ ]+)} $line -> my_addr]} {
                    # my_addr is ip:port or [ip]:port
                    if {[string match "*\[*" $my_addr]} {
                        regexp {\[([^\]]+)\]} $my_addr -> my_ip
                    } else {
                        set my_ip [lindex [split $my_addr ":"] 0]
                    }
                    return $my_ip
                }
            }
        }
        error "Could not find current client IP"
    }

    proc get_current_client_ip_with_mask {} {
        set ip [get_current_client_ip]
        if {[string match "*:*" $ip]} {
            return "$ip/128"
        } else {
            return "$ip/32"
        }
    }

    # Helper to check if a client has expected QoS
    proc assert_client_qos {client_id expected_qos} {
        set client_list [r client list]
        set found 0
        foreach line [split $client_list "\n"] {
            if {$line == ""} continue
            if {[regexp "id=$client_id " $line]} {
                set found 1
                if {$expected_qos eq "prioritized"} {
                    assert {[regexp {flags=[^ ]*H} $line]}
                } else {
                    assert {![regexp {flags=[^ ]*H} $line]}
                }
                break
            }
        }
        assert_equal 1 $found "Client ID $client_id not found in client list"
    }

    # Save original configs for global restoration
    set global_old_maxclients [lindex [r config get maxclients] 1]
    set global_old_qos_reserved_min_clients [lindex [r config get qos-reserved-min-clients] 1]
    set global_old_qos_subnet_sources [lindex [r config get qos-subnet-sources] 1]

    set qos_test_script_err ""
    set qos_test_script_status [catch {

    test {CONFIG SET / GET qos-subnet-sources} {
        r config set qos-subnet-sources "127.0.0.1/32 10.0.0.0/8"
        assert_equal {127.0.0.1/32 10.0.0.0/8} [lindex [r config get qos-subnet-sources] 1]

        r config set qos-subnet-sources "::1/128,2001:db8::/32"
        assert_equal {::1/128,2001:db8::/32} [lindex [r config get qos-subnet-sources] 1]

        r config set qos-subnet-sources "127.0.0.1 ::1"
        assert_equal {127.0.0.1 ::1} [lindex [r config get qos-subnet-sources] 1]

        r config set qos-subnet-sources ""
        assert_equal {} [lindex [r config get qos-subnet-sources] 1]
    }

    test {CONFIG SET qos-subnet-sources invalid inputs} {
        catch {r config set qos-subnet-sources "127.0.0.1/99"} err
        assert_match "*Invalid IP address or CIDR subnet*" $err

        catch {r config set qos-subnet-sources "invalid/24"} err
        assert_match "*Invalid IP address or CIDR subnet*" $err
    }

    test {CONFIG SET / GET qos-reserved-min-clients} {
        r config set qos-reserved-min-clients 100
        assert_equal 100 [lindex [r config get qos-reserved-min-clients] 1]

        r config set qos-reserved-min-clients 200
        assert_equal 200 [lindex [r config get qos-reserved-min-clients] 1]

        catch {r config set qos-reserved-min-clients -1} err
        assert_match "*argument must be*" $err

        # Should fail if >= maxclients
        set cur_maxclients [lindex [r config get maxclients] 1]
        catch {r config set qos-reserved-min-clients $cur_maxclients} err
        assert_match "*must be less than maxclients*" $err
    }

    test {QoS reserved partition admission control and observability} {
        # Current active clients = 1 (r).
        # Set maxclients to 4 (allows 4 total connections).
        # Set qos-reserved-min-clients to 2.
        # Normal client threshold = 4 - 2 = 2 (r + 1 normal client).
        r config set maxclients 4
        r config set qos-reserved-min-clients 2
        set my_ip_mask [get_current_client_ip_with_mask]
        # Without qos-subnet-sources, reservation is inactive.
        r config set qos-subnet-sources ""

        # Connect 1 normal client (total normal = 2: r + c1)
        set c1 [valkey_deferring_client]
        $c1 client id
        set c1_id [$c1 read]

        # Enable QoS by specifying qos-subnet-sources for loopback
        r config set qos-subnet-sources $my_ip_mask

        # With QoS active, total clients = 2 (r + c1).
        # Normal client ceiling is maxclients - reserved = 4 - 2 = 2.
        # A new non-prioritized connection or when normal quota is full:
        # Since loopback is now prioritized, connections from loopback will be prioritized!
        # Connect prioritized client p1 (total = 3 < 4 maxclients) - succeeds!
        set p1 [valkey_deferring_client]
        $p1 client id
        set p1_id [$p1 read]
        $p1 ping
        assert_equal {PONG} [$p1 read]

        # Connect prioritized client p2 (total = 4 = maxclients) - succeeds!
        set p2 [valkey_deferring_client]
        $p2 client id
        set p2_id [$p2 read]
        $p2 ping
        assert_equal {PONG} [$p2 read]

        # Verify INFO clients metrics
        set info_clients [r info clients]
        assert_match "*connected_clients_prioritized:2*" $info_clients

        # Verify CLIENT LIST output contains 'H' flag for p1/p2 and not for c1 or r
        assert_client_qos $p1_id "prioritized"
        assert_client_qos $p2_id "prioritized"
        assert_client_qos $c1_id "normal"
        assert_client_qos [r client id] "normal"

        # Verify CLIENT INFO from prioritized client contains flags=...H...
        $p1 client info
        set p1_info [$p1 read]
        assert {[regexp {flags=[^ ]*H} $p1_info]}

        # Verify CLIENT LIST with FLAGS H and NOT-FLAGS H filters
        set h_list [r client list flags H]
        set not_h_list [r client list not-flags H]
        assert {[regexp "id=$p1_id " $h_list]}
        assert {[regexp "id=$p2_id " $h_list]}
        assert {![regexp "id=$c1_id " $h_list]}
        assert {![regexp "id=$p1_id " $not_h_list]}
        assert {![regexp "id=$p2_id " $not_h_list]}
        assert {[regexp "id=$c1_id " $not_h_list]}

        # Verify CLIENT KILL with FLAGS H kills only prioritized clients
        set killed [r client kill flags H]
        assert_equal 2 $killed
        assert_client_qos $c1_id "normal"

        # Close all to clean up
        catch {$c1 close}
        catch {$p1 close}
        catch {$p2 close}
    }

    test {QoS maxclients ceiling rejection with rejected_priority_connections stat} {
        r config set maxclients 3
        r config set qos-reserved-min-clients 1
        r config set qos-subnet-sources [get_current_client_ip_with_mask]

        # Active clients: r (1) + p1 (1) + p2 (1) = 3 (reaches maxclients)
        set p1 [valkey_deferring_client]
        $p1 ping
        assert_equal {PONG} [$p1 read]

        set p2 [valkey_deferring_client]
        $p2 ping
        assert_equal {PONG} [$p2 read]

        r config resetstat

        # 3rd prioritized client should fail because total reached maxclients (3)
        if {$::tls} {
            set expected_code "*I/O error*"
        } else {
            set expected_code "*max number of priority clients reached*"
        }
        catch {
            set p3 [valkey_deferring_client]
            $p3 ping
            $p3 read
        } err_p3
        assert_match $expected_code $err_p3

        # Verify INFO stats contains rejected_priority_connections:1
        set info_stats [r info stats]
        assert_match "*rejected_priority_connections:1*" $info_stats

        catch {$p1 close}
        catch {$p2 close}
    }

    test {QoS admission control with comma-separated qos-subnet-sources} {
        r config set maxclients 10
        r config set qos-reserved-min-clients 2
        
        # Test comma-separated list (with and without space)
        set client_ip_mask [get_current_client_ip_with_mask]
        r config set qos-subnet-sources "$client_ip_mask,1.1.1.1/32"
        
        set p1 [valkey_deferring_client]
        $p1 client id
        set p1_id [$p1 read]
        $p1 ping
        assert_equal {PONG} [$p1 read]
        
        assert_client_qos $p1_id "prioritized"

        catch {$p1 close}
        
        # Test mixed comma and space list
        r config set qos-subnet-sources "$client_ip_mask, 1.1.1.1/32"
        
        set p2 [valkey_deferring_client]
        $p2 client id
        set p2_id [$p2 read]
        $p2 ping
        assert_equal {PONG} [$p2 read]
        
        assert_client_qos $p2_id "prioritized"
        
        catch {$p2 close}
    }

    test {QoS admission control with raw IP qos-subnet-sources} {
        r config set maxclients 10
        r config set qos-reserved-min-clients 2
        r config set qos-subnet-sources "[get_current_client_ip] 1.1.1.1"
        
        set p1 [valkey_deferring_client]
        $p1 client id
        set p1_id [$p1 read]
        $p1 ping
        assert_equal {PONG} [$p1 read]
        
        assert_client_qos $p1_id "prioritized"

        catch {$p1 close}
    }

    } qos_test_script_err]

    # Restore global configs
    r config set maxclients $global_old_maxclients
    r config set qos-reserved-min-clients $global_old_qos_reserved_min_clients
    r config set qos-subnet-sources $global_old_qos_subnet_sources

    if {$qos_test_script_status != 0} {
        error $qos_test_script_err $::errorInfo
    }
}

