start_server {tags {"qos"}} {
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

    # Helper to match connection rejection error across plain, cluster, and TLS modes.
    # In cluster mode, the error string is "-ERR max number of clients + cluster connections reached".
    # In standalone mode, the error string is "-ERR max number of clients reached".
    # In TLS mode, connection drops before handshake completing, resulting in I/O error.
    proc get_maxclients_error_pattern {} {
        if {$::tls} {
            return "*I/O error*"
        } else {
            return "*max number of clients*reached*"
        }
    }

    proc can_bind_loopback_ip {ip} {
        if {[catch {
            set s [socket -myaddr $ip [srv 0 "host"] [srv 0 "port"]]
            close $s
        }] == 0} {
            return 1
        }
        return 0
    }

    proc valkey_from_ip {myaddr server port {defer 0}} {
        if {$::tls} {
            package require tls
            ::tls::init \
                -cafile "$::tlsdir/ca.crt" \
                -certfile "$::tlsdir/client.crt" \
                -keyfile "$::tlsdir/client.key"
            set fd [::tls::socket -myaddr $myaddr $server $port]
        } else {
            set fd [socket -myaddr $myaddr $server $port]
        }
        fconfigure $fd -translation binary
        set id [incr ::valkey::id]
        set ::valkey::fd($id) $fd
        set ::valkey::addr($id) [list $server $port]
        set ::valkey::blocking($id) 1
        set ::valkey::deferred($id) $defer
        set ::valkey::readraw($id) 0
        set ::valkey::reconnect($id) 0
        set ::valkey::curr_argv($id) 0
        set ::valkey::testing_resp3($id) 0
        set ::valkey::tls($id) $::tls
        ::valkey::valkey_reset_state $id
        interp alias {} ::valkey::valkeyHandle$id {} ::valkey::__dispatch__ $id
    }

    proc valkey_deferring_client_from_ip {myaddr} {
        set client [valkey_from_ip $myaddr [srv 0 "host"] [srv 0 "port"] 1]
        if {!$::singledb} {
            $client select 9
            $client read
        } else {
            $client echo goodday
            $client read
        }
        return $client
    }

    # Save original configs for global restoration
    set global_old_maxclients [lindex [r config get maxclients] 1]
    set global_old_maxclients_reserved [lindex [r config get maxclients-reserved] 1]
    set global_old_priority_subnets [lindex [r config get priority-subnets] 1]

    set qos_test_script_err ""
    set qos_test_script_status [catch {

    test {CONFIG SET / GET priority-subnets} {
        r config set priority-subnets "127.0.0.1/32 10.0.0.0/8"
        assert_equal {127.0.0.1/32 10.0.0.0/8} [lindex [r config get priority-subnets] 1]

        r config set priority-subnets "::1/128,2001:db8::/32"
        assert_equal {::1/128,2001:db8::/32} [lindex [r config get priority-subnets] 1]

        r config set priority-subnets "127.0.0.1 ::1"
        assert_equal {127.0.0.1 ::1} [lindex [r config get priority-subnets] 1]

        r config set priority-subnets ""
        assert_equal {} [lindex [r config get priority-subnets] 1]
    }

    test {CONFIG SET priority-subnets invalid inputs} {
        catch {r config set priority-subnets "127.0.0.1/99"} err
        assert_match "*Invalid IP address or CIDR subnet*" $err

        catch {r config set priority-subnets "invalid/24"} err
        assert_match "*Invalid IP address or CIDR subnet*" $err
    }

    test {CONFIG SET / GET maxclients-reserved} {
        r config set maxclients-reserved 100
        assert_equal 100 [lindex [r config get maxclients-reserved] 1]

        r config set maxclients-reserved 200
        assert_equal 200 [lindex [r config get maxclients-reserved] 1]

        catch {r config set maxclients-reserved -1} err
        assert_match "*argument must be*" $err

        # Values >= maxclients can be set without order dependency
        set cur_maxclients [lindex [r config get maxclients] 1]
        r config set maxclients-reserved [expr {$cur_maxclients + 100}]
        assert_equal [expr {$cur_maxclients + 100}] [lindex [r config get maxclients-reserved] 1]

        # maxclients can also be changed freely regardless of maxclients-reserved
        r config set maxclients 10
        assert_equal 10 [lindex [r config get maxclients] 1]

        r config set maxclients $cur_maxclients
        r config set maxclients-reserved 0
    }

    test {Admission control with maxclients-reserved and priority-subnets} {
        # Current active clients = 1 (r).
        # Set maxclients to 4 (allows 4 total connections).
        # Set maxclients-reserved to 2.
        # Normal client threshold = 4 - 2 = 2 (r + 1 normal client).
        r config set maxclients 4
        r config set maxclients-reserved 2
        set my_ip_mask [get_current_client_ip_with_mask]
        # Without priority-subnets, reservation is inactive.
        r config set priority-subnets ""

        # Connect 1 normal client (total normal = 2: r + c1)
        set c1 [valkey_deferring_client]
        $c1 client id
        set c1_id [$c1 read]

        # Enable priority by specifying priority-subnets for loopback.
        # Dynamic re-classification immediately promotes all existing clients matching
        # the subnet (r and c1) to prioritized status.
        r config set priority-subnets $my_ip_mask

        # With reservation active, total clients = 2 (r + c1).
        # Normal client ceiling is maxclients - reserved = 4 - 2 = 2.
        # Since loopback is now prioritized, connections from loopback will be prioritized.
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

        # Verify INFO clients metrics: all 4 clients (r, c1, p1, p2) are prioritized
        set info_clients [r info clients]
        assert_match "*connected_clients_prioritized:4*" $info_clients

        # Close p1 and verify active prioritized count decrements to 3
        $p1 close
        wait_for_condition 50 100 {
            [string match "*connected_clients_prioritized:3*" [r info clients]]
        } else {
            fail "connected_clients_prioritized did not decrement to 3 after closing p1"
        }

        # Close p2 and verify active prioritized count decrements to 2
        $p2 close
        wait_for_condition 50 100 {
            [string match "*connected_clients_prioritized:2*" [r info clients]]
        } else {
            fail "connected_clients_prioritized did not decrement to 2 after closing p2"
        }

        # Close c1 and verify active prioritized count decrements to 1 (only r remains)
        catch {$c1 close}
        wait_for_condition 50 100 {
            [string match "*connected_clients_prioritized:1*" [r info clients]]
        } else {
            fail "connected_clients_prioritized did not decrement to 1 after closing c1"
        }

        # Clearing priority-subnets dynamically demotes r to normal
        r config set priority-subnets ""
        set info_clients [r info clients]
        assert_match "*connected_clients_prioritized:0*" $info_clients

        r config set maxclients-reserved 0
    }

    test {Admission control when maxclients-reserved >= maxclients} {
        r config set maxclients 3
        r config set maxclients-reserved 10
        # Set subnet to an unrelated IP so loopback is normal (non-prioritized)
        r config set priority-subnets "192.0.2.1/32"

        # Active clients: r (1). Since reserved (10) >= maxclients (3),
        # normal limit is clamped to 0. Any new normal connection must be rejected.
        set expected_code [get_maxclients_error_pattern]
        catch {
            set c_normal [valkey_deferring_client]
            $c_normal ping
            $c_normal read
        } err_normal
        assert_match $expected_code $err_normal

        # Now set priority-subnets to current client IP (making loopback prioritized)
        r config set priority-subnets [get_current_client_ip_with_mask]

        # Prioritized client should connect successfully (active: r + p1 = 2 <= 3)
        set p1 [valkey_deferring_client]
        $p1 ping
        assert_equal {PONG} [$p1 read]

        # Second prioritized client connects successfully (active: r + p1 + p2 = 3 <= 3)
        set p2 [valkey_deferring_client]
        $p2 ping
        assert_equal {PONG} [$p2 read]

        # Third prioritized client rejected at maxclients ceiling (3)
        set expected_p_code [get_maxclients_error_pattern]
        catch {
            set p3 [valkey_deferring_client]
            $p3 ping
            $p3 read
        } err_p3
        assert_match $expected_p_code $err_p3

        catch {$p1 close}
        catch {$p2 close}
        r config set maxclients-reserved 0
        r config set priority-subnets ""
    }

    test {Maxclients ceiling rejection with rejected_connections_prioritized stat} {
        r config set maxclients 3
        r config set maxclients-reserved 1
        r config set priority-subnets [get_current_client_ip_with_mask]

        # Active clients: r (1) + p1 (1) + p2 (1) = 3 (reaches maxclients)
        set p1 [valkey_deferring_client]
        $p1 ping
        assert_equal {PONG} [$p1 read]

        set p2 [valkey_deferring_client]
        $p2 ping
        assert_equal {PONG} [$p2 read]

        r config resetstat

        # 3rd prioritized client should fail because total reached maxclients (3)
        set expected_code [get_maxclients_error_pattern]
        catch {
            set p3 [valkey_deferring_client]
            $p3 ping
            $p3 read
        } err_p3
        assert_match $expected_code $err_p3

        # Verify INFO stats contains rejected_connections:1 and rejected_connections_prioritized:1
        set info_stats [r info stats]
        assert_match "*rejected_connections:1*" $info_stats
        assert_match "*rejected_connections_prioritized:1*" $info_stats

        catch {$p1 close}
        catch {$p2 close}
        r config set maxclients-reserved 0
        r config set priority-subnets ""
    }

    test {Admission control with comma-separated priority-subnets} {
        r config set maxclients 10
        r config set maxclients-reserved 2
        
        # Test comma-separated list (with and without space)
        set client_ip_mask [get_current_client_ip_with_mask]
        r config set priority-subnets "$client_ip_mask,1.1.1.1/32"
        
        set p1 [valkey_deferring_client]
        $p1 client id
        set p1_id [$p1 read]
        $p1 ping
        assert_equal {PONG} [$p1 read]
        
        # r (1) + p1 (1) = 2 prioritized clients due to dynamic re-classification
        assert_match "*connected_clients_prioritized:2*" [r info clients]

        catch {$p1 close}
        
        # Test mixed comma and space list
        r config set priority-subnets "$client_ip_mask, 1.1.1.1/32"
        
        set p2 [valkey_deferring_client]
        $p2 client id
        set p2_id [$p2 read]
        $p2 ping
        assert_equal {PONG} [$p2 read]
        
        assert_match "*connected_clients_prioritized:2*" [r info clients]
        
        catch {$p2 close}
        r config set maxclients-reserved 0
        r config set priority-subnets ""
    }

    test {Admission control with raw IP priority-subnets} {
        r config set maxclients 10
        r config set maxclients-reserved 2
        r config set priority-subnets "[get_current_client_ip] 1.1.1.1"
        
        set p1 [valkey_deferring_client]
        $p1 client id
        set p1_id [$p1 read]
        $p1 ping
        assert_equal {PONG} [$p1 read]
        
        # r (1) + p1 (1) = 2 prioritized clients
        assert_match "*connected_clients_prioritized:2*" [r info clients]

        catch {$p1 close}
        r config set maxclients-reserved 0
        r config set priority-subnets ""
    }

    test {Priority clients exceeding reservation do not starve normal clients} {
        r config set maxclients 10
        r config set maxclients-reserved 5

        # If system supports binding to 127.0.0.2, test multi-subnet priority separation
        if {[can_bind_loopback_ip "127.0.0.2"]} {
            r config set priority-subnets "127.0.0.2/32"

            set prio_clients {}
            for {set i 0} {$i < 6} {incr i} {
                set p [valkey_deferring_client_from_ip "127.0.0.2"]
                $p ping
                assert_equal {PONG} [$p read]
                lappend prio_clients $p
            }

            # Active: r (normal from 127.0.0.1, 1) + 6 prioritized (from 127.0.0.2) = 7 total.
            assert_match "*connected_clients_prioritized:6*" [r info clients]

            # Normal limit is 10 - 5 = 5.
            # Current normal clients = 1 (r). Normal quota has 4 slots available.
            # Normal clients from 127.0.0.1 can connect despite priority clients exceeding reserved.
            set c1 [valkey_deferring_client]
            $c1 ping
            assert_equal {PONG} [$c1 read]

            set c2 [valkey_deferring_client]
            $c2 ping
            assert_equal {PONG} [$c2 read]

            # Normal clients = 3 (r + c1 + c2), prioritized = 6, total = 9 <= 10
            assert_match "*connected_clients_prioritized:6*" [r info clients]

            catch {$c1 close}
            catch {$c2 close}
            foreach p $prio_clients {
                catch {$p close}
            }
        }
        r config set maxclients-reserved 0
        r config set priority-subnets ""
    }

    test {Dynamic Re-Classification of connected clients on priority-subnets change} {
        if {[can_bind_loopback_ip "127.0.0.2"]} {
            r config set maxclients 10
            r config set maxclients-reserved 3
            r config set priority-subnets ""

            # Step 1: Initial state without priority subnets
            set c_normal [valkey_deferring_client]
            $c_normal ping
            assert_equal {PONG} [$c_normal read]

            set c_alt [valkey_deferring_client_from_ip "127.0.0.2"]
            $c_alt ping
            assert_equal {PONG} [$c_alt read]

            # r, c_normal (127.0.0.1) and c_alt (127.0.0.2) are all normal clients
            assert_match "*connected_clients_prioritized:0*" [r info clients]

            # Step 2: Configure priority-subnets to 127.0.0.2/32
            # Dynamic re-classification immediately promotes c_alt to prioritized;
            # r and c_normal remain normal.
            r config set priority-subnets "127.0.0.2/32"
            assert_match "*connected_clients_prioritized:1*" [r info clients]

            # Step 3: Switch priority-subnets to 127.0.0.1/32
            # Dynamic re-classification immediately promotes r and c_normal to prioritized,
            # and demotes c_alt back to normal.
            r config set priority-subnets "127.0.0.1/32"
            assert_match "*connected_clients_prioritized:2*" [r info clients]

            # Step 4: Include both subnets in priority-subnets
            # Dynamic re-classification promotes all 3 clients to prioritized.
            r config set priority-subnets "127.0.0.1/32 127.0.0.2/32"
            assert_match "*connected_clients_prioritized:3*" [r info clients]

            # Step 5: Disconnect c_alt; prioritized count decrements to 2
            $c_alt close
            wait_for_condition 50 100 {
                [string match "*connected_clients_prioritized:2*" [r info clients]]
            } else {
                fail "connected_clients_prioritized did not decrement to 2 after closing c_alt"
            }

            # Step 6: Clear priority-subnets; remaining clients demoted to normal
            r config set priority-subnets ""
            assert_match "*connected_clients_prioritized:0*" [r info clients]

            # Step 7: Disconnect c_normal; ensures no underflow desync
            $c_normal close
            wait_for_condition 50 100 {
                [string match "*connected_clients_prioritized:0*" [r info clients]]
            } else {
                fail "connected_clients_prioritized did not remain 0 after closing c_normal"
            }

            r config set maxclients-reserved 0
        }
    }

    test {Normal clients rejected when normal quota exhausted, while priority clients still connect} {
        r config set maxclients 6
        r config set maxclients-reserved 3

        # Loopback is normal
        r config set priority-subnets "192.0.2.1/32"

        # Normal limit is 6 - 3 = 3.
        # Currently 1 normal client (r).
        set c1 [valkey_deferring_client]
        $c1 ping
        assert_equal {PONG} [$c1 read]

        set c2 [valkey_deferring_client]
        $c2 ping
        assert_equal {PONG} [$c2 read]

        # Now normal clients = 3 (r + c1 + c2) == normal_limit (3).
        # Total clients = 3 < maxclients (6).
        # A new normal client must be rejected because normal quota is full.
        set expected_code [get_maxclients_error_pattern]
        catch {
            set c3 [valkey_deferring_client]
            $c3 ping
            $c3 read
        } err_c3
        assert_match $expected_code $err_c3

        # Switch loopback to priority: prioritized clients can still connect into reserved slots
        r config set priority-subnets [get_current_client_ip_with_mask]

        set p1 [valkey_deferring_client]
        $p1 ping
        assert_equal {PONG} [$p1 read]

        set p2 [valkey_deferring_client]
        $p2 ping
        assert_equal {PONG} [$p2 read]

        set p3 [valkey_deferring_client]
        $p3 ping
        assert_equal {PONG} [$p3 read]

        # Total is now 6 == maxclients. A 4th priority client should fail at global ceiling.
        catch {
            set p4 [valkey_deferring_client]
            $p4 ping
            $p4 read
        } err_p4
        assert_match $expected_code $err_p4

        catch {$c1 close}
        catch {$c2 close}
        catch {$p1 close}
        catch {$p2 close}
        catch {$p3 close}
        r config set maxclients-reserved 0
        r config set priority-subnets ""
    }

    test {Normal client rejected when total reaches maxclients even if normal quota has headroom} {
        r config set maxclients 5
        r config set maxclients-reserved 3

        # Normal limit = 5 - 3 = 2.
        # Active normal: r (1 < 2). Normal quota has 1 headroom slot.
        # Fill total capacity to maxclients (5) using priority clients.
        r config set priority-subnets [get_current_client_ip_with_mask]

        set prio_clients {}
        for {set i 0} {$i < 4} {incr i} {
            set p [valkey_deferring_client]
            $p ping
            assert_equal {PONG} [$p read]
            lappend prio_clients $p
        }

        # Total clients = 1 (r) + 4 (prioritized) = 5 == maxclients.
        # Now switch loopback to normal.
        r config set priority-subnets "192.0.2.1/32"

        set expected_code [get_maxclients_error_pattern]
        catch {
            set c1 [valkey_deferring_client]
            $c1 ping
            $c1 read
        } err_c1
        assert_match $expected_code $err_c1

        # Priority client should also be rejected because total == maxclients
        r config set priority-subnets [get_current_client_ip_with_mask]
        catch {
            set p5 [valkey_deferring_client]
            $p5 ping
            $p5 read
        } err_p5
        assert_match $expected_code $err_p5

        foreach p $prio_clients {
            catch {$p close}
        }
        r config set maxclients-reserved 0
        r config set priority-subnets ""
    }

    test {Dynamic reconfiguration of maxclients-reserved and priority-subnets} {
        r config set maxclients 5
        r config set maxclients-reserved 0
        r config set priority-subnets ""

        # Connect 3 normal clients without QoS (active: r + 3 = 4 < 5)
        set c1 [valkey_deferring_client]
        $c1 ping
        assert_equal {PONG} [$c1 read]

        set c2 [valkey_deferring_client]
        $c2 ping
        assert_equal {PONG} [$c2 read]

        set c3 [valkey_deferring_client]
        $c3 ping
        assert_equal {PONG} [$c3 read]

        # Dynamically set maxclients-reserved 3 (normal_limit = 5 - 3 = 2)
        # Existing 4 normal clients (r + c1..c3) exceed normal_limit (2).
        # Existing clients must continue working without disruption.
        r config set priority-subnets "192.0.2.1/32"
        r config set maxclients-reserved 3

        $c1 ping
        assert_equal {PONG} [$c1 read]
        $c2 ping
        assert_equal {PONG} [$c2 read]
        $c3 ping
        assert_equal {PONG} [$c3 read]

        # New normal connection must be rejected because normal_clients (4) >= normal_limit (2)
        set expected_code [get_maxclients_error_pattern]
        catch {
            set c4 [valkey_deferring_client]
            $c4 ping
            $c4 read
        } err_c4
        assert_match $expected_code $err_c4

        # Dynamically change maxclients-reserved to 0: reservation disabled
        r config set maxclients-reserved 0

        # Now normal client can connect since total (4) < maxclients (5)
        set c5 [valkey_deferring_client]
        $c5 ping
        assert_equal {PONG} [$c5 read]

        catch {$c1 close}
        catch {$c2 close}
        catch {$c3 close}
        catch {$c5 close}
        r config set maxclients-reserved 0
        r config set priority-subnets ""
    }

    test {Zero maxclients-reserved or empty priority-subnets disables reservation} {
        r config set maxclients 4

        # Case 1: priority-subnets configured, but maxclients-reserved is 0
        r config set maxclients-reserved 0
        r config set priority-subnets "192.0.2.1/32"

        # Active: r (1). Connect 3 normal clients up to maxclients (4)
        set c1 [valkey_deferring_client]
        $c1 ping
        assert_equal {PONG} [$c1 read]
        set c2 [valkey_deferring_client]
        $c2 ping
        assert_equal {PONG} [$c2 read]
        set c3 [valkey_deferring_client]
        $c3 ping
        assert_equal {PONG} [$c3 read]

        # 4th normal connection fails at maxclients
        set expected_code [get_maxclients_error_pattern]
        catch {
            set c4 [valkey_deferring_client]
            $c4 ping
            $c4 read
        } err_c4
        assert_match $expected_code $err_c4

        catch {$c1 close}
        catch {$c2 close}
        catch {$c3 close}

        # Case 2: maxclients-reserved > 0, but priority-subnets is empty
        r config set maxclients-reserved 2
        r config set priority-subnets ""

        set c1 [valkey_deferring_client]
        $c1 ping
        assert_equal {PONG} [$c1 read]
        set c2 [valkey_deferring_client]
        $c2 ping
        assert_equal {PONG} [$c2 read]
        set c3 [valkey_deferring_client]
        $c3 ping
        assert_equal {PONG} [$c3 read]

        catch {
            set c4 [valkey_deferring_client]
            $c4 ping
            $c4 read
        } err_c4
        assert_match $expected_code $err_c4

        catch {$c1 close}
        catch {$c2 close}
        catch {$c3 close}
        r config set maxclients-reserved 0
        r config set priority-subnets ""
    }

    } qos_test_script_err]

    # Restore global configs
    r config set maxclients $global_old_maxclients
    r config set maxclients-reserved $global_old_maxclients_reserved
    r config set priority-subnets $global_old_priority_subnets

    if {$qos_test_script_status != 0} {
        error $qos_test_script_err $::errorInfo
    }
}

start_server {tags {"qos external:skip"} overrides {priority-subnets {"127.0.0.0/8,::1/128"} maxclients 5 maxclients-reserved 2}} {
    test {Priority subnets configured on startup enable priority admission} {
        assert_match "*connected_clients_prioritized:1*" [r info clients]
        set c1 [valkey_client]
        assert_match "*connected_clients_prioritized:2*" [r info clients]
        $c1 close
        wait_for_condition 50 100 {
            [string match "*connected_clients_prioritized:1*" [r info clients]]
        } else {
            fail "connected_clients_prioritized did not decrement to 1 after closing c1"
        }
    }
}


