start_server {tags {"qos"}} {
    set qos_info [r info clients]
    set cluster_conns 0
    regexp {cluster_connections:(\d+)} $qos_info -> cluster_conns
    set normal_clients 0
    regexp {connected_clients_normal:(\d+)} $qos_info -> normal_clients
    set prioritized_clients 0
    regexp {connected_clients_prioritized:(\d+)} $qos_info -> prioritized_clients
    set base_clients [expr {$normal_clients + $prioritized_clients}]

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
                assert {[regexp "qos=$expected_qos" $line]}
                break
            }
        }
        assert_equal 1 $found "Client ID $client_id not found in client list"
    }

    # Save original configs for global restoration
    set global_old_maxclients [lindex [r config get maxclients] 1]
    set global_old_priority_maxclients [lindex [r config get priority-maxclients] 1]
    set global_old_priority_net_sources [lindex [r config get priority-net-sources] 1]
    set global_old_prioritize_unixsocket [lindex [r config get prioritize-unixsocket] 1]

    set qos_test_script_err ""
    set qos_test_script_status [catch {

    test {CONFIG SET / GET priority-net-sources} {
        r config set priority-net-sources "127.0.0.1/32 10.0.0.0/8"
        assert_equal {127.0.0.1/32 10.0.0.0/8} [lindex [r config get priority-net-sources] 1]

        r config set priority-net-sources "::1/128,2001:db8::/32"
        assert_equal {::1/128,2001:db8::/32} [lindex [r config get priority-net-sources] 1]

        r config set priority-net-sources "127.0.0.1 ::1"
        assert_equal {127.0.0.1 ::1} [lindex [r config get priority-net-sources] 1]

        r config set priority-net-sources ""
        assert_equal {} [lindex [r config get priority-net-sources] 1]
    }

    test {CONFIG SET priority-net-sources invalid inputs} {
        catch {r config set priority-net-sources "127.0.0.1/99"} err
        assert_match "*Invalid IP address or CIDR subnet*" $err

        catch {r config set priority-net-sources "invalid/24"} err
        assert_match "*Invalid IP address or CIDR subnet*" $err
    }

    test {CONFIG SET / GET priority-maxclients} {
        r config set priority-maxclients 5000
        assert_equal 5000 [lindex [r config get priority-maxclients] 1]

        catch {r config set priority-maxclients -1} err
        assert_match "*argument must be*" $err
    }

    test {CONFIG SET / GET prioritize-unixsocket} {
        r config set prioritize-unixsocket yes
        assert_equal yes [lindex [r config get prioritize-unixsocket] 1]

        r config set prioritize-unixsocket no
        assert_equal no [lindex [r config get prioritize-unixsocket] 1]
    }

    test {QoS admission control and observability} {
        # Set maxclients to 3 + cluster_conns, priority-maxclients to 5
        r config set maxclients [expr {$base_clients + 2 + $cluster_conns}]
        r config set priority-maxclients 5
        r config set priority-net-sources ""

        # Connect 2 normal clients
        set c1 [valkey_deferring_client]
        $c1 client id
        set c1_id [$c1 read]
        set c2 [valkey_deferring_client]
        $c2 client id
        set c2_id [$c2 read]

        # 3rd normal client should fail (total normal clients = 4: r + c1 + c2 + c3)
        if {$::tls} {
            set expected_code "*I/O error*"
        } else {
            set expected_code "*max number of clients*reached*"
        }
        catch {
            set c3 [valkey_deferring_client]
            $c3 ping
            $c3 read
        } err3
        assert_match $expected_code $err3

        # Now configure loopback as prioritized source
        # We allow both 127.0.0.1 and ::1 for loopback
        r config set priority-net-sources [get_current_client_ip_with_mask]

        # Now connect a prioritized client - it should succeed!
        set p1 [valkey_deferring_client]
        $p1 client id
        set p1_id [$p1 read]
        $p1 ping
        assert_equal {PONG} [$p1 read]

        # Connect another prioritized client - should succeed
        set p2 [valkey_deferring_client]
        $p2 client id
        set p2_id [$p2 read]
        $p2 ping
        assert_equal {PONG} [$p2 read]

        # Verify INFO clients metrics
        set info_clients [r info clients]
        assert_match "*connected_clients_prioritized:[expr {$prioritized_clients + 2}]*" $info_clients
        assert_match "*connected_clients_normal:[expr {$normal_clients + 2}]*" $info_clients
        assert_match "*priority_maxclients:5*" $info_clients

        # Verify CLIENT LIST output contains qos=prioritized for p1/p2 and qos=normal for others
        assert_client_qos $p1_id "prioritized"
        assert_client_qos $p2_id "prioritized"
        assert_client_qos $c1_id "normal"
        assert_client_qos $c2_id "normal"
        assert_client_qos [r client id] "normal"

        set client_list [r client list]
        set qos_prioritized_count 0
        set qos_normal_count 0
        foreach line [split $client_list "\n"] {
            if {$line == ""} continue
            if {[regexp {qos=prioritized} $line]} {
                incr qos_prioritized_count
            }
            if {[regexp {qos=normal} $line]} {
                incr qos_normal_count
            }
        }
        assert_equal [expr {$prioritized_clients + 2}] $qos_prioritized_count
        assert_equal [expr {$normal_clients + 2}] $qos_normal_count

        # Close all to clean up
        catch {$c1 close}
        catch {$c2 close}
        catch {$p1 close}
        catch {$p2 close}
    }

    test {QoS priority-maxclients limit rejection} {
        r config set maxclients [expr {$base_clients + 2 + $cluster_conns}]
        r config set priority-maxclients 2
        r config set priority-net-sources [get_current_client_ip_with_mask]

        # Connect 2 normal clients to saturate maxclients (total normal = 3: r + c1 + c2)
        set c1 [valkey_deferring_client]
        set c2 [valkey_deferring_client]

        # Connect 2 prioritized clients (they should succeed)
        set p1 [valkey_deferring_client]
        $p1 ping
        assert_equal {PONG} [$p1 read]

        set p2 [valkey_deferring_client]
        $p2 ping
        assert_equal {PONG} [$p2 read]

        r config resetstat

        # 3rd prioritized client should fail because priority-maxclients is 2
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

        catch {$c1 close}
        catch {$c2 close}
        catch {$p1 close}
        catch {$p2 close}
    }

    test {QoS admission control with comma-separated priority-net-sources} {
        r config set maxclients [expr {$base_clients + 2 + $cluster_conns}]
        r config set priority-maxclients 5
        
        # Test comma-separated list (with and without space)
        set client_ip_mask [get_current_client_ip_with_mask]
        r config set priority-net-sources "$client_ip_mask,1.1.1.1/32"
        
        set p1 [valkey_deferring_client]
        $p1 client id
        set p1_id [$p1 read]
        $p1 ping
        assert_equal {PONG} [$p1 read]
        
        assert_client_qos $p1_id "prioritized"

        catch {$p1 close}
        
        # Test mixed comma and space list
        r config set priority-net-sources "$client_ip_mask, 1.1.1.1/32"
        
        set p2 [valkey_deferring_client]
        $p2 client id
        set p2_id [$p2 read]
        $p2 ping
        assert_equal {PONG} [$p2 read]
        
        assert_client_qos $p2_id "prioritized"
        
        catch {$p2 close}
    }

    test {QoS admission control with raw IP priority-net-sources} {
        r config set maxclients [expr {$base_clients + 2 + $cluster_conns}]
        r config set priority-maxclients 5
        r config set priority-net-sources "[get_current_client_ip] 1.1.1.1"
        
        set p1 [valkey_deferring_client]
        $p1 client id
        set p1_id [$p1 read]
        $p1 ping
        assert_equal {PONG} [$p1 read]
        
        assert_client_qos $p1_id "prioritized"

        catch {$p1 close}
    }

    test {QoS prioritize-unixsocket admission control} {
        if {$::external} {
            skip "unixsocket tests not supported on external server"
        }
        r config set maxclients [expr {$base_clients + 2 + $cluster_conns}]
        r config set priority-maxclients 2
        r config set prioritize-unixsocket yes
        r config set priority-net-sources ""


        # Connect 2 normal TCP clients (total normal = 3: r + c1 + c2)
        set c1 [valkey_deferring_client]
        set c2 [valkey_deferring_client]

        # Verify 3rd TCP client fails
        if {$::tls} {
            set expected_code "*I/O error*"
        } else {
            set expected_code "*max number of clients*reached*"
        }
        catch {
            set c3 [valkey_deferring_client]
            $c3 ping
            $c3 read
        } err3
        assert_match $expected_code $err3

        # Connect via unix socket using valkey-cli - should succeed
        set socket_path [srv unixsocket]
        set res [exec $::VALKEY_CLI_BIN -s $socket_path ping]
        assert_equal "PONG" $res

        # Now disable prioritize-unixsocket
        r config set prioritize-unixsocket no

        # Connect via unix socket should now fail
        catch {
            exec $::VALKEY_CLI_BIN -s $socket_path ping
        } err_cli
        if {![string match "*max number of clients*reached*" $err_cli] &&
            ![string match "*write on pipe with no readers*" $err_cli]} {
            assert_failed "Expected max clients reached error or SIGPIPE, but got: '$err_cli'" ""
        }

        catch {$c1 close}
        catch {$c2 close}
    }

    test {CONFIG SET priority-maxclients huge value should fail} {

        r config set prioritize-unixsocket yes

        set old_maxclients [lindex [r config get maxclients] 1]
        
        # Try to set priority-maxclients to a value that exceeds ulimit
        # It should fail and NOT modify maxclients
        set hard_limit [exec sh -c "ulimit -H -n"]
        if {$hard_limit eq "unlimited"} {
            set target 1000000000
        } else {
            set target [expr $hard_limit + 10000]
        }
        
        catch {r config set priority-maxclients $target} err
        assert_match "*operating system is not able to handle*" $err
        
        # Verify maxclients was not modified
        assert_equal $old_maxclients [lindex [r config get maxclients] 1]
    }

    test {QoS prioritize-unixsocket has no effect on TCP when unixsocket is not configured} {
        if {$::external} {
            if {[lindex [r config get unixsocket] 1] ne ""} {
                skip "unixsocket is configured on external server, cannot test 'not configured' behavior"
            }
        }
        start_server {omit {unixsocket}} {
            set qos_info [r info clients]
            set cluster_conns 0
            regexp {cluster_connections:(\d+)} $qos_info -> cluster_conns
            set normal_clients 0
            regexp {connected_clients_normal:(\d+)} $qos_info -> normal_clients
            set prioritized_clients 0
            regexp {connected_clients_prioritized:(\d+)} $qos_info -> prioritized_clients
            set base_clients [expr {$normal_clients + $prioritized_clients}]

            set old_maxclients [lindex [r config get maxclients] 1]
            set old_prioritize_unixsocket [lindex [r config get prioritize-unixsocket] 1]
            set old_priority_maxclients [lindex [r config get priority-maxclients] 1]
            set old_priority_net_sources [lindex [r config get priority-net-sources] 1]

            # Set maxclients to 2 + base_clients + cluster_conns
            r config set maxclients [expr {$base_clients + 2 + $cluster_conns}]
            r config set priority-maxclients 5
            r config set priority-net-sources ""

            # Enable prioritize-unixsocket dynamically
            r config set prioritize-unixsocket yes

            # Connect 2 normal TCP clients
            set c1 [valkey_deferring_client]
            set c2 [valkey_deferring_client]

            # 3rd TCP client should fail
            if {$::tls} {
                set expected_code "*I/O error*"
            } else {
                set expected_code "*max number of clients*reached*"
            }
            catch {
                set c3 [valkey_deferring_client]
                $c3 ping
                $c3 read
            } err3
            assert_match $expected_code $err3

            catch {$c1 close}
            catch {$c2 close}
            catch {$c3 close}

            # Restore configs
            r config set maxclients $old_maxclients
            r config set prioritize-unixsocket $old_prioritize_unixsocket
            r config set priority-maxclients $old_priority_maxclients
            r config set priority-net-sources $old_priority_net_sources
        }
    }

    } qos_test_script_err]

    # Restore global configs
    r config set maxclients $global_old_maxclients
    r config set priority-maxclients $global_old_priority_maxclients
    r config set priority-net-sources $global_old_priority_net_sources
    r config set prioritize-unixsocket $global_old_prioritize_unixsocket

    if {$qos_test_script_status != 0} {
        error $qos_test_script_err $::errorInfo
    }
}

