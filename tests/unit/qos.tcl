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

        # Verify INFO clients metrics: 2 clients (p1, p2) admitted as prioritized;
        # r and c1 were admitted before priority-subnets was configured, so they remain normal.
        set info_clients [r info clients]
        assert_match "*connected_priority_clients:2*" $info_clients

        # Close p1 and verify active prioritized count decrements to 1
        $p1 close
        wait_for_condition 50 100 {
            [string match "*connected_priority_clients:1*" [r info clients]]
        } else {
            fail "connected_priority_clients did not decrement to 1 after closing p1"
        }

        # Close p2 and verify active prioritized count decrements to 0
        $p2 close
        wait_for_condition 50 100 {
            [string match "*connected_priority_clients:0*" [r info clients]]
        } else {
            fail "connected_priority_clients did not decrement to 0 after closing p2"
        }

        # Close normal client c1
        catch {$c1 close}

        # Clearing priority-subnets dynamically does not affect already-zero prioritized counter
        r config set priority-subnets ""
        set info_clients [r info clients]
        assert_match "*connected_priority_clients:0*" $info_clients

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

    test {Maxclients ceiling rejection with rejected_priority_connections stat} {
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

        # Verify INFO stats contains rejected_connections:1 and rejected_priority_connections:1
        set info_stats [r info stats]
        assert_match "*rejected_connections:1*" $info_stats
        assert_match "*rejected_priority_connections:1*" $info_stats

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
        
        # p1 (1) is admitted as prioritized (r was admitted before config change, so remains normal)
        assert_match "*connected_priority_clients:1*" [r info clients]

        catch {$p1 close}
        
        # Test mixed comma and space list
        r config set priority-subnets "$client_ip_mask, 1.1.1.1/32"
        
        set p2 [valkey_deferring_client]
        $p2 client id
        set p2_id [$p2 read]
        $p2 ping
        assert_equal {PONG} [$p2 read]
        
        assert_match "*connected_priority_clients:1*" [r info clients]
        
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
        
        # p1 (1) is admitted as prioritized
        assert_match "*connected_priority_clients:1*" [r info clients]

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
            assert_match "*connected_priority_clients:6*" [r info clients]

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
            assert_match "*connected_priority_clients:6*" [r info clients]

            catch {$c1 close}
            catch {$c2 close}
            foreach p $prio_clients {
                catch {$p close}
            }
        }
        r config set maxclients-reserved 0
        r config set priority-subnets ""
    }

    test {Admission-time priority enforcement on priority-subnets change} {
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
            assert_match "*connected_priority_clients:0*" [r info clients]

            # Step 2: Configure priority-subnets to 127.0.0.2/32
            # Admission-time policy: existing connections are not reclassified.
            r config set priority-subnets "127.0.0.2/32"
            assert_match "*connected_priority_clients:0*" [r info clients]

            # Step 3: A new connection from 127.0.0.2 is admitted as prioritized
            set c_prio [valkey_deferring_client_from_ip "127.0.0.2"]
            $c_prio ping
            assert_equal {PONG} [$c_prio read]
            assert_match "*connected_priority_clients:1*" [r info clients]

            # Step 4: A new connection from 127.0.0.1 is admitted as normal
            set c_norm2 [valkey_deferring_client]
            $c_norm2 ping
            assert_equal {PONG} [$c_norm2 read]
            assert_match "*connected_priority_clients:1*" [r info clients]

            # Step 5: Switch priority-subnets to 127.0.0.1/32
            # Existing clients retain their priority status
            r config set priority-subnets "127.0.0.1/32"
            assert_match "*connected_priority_clients:1*" [r info clients]

            # A new connection from 127.0.0.1 is admitted as prioritized
            set c_prio2 [valkey_deferring_client]
            $c_prio2 ping
            assert_equal {PONG} [$c_prio2 read]
            assert_match "*connected_priority_clients:2*" [r info clients]

            # Close c_prio; prioritized count decrements to 1
            $c_prio close
            wait_for_condition 50 100 {
                [string match "*connected_priority_clients:1*" [r info clients]]
            } else {
                fail "connected_priority_clients did not decrement to 1 after closing c_prio"
            }

            # Step 6: Clear priority-subnets; existing prioritized client c_prio2 is not demoted
            r config set priority-subnets ""
            assert_match "*connected_priority_clients:1*" [r info clients]

            # Step 7: Close c_prio2; ensures count decrements to 0 without underflow
            $c_prio2 close
            wait_for_condition 50 100 {
                [string match "*connected_priority_clients:0*" [r info clients]]
            } else {
                fail "connected_priority_clients did not reach 0 after closing c_prio2"
            }

            $c_normal close
            $c_alt close
            $c_norm2 close
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
        assert_match "*connected_priority_clients:1*" [r info clients]
        set c1 [valkey_client]
        assert_match "*connected_priority_clients:2*" [r info clients]
        $c1 close
        wait_for_condition 50 100 {
            [string match "*connected_priority_clients:1*" [r info clients]]
        } else {
            fail "connected_priority_clients did not decrement to 1 after closing c1"
        }
    }
}

start_server {tags {"qos external:skip"}} {
    test {CONFIG REWRITE and reload persists priority-subnets and maxclients-reserved} {
        # Configure non-default settings
        r config set priority-subnets "127.0.0.0/8,::1/128"
        r config set maxclients-reserved 10
        r config set maxclients 20

        # Verify initial values
        assert_equal {127.0.0.0/8,::1/128} [lindex [r config get priority-subnets] 1]
        assert_equal 10 [lindex [r config get maxclients-reserved] 1]
        assert_equal 20 [lindex [r config get maxclients] 1]

        # Trigger CONFIG REWRITE to write changes to valkey.conf on disk
        assert_equal "OK" [r config rewrite]
        set config_file [srv 0 config_file]
        assert_equal 1 [count_message_lines $config_file "priority-subnets"]
        assert_equal 1 [count_message_lines $config_file "maxclients-reserved"]

        # Restart server to reload configuration from disk
        restart_server 0 true false

        # Verify configuration is reloaded accurately from disk
        assert_equal {127.0.0.0/8,::1/128} [lindex [r config get priority-subnets] 1]
        assert_equal 10 [lindex [r config get maxclients-reserved] 1]
        assert_equal 20 [lindex [r config get maxclients] 1]

        # Verify priority admission control functions properly after config reload
        assert_match "*connected_priority_clients:1*" [r info clients]
        set c1 [valkey_client]
        assert_match "*connected_priority_clients:2*" [r info clients]
        $c1 close

        # Reset to default (empty priority-subnets and 0 maxclients-reserved) and rewrite again
        r config set priority-subnets ""
        r config set maxclients-reserved 0
        assert_equal "OK" [r config rewrite]

        # Restart server to verify clearing config persists after reload
        restart_server 0 true false
        assert_equal {} [lindex [r config get priority-subnets] 1]
        assert_equal 0 [lindex [r config get maxclients-reserved] 1]
        assert_match "*connected_priority_clients:0*" [r info clients]

        # Verify debug config-rewrite-force-all rewrite and reload
        r config set priority-subnets "10.0.0.0/8"
        r config set maxclients-reserved 5
        assert_equal [r debug config-rewrite-force-all] "OK"
        restart_server 0 true false
        assert_equal {10.0.0.0/8} [lindex [r config get priority-subnets] 1]
        assert_equal 5 [lindex [r config get maxclients-reserved] 1]

        # Clean up
        r config set priority-subnets ""
        r config set maxclients-reserved 0
        r config rewrite
    }
}

start_server {tags {"qos repl needs:repl external:skip cluster:skip"}} {
    test {CONFIG SET priority-subnets does not demote replication link (Issue #4674)} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        start_server {} {
            set replica [srv 0 client]

            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 100 {
                [string match "*role:slave*master_link_status:up*" [$replica info replication]]
            } else {
                fail "Can't turn the instance into a replica"
            }

            # On primary server, verify that the replica client has flags=H
            assert_match "*flags=*H*" [$primary client list flags H]

            # Change priority-subnets on primary to a subnet that does NOT include the replica
            $primary config set priority-subnets "192.0.2.0/24"

            # Issue #4674: Re-verify that replica connection STILL has flags=H (not demoted)
            assert_match "*flags=*H*" [$primary client list flags H]

            # Verify connected_priority_clients on primary is 0 (replica is not a subnet priority client)
            assert_match "*connected_priority_clients:0*" [$primary info clients]

            # Verify connected_priority_clients on replica is 0 (primary link is not a subnet priority client)
            assert_match "*connected_priority_clients:0*" [$replica info clients]

            # Clear priority-subnets dynamically
            $primary config set priority-subnets ""

            # Re-verify flags=H is still preserved
            assert_match "*flags=*H*" [$primary client list flags H]
        }
    }

    test {Replication link included in priority-subnets preserves high priority and new clients get priority} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        start_server {} {
            set replica [srv 0 client]

            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 100 {
                [string match "*role:slave*master_link_status:up*" [$replica info replication]]
            } else {
                fail "Can't turn the instance into a replica"
            }

            # Find replica IP from primary's client list
            set replica_ip ""
            foreach line [split [$primary client list] "\n"] {
                if {[string match "*flags=*S*" $line] || [string match "*cmd=psync*" $line] || [string match "*cmd=sync*" $line]} {
                    if {[regexp {addr=([^ ]+)} $line -> my_addr]} {
                        if {[string match "*\[*" $my_addr]} {
                            regexp {\[([^\]]+)\]} $my_addr -> replica_ip
                        } else {
                            set replica_ip [lindex [split $my_addr ":"] 0]
                        }
                        break
                    }
                }
            }
            if {$replica_ip eq ""} {
                set replica_ip "127.0.0.1"
            }
            set mask [expr {[string match "*:*" $replica_ip] ? 128 : 32}]
            set replica_subnet "$replica_ip/$mask"

            # Reconfigure priority-subnets on primary to INCLUDE the replica's subnet
            $primary config set priority-subnets $replica_subnet

            # Replica retains flags=H, but existing connections are not reclassified into connected_priority_clients
            assert_match "*flags=*H*" [$primary client list flags H]
            assert_match "*connected_priority_clients:0*" [$primary info clients]

            # A new connection from this subnet is admitted as prioritized
            set new_prio [valkey_client_by_addr $primary_host $primary_port]
            assert_match "*connected_priority_clients:1*" [$primary info clients]

            # Reconfigure priority-subnets to a subnet that does NOT include the replica
            $primary config set priority-subnets "192.0.2.0/24"

            # Replica still retains high priority flags, and new_prio is not demoted
            assert_match "*flags=*H*" [$primary client list flags H]
            assert_match "*connected_priority_clients:1*" [$primary info clients]

            # Disconnect new_prio; prioritized count decrements to 0
            $new_prio close
            wait_for_condition 50 100 {
                [string match "*connected_priority_clients:0*" [$primary info clients]]
            } else {
                fail "connected_priority_clients did not decrement to 0 after closing new_prio"
            }

            # Clear priority-subnets completely and re-verify replica flags=H
            $primary config set priority-subnets ""
            assert_match "*flags=*H*" [$primary client list flags H]
            assert_match "*connected_priority_clients:0*" [$primary info clients]
        }
    }

    test {Priority flags in client list and no stat_num_active_priority_clients counter drift with and without priority-subnets} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        # Case 1: Without priority-subnets configured
        $primary config set priority-subnets ""
        assert_match "*connected_priority_clients:0*" [$primary info clients]

        # Normal test client connection
        set normal_client [valkey_client_by_addr $primary_host $primary_port]
        $normal_client ping

        # Verify normal client does NOT have 'H' flag and "client list flags H" is empty
        assert_equal "" [$primary client list flags H]
        assert_match "*connected_priority_clients:0*" [$primary info clients]

        # Connect a replica
        start_server {} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 100 {
                [string match "*role:slave*master_link_status:up*" [$replica info replication]]
            } else {
                fail "Can't turn the instance into a replica"
            }

            # Verify replica client on primary has flags=H and matches "client list flags H"
            assert_match "*flags=*H*" [$primary client list flags H]
            assert_match "*flags=*S*H*" [$primary client list flags H]

            # System-critical replica MUST NOT be counted in stat_num_active_priority_clients
            assert_match "*connected_priority_clients:0*" [$primary info clients]

            # Disconnect replica using "replicaof no one"
            $replica replicaof no one
            wait_for_condition 50 100 {
                [string match "*connected_slaves:0*" [$primary info replication]]
            } else {
                fail "Replica failed to disconnect from primary"
            }

            # Verify no counter drift (stays 0, no underflow) and flags H list is now empty
            assert_match "*connected_priority_clients:0*" [$primary info clients]
            assert_equal "" [$primary client list flags H]
        }

        $normal_client close
        assert_match "*connected_priority_clients:0*" [$primary info clients]

        # Case 2: With priority-subnets configured
        if {[can_bind_loopback_ip "127.0.0.2"]} {
            # Configure priority-subnets to 127.0.0.2/32 (replica connects from 127.0.0.1)
            $primary config set priority-subnets "127.0.0.2/32"
            assert_match "*connected_priority_clients:0*" [$primary info clients]

            # Prioritized client connects from 127.0.0.2
            set prio_client [valkey_from_ip "127.0.0.2" $primary_host $primary_port 1]
            $prio_client ping
            assert_equal {PONG} [$prio_client read]

            # Verify prioritized client has 'H' flag and is counted in connected_priority_clients
            assert_match "*flags=*H*" [$primary client list flags H]
            assert_match "*connected_priority_clients:1*" [$primary info clients]

            # Connect replica (from 127.0.0.1) while priority-subnets is active
            start_server {} {
                set replica [srv 0 client]
                $replica replicaof $primary_host $primary_port
                wait_for_condition 50 100 {
                    [string match "*role:slave*master_link_status:up*" [$replica info replication]]
                } else {
                    fail "Can't turn the instance into a replica"
                }

                # Both prio_client and replica have 'H' flag in client list
                set prio_clients_h [$primary client list flags H]
                assert_match "*flags=*H*" $prio_clients_h
                assert_match "*flags=*S*H*" $prio_clients_h

                # Crucial invariant: stat_num_active_priority_clients ONLY counts subnet clients,
                # NOT system-critical connections like replicas. Thus count remains 1, NOT 2!
                assert_match "*connected_priority_clients:1*" [$primary info clients]

                # Disconnect replica
                $replica replicaof no one
                wait_for_condition 50 100 {
                    [string match "*connected_slaves:0*" [$primary info replication]]
                } else {
                    fail "Replica failed to disconnect from primary"
                }

                # Counter must NOT decrement when replica disconnects (must remain 1)
                assert_match "*connected_priority_clients:1*" [$primary info clients]
            }

            # Disconnect prioritized client
            $prio_client close
            wait_for_condition 50 100 {
                [string match "*connected_priority_clients:0*" [$primary info clients]]
            } else {
                fail "connected_priority_clients did not decrement to 0"
            }

            # Final verification: counter is exactly 0, zero drift
            assert_match "*connected_priority_clients:0*" [$primary info clients]
            assert_equal "" [$primary client list flags H]
            $primary config set priority-subnets ""
        } else {
            # Single loopback fallback: configure subnet not matching replica
            $primary config set priority-subnets "192.0.2.0/24"
            assert_match "*connected_priority_clients:0*" [$primary info clients]

            start_server {} {
                set replica [srv 0 client]
                $replica replicaof $primary_host $primary_port
                wait_for_condition 50 100 {
                    [string match "*role:slave*master_link_status:up*" [$replica info replication]]
                } else {
                    fail "Can't turn the instance into a replica"
                }

                assert_match "*flags=*S*H*" [$primary client list flags H]
                assert_match "*connected_priority_clients:0*" [$primary info clients]

                $replica replicaof no one
                wait_for_condition 50 100 {
                    [string match "*connected_slaves:0*" [$primary info replication]]
                } else {
                    fail "Replica failed to disconnect from primary"
                }

                assert_match "*connected_priority_clients:0*" [$primary info clients]
                assert_equal "" [$primary client list flags H]
            }
            $primary config set priority-subnets ""
        }
    }
}



