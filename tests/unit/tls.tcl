start_server {tags {"tls"}} {
    if {$::tls} {
        package require tls

        test {TLS: Not accepting non-TLS connections on a TLS port} {
            set s [valkey [srv 0 host] [srv 0 port]]
            catch {$s PING} e
            set e
        } {*I/O error*}

        test {TLS: Verify tls-auth-clients behaves as expected} {
            set s [valkey [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel]
            catch {$s PING} e
            assert_match {*error*} $e

            r CONFIG SET tls-auth-clients no

            set s [valkey [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel]
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-auth-clients optional

            set s [valkey [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel]
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-auth-clients yes

            set s [valkey [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel]
            catch {$s PING} e
            assert_match {*error*} $e
        }

        test {TLS: Verify tls-protocols behaves as expected} {
            r CONFIG SET tls-protocols TLSv1.2

            set s [valkey [srv 0 host] [srv 0 port] 0 1 {-tls1.2 0}]
            catch {$s PING} e
            assert_match {*I/O error*} $e

            set s [valkey [srv 0 host] [srv 0 port] 0 1 {-tls1.2 1}]
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-protocols ""
        }

        test {TLS: Verify tls-ciphers behaves as expected} {
            r CONFIG SET tls-protocols TLSv1.2
            r CONFIG SET tls-ciphers "DEFAULT:-AES128-SHA256"

            set s [valkey [srv 0 host] [srv 0 port] 0 1 {-cipher "-ALL:AES128-SHA256"}]
            catch {$s PING} e
            assert_match {*I/O error*} $e

            set s [valkey [srv 0 host] [srv 0 port] 0 1 {-cipher "-ALL:AES256-SHA256"}]
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-ciphers "DEFAULT"

            set s [valkey [srv 0 host] [srv 0 port] 0 1 {-cipher "-ALL:AES128-SHA256"}]
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-protocols ""
            r CONFIG SET tls-ciphers "DEFAULT"
        }

        test {TLS: Verify tls-prefer-server-ciphers behaves as expected} {
            r CONFIG SET tls-protocols TLSv1.2
            r CONFIG SET tls-ciphers "AES128-SHA256:AES256-SHA256"

            set s [valkey [srv 0 host] [srv 0 port] 0 1 {-cipher "AES256-SHA256:AES128-SHA256"}]
            catch {$s PING} e
            assert_match {PONG} $e

            assert_equal "AES256-SHA256" [dict get [::tls::status [$s channel]] cipher]

            r CONFIG SET tls-prefer-server-ciphers yes

            set s [valkey [srv 0 host] [srv 0 port] 0 1 {-cipher "AES256-SHA256:AES128-SHA256"}]
            catch {$s PING} e
            assert_match {PONG} $e

            assert_equal "AES128-SHA256" [dict get [::tls::status [$s channel]] cipher]

            r CONFIG SET tls-protocols ""
            r CONFIG SET tls-ciphers "DEFAULT"
        }

        test {TLS: Verify tls-cert-file is also used as a client cert if none specified} {
            set master [srv 0 client]
            set master_host [srv 0 host]
            set master_port [srv 0 port]

            # Use a non-restricted client/server cert for the replica
            set valkey_crt [format "%s/tests/tls/valkey.crt" [pwd]]
            set valkey_key [format "%s/tests/tls/valkey.key" [pwd]]

            start_server [list overrides [list tls-cert-file $valkey_crt tls-key-file $valkey_key] \
                               omit [list tls-client-cert-file tls-client-key-file]] {
                set replica [srv 0 client]
                $replica replicaof $master_host $master_port
                wait_for_condition 30 100 {
                    [string match {*master_link_status:up*} [$replica info replication]]
                } else {
                    fail "Can't authenticate to master using just tls-cert-file!"
                }
            }
        }

        test {TLS: switch between tcp and tls ports} {
            set srv_port [srv 0 port]

            # TLS
            set rd [valkey [srv 0 host] $srv_port 0 1]
            $rd PING

            # TCP
            $rd CONFIG SET tls-port 0
            $rd CONFIG SET port $srv_port
            $rd close

            set rd [valkey [srv 0 host] $srv_port 0 0]
            $rd PING

            # TLS
            $rd CONFIG SET port 0
            $rd CONFIG SET tls-port $srv_port
            $rd close

            set rd [valkey [srv 0 host] $srv_port 0 1]
            $rd PING
            $rd close
        }

        test {TLS: Working with an encrypted keyfile} {
            # Create an encrypted version
            set keyfile [lindex [r config get tls-key-file] 1]
            set keyfile_encrypted "$keyfile.encrypted"
            exec -ignorestderr openssl rsa -in $keyfile -out $keyfile_encrypted -aes256 -passout pass:1234 2>/dev/null

            # Using it without a password fails
            catch {r config set tls-key-file $keyfile_encrypted} e
            assert_match {*Unable to update TLS*} $e

            # Now use a password
            r config set tls-key-file-pass 1234
            r config set tls-key-file $keyfile_encrypted
        }

        test {TLS: Auto-authenticate using tls-auth-clients-user (CN)} {
            # Create a user matching the CN in the client certificate (CN=Client-only)
            r ACL SETUSER {Client-only} on allcommands allkeys

            # Enable the feature to auto-authenticate based on CN
            r CONFIG SET tls-auth-clients-user CN

            # With feature on, client should be auto-authenticated using CN=Client-only
            set s [valkey_client]

            # Now no explicit AUTH is needed
            assert_equal "PONG" [$s PING]

            # Verify that the authenticated user is 'Client-only'
            assert_equal "Client-only" [$s ACL WHOAMI]

            $s close
        }

        test {TLS: Auto-authenticate using tls-auth-clients-user (URI)} {
            # Enable the feature to auto-authenticate based on URI
            r CONFIG SET tls-auth-clients-user URI
            
            # Create users matching the URI in the client certificate
            r ACL SETUSER {urn:valkey:user:first} on allcommands
            r ACL SETUSER {urn:valkey:user:second} on allcommands

            # With feature on, client should be auto-authenticated using the URI from SAN
            # Verify that the authenticated user matches the first URI
            set s [valkey_client]
            assert_equal "urn:valkey:user:first" [$s ACL WHOAMI]
            $s close

            # Turn off the first user
            r ACL SETUSER {urn:valkey:user:first} off

            # Verify that the authenticated user matches the second URI
            set s [valkey_client]
            assert_equal "urn:valkey:user:second" [$s ACL WHOAMI]
            $s close

            # Turn off the second user
            r ACL SETUSER {urn:valkey:user:second} off

            # Verify that the authenticated user matches the default
            set s [valkey_client]
            assert_equal "default" [$s ACL WHOAMI]
            $s close

            # Delete all users
            r ACL DELUSER {urn:valkey:user:first} {urn:valkey:user:second}

            # Verify that the authenticated user matches the default
            set s [valkey_client]
            assert_equal "default" [$s ACL WHOAMI]
            $s close

            # Restore
            r CONFIG SET tls-auth-clients-user off
        }

        test {TLS: Verify CN and URI modes are mutually exclusive} {
            # Create both CN and URI users
            r ACL SETUSER {Client-only} on allcommands allkeys
            r ACL SETUSER {urn:valkey:user:first} on allcommands allkeys

            # Set to CN mode
            r CONFIG SET tls-auth-clients-user CN
            set s [valkey_client]
            assert_equal "Client-only" [$s ACL WHOAMI]
            $s close

            # Set to URI mode
            r CONFIG SET tls-auth-clients-user URI
            set s [valkey_client]
            assert_equal "urn:valkey:user:first" [$s ACL WHOAMI]
            $s close

            # Clean up
            r ACL DELUSER {Client-only} {urn:valkey:user:first}
            r CONFIG SET tls-auth-clients-user off
        }

        test {TLS: Auto-reload detects changes} {
            if {$::tls_module} {
                # Auto-reload requires built-in TLS
                skip "Not supported with TLS built as a module"
            }
            # Get current certificate files
            set orig_server_crt [lindex [r config get tls-cert-file] 1]
            set orig_server_key [lindex [r config get tls-key-file] 1]

            # Create temporary certificate files (copies of current ones)
            set temp_crt "$orig_server_crt.temp"
            set temp_key "$orig_server_key.temp"
            file copy -force $orig_server_crt $temp_crt
            file copy -force $orig_server_key $temp_key

            # Ensure cleanup happens even if test fails
            try {
                # Update server to use temporary certificate files
                r CONFIG SET tls-cert-file $temp_crt tls-key-file $temp_key

                # Enable auto-reload with 1 second interval for faster testing
                r CONFIG SET tls-auto-reload-interval 1

                # Verify initial connection works
                set s [valkey_client]
                assert_equal "PONG" [$s PING]
                $s close
                set info1 [r info tls]
                if {![regexp {tls_server_cert_serial:([^\r\n]+)} $info1 -> serial1]} {
                    fail "INFO tls missing tls_server_cert_serial"
                }
                assert {$serial1 ne "none"}

                # Wait for at least one auto-reload cycle to complete
                after 1100

                # Update temporary files with different certificate
                set valkey_crt [format "%s/tests/tls/valkey.crt" [pwd]]
                set valkey_key [format "%s/tests/tls/valkey.key" [pwd]]
                file copy -force $valkey_crt $temp_crt
                file copy -force $valkey_key $temp_key

                # Wait for reload to actually complete by checking server logs
                # Use generous timeout for slow/busy CI systems
                wait_for_log_messages 0 {"*TLS materials reloaded successfully*"} 0 150 100

                # Verify connection still works after reload
                set s [valkey_client]
                assert_equal "PONG" [$s PING]
                $s close
                set info2 [r info tls]
                if {![regexp {tls_server_cert_serial:([^\r\n]+)} $info2 -> serial2]} {
                    fail "INFO tls missing tls_server_cert_serial after reload"
                }
                assert {$serial2 ne "none"}
                assert {$serial1 ne $serial2}

                # Wait again to ensure filesystem timestamp will be different
                # for the second modification and next reload cycle can detect it
                after 1100

                # Restore original certificate content to temporary files
                file copy -force $orig_server_crt $temp_crt
                file copy -force $orig_server_key $temp_key

                # Wait for second reload to complete
                # Use generous timeout for slow/busy CI systems
                wait_for_log_messages 0 {"*TLS materials reloaded successfully*"} 0 150 100

                # Verify connection still works after restore
                set s [valkey_client]
                assert_equal "PONG" [$s PING]
                $s close
            } finally {
                # Restore original configuration
                r CONFIG SET tls-cert-file $orig_server_crt tls-key-file $orig_server_key

                # Disable auto-reload
                r CONFIG SET tls-auto-reload-interval 0

                # Clean up temporary files
                file delete -force $temp_crt $temp_key
            }
        }

        test {TLS: Auto-reload skips unchanged materials} {
            if {$::tls_module} {
                # Auto-reload requires built-in TLS
                skip "Not supported with TLS built as a module"
            }
            # Save original loglevel
            set orig_loglevel [lindex [r config get loglevel] 1]

            try {
                # Enable auto-reload with 1 second interval
                r CONFIG SET loglevel debug
                r CONFIG SET tls-auto-reload-interval 1

                # Wait for at least one cron cycle to ensure reload check happens
                after 1100

                # Wait for at least one reload check cycle
                # Use generous timeout for slow/busy CI systems
                wait_for_log_messages 0 {"*materials unchanged*"} 0 150 100
            } finally {
                # Disable auto-reload and restore loglevel
                r CONFIG SET tls-auto-reload-interval 0 loglevel $orig_loglevel
            }
        }

        test {TLS: Auto-reload interval validation} {
            if {$::tls_module} {
                # Auto-reload requires built-in TLS
                skip "Not supported with TLS built as a module"
            }
            try {
                # Valid intervals
                r CONFIG SET tls-auto-reload-interval 0
                r CONFIG SET tls-auto-reload-interval 5
                r CONFIG SET tls-auto-reload-interval 3600

                # Invalid intervals should fail
                catch {r CONFIG SET tls-auto-reload-interval -1} e
                assert_match {*ERR CONFIG SET failed*} $e
            } finally {
                # Reset to disabled
                r CONFIG SET tls-auto-reload-interval 0
            }
        }

        test {TLS: Auto-reload with CA cert directory} {
            if {$::tls_module} {
                # Auto-reload requires built-in TLS
                skip "Not supported with TLS built as a module"
            }
            # Get current CA cert directory
            set ca_cert_dir [lindex [r config get tls-ca-cert-dir] 1]

            if {$ca_cert_dir ne ""} {
                # Touch a file in the directory to trigger change detection
                set test_file "$ca_cert_dir/test_marker"
                set fd [open $test_file w]
                puts $fd "test"
                close $fd

                # Ensure cleanup happens even if test fails
                try {
                    # Enable auto-reload with 1 second interval
                    r CONFIG SET tls-auto-reload-interval 1

                    # Wait for reload to actually complete by checking server logs
                    wait_for_log_messages 0 {"*TLS materials reloaded successfully*"} 0 50 100

                    # Verify connection still works after reload
                    set s [valkey_client]
                    assert_equal "PONG" [$s PING]
                    $s close
                } finally {
                    # Disable auto-reload
                    r CONFIG SET tls-auto-reload-interval 0

                    # Clean up test file
                    file delete -force $test_file
                }
            }
        }

        proc test_tls_cert_rejection {cert_type cert_path expected_error} {
            set tlsdir [file normalize ./tests/tls]
            set server_path $::VALKEY_SERVER_BIN
            set server_cert $tlsdir/server.crt
            set server_key $tlsdir/server.key
            set client_cert $tlsdir/client.crt
            set client_key $tlsdir/client.key
            set ca_cert_file $tlsdir/ca.crt
            set ca_cert_dir  ""

            switch -- $cert_type {
                server { set server_cert $cert_path }
                client { set client_cert $cert_path }
                "ca-file" { set ca_cert_file $cert_path }
                "ca-dir"  { set ca_cert_dir $cert_path; set ca_cert_file "" }
            }

            set cmd [list $server_path --port 0 --tls-port 16379 \
                --tls-cert-file $server_cert --tls-key-file $server_key \
                --tls-client-cert-file $client_cert --tls-client-key-file $client_key]

            if {$ca_cert_file ne ""} { lappend cmd --tls-ca-cert-file $ca_cert_file }
            if {$ca_cert_dir ne ""}  { lappend cmd --tls-ca-cert-dir  $ca_cert_dir }

            if {$::tls_module} {
                lappend cmd --loadmodule $::VALKEY_TLS_MODULE
            }

            catch {exec {*}$cmd 2>@1} err
            assert_match $expected_error $err
        }

        test {TLS: Fail-fast on invalid certificates at startup} {
            set tlsdir [file normalize ./tests/tls]

            # Expired server certificate
            test_tls_cert_rejection server $tlsdir/server-expired.crt {*Server TLS certificate is invalid*}

            # Not-yet-valid server certificate
            test_tls_cert_rejection server $tlsdir/server-notyet.crt {*Server TLS certificate is invalid*}

            # Expired client certificate
            test_tls_cert_rejection client $tlsdir/client-expired.crt {*Client TLS certificate is invalid*}

            # Not-yet-valid client certificate
            test_tls_cert_rejection client $tlsdir/client-notyet.crt {*Client TLS certificate is invalid*}

            # Expired CA certificate file
            test_tls_cert_rejection ca-file $tlsdir/ca-expired.crt {*One or more loaded CA certificates are invalid*}

            # Not-yet-valid CA certificate file
            test_tls_cert_rejection ca-file $tlsdir/ca-notyet.crt {*One or more loaded CA certificates are invalid*}

            # Expired CA certificate directory
            test_tls_cert_rejection ca-dir $tlsdir/ca-expired {*One or more loaded CA certificates are invalid*}

            # Not-yet-valid CA certificate directory
            test_tls_cert_rejection ca-dir $tlsdir/ca-notyet {*One or more loaded CA certificates are invalid*}
        }

        proc test_tls_cert_rejection_runtime {r cert_type cert_path} {
            switch -- $cert_type {
                server {
                    catch {$r CONFIG SET tls-cert-file $cert_path} err
                }
                client {
                    catch {$r CONFIG SET tls-client-cert-file $cert_path} err
                }
                "ca-file" {
                    catch {$r CONFIG SET tls-ca-cert-file $cert_path} err
                }
                "ca-dir" {
                    catch {$r CONFIG SET tls-ca-cert-dir $cert_path} err
                }
            }
            assert_match {*Unable to update TLS*} $err
        }

        test {TLS: Fail-fast on invalid certificates at runtime} {
            set tlsdir [file normalize ./tests/tls]

            # Expired server certificate
            test_tls_cert_rejection_runtime r server $tlsdir/server-expired.crt

            # Not-yet-valid server certificate
            test_tls_cert_rejection_runtime r server $tlsdir/server-notyet.crt

            # Expired client certificate
            test_tls_cert_rejection_runtime r client $tlsdir/client-expired.crt

            # Not-yet-valid client certificate
            test_tls_cert_rejection_runtime r client $tlsdir/client-notyet.crt

            # Expired CA certificate file
            test_tls_cert_rejection_runtime r ca-file $tlsdir/ca-expired.crt

            # Not-yet-valid CA certificate file
            test_tls_cert_rejection_runtime r ca-file $tlsdir/ca-notyet.crt

            # Expired CA certificate directory
            test_tls_cert_rejection_runtime r ca-dir $tlsdir/ca-expired

            # Not-yet-valid CA certificate directory
            test_tls_cert_rejection_runtime r ca-dir $tlsdir/ca-notyet
        }
    }
}

start_server {} {
    test {INFO tls reports empty values when TLS disabled} {
        if {$::tls} {
            skip "TLS enabled"
        }
        set info [r info tls]
        foreach field {tls_server_cert_serial tls_client_cert_serial tls_ca_cert_serial} {
            set pattern [format {%s:([^\r\n]+)} $field]
            if {![regexp $pattern $info -> value]} {
                fail "INFO tls missing $field"
            }
            assert_equal "none" $value
        }
        foreach field {tls_server_cert_expires_in_seconds tls_client_cert_expires_in_seconds tls_ca_cert_expires_in_seconds} {
            set pattern [format {%s:(-?[0-9]+)} $field]
            if {![regexp $pattern $info -> value]} {
                fail "INFO tls missing $field"
            }
            assert_equal 0 $value
        }
    }
}

start_server {tags {"tls"}} {
    if {$::tls} {
        test {TLS: INFO tls reports decreasing expiration countdown} {
            set info1 [r info tls]
            if {![regexp {tls_server_cert_serial:([^\r\n]+)} $info1 -> server_serial]} {
                fail "INFO tls missing tls_server_cert_serial"
            }
            assert {$server_serial ne "none"}
            set client_serial [getInfoProperty $info1 tls_client_cert_serial]
            assert_not_equal $client_serial {}
            assert {$client_serial ne "none"}
            if {![regexp {tls_ca_cert_serial:([^\r\n]+)} $info1 -> ca_serial]} {
                fail "INFO tls missing tls_ca_cert_serial"
            }
            assert {$ca_serial ne "none"}
            if {![regexp {tls_server_cert_expires_in_seconds:(-?[0-9]+)} $info1 -> expire1]} {
                fail "INFO tls missing tls_server_cert_expires_in_seconds"
            }
            assert_morethan $expire1 0
            foreach field {tls_client_cert_expires_in_seconds tls_ca_cert_expires_in_seconds} {
                set pattern [format {%s:(-?[0-9]+)} $field]
                if {![regexp $pattern $info1 -> exp_value]} {
                    fail "INFO tls missing $field"
                }
                assert_morethan $exp_value 0
            }

            set expire2 -1
            wait_for_condition 10 500 {
                [regexp {tls_server_cert_expires_in_seconds:(-?[0-9]+)} [r info tls] -> expire2] &&
                $expire2 < $expire1
            } else {
                fail "INFO tls expiration countdown did not decrease"
            }

            assert_morethan $expire1 $expire2
            set delta [expr {$expire1 - $expire2}]
            assert_morethan_equal $delta 1
        }

        test {TLS: INFO tls uses earliest CA expiry in bundle} {
            set ca_cert [format "%s/tests/tls/ca.crt" [pwd]]
            set server_cert [format "%s/tests/tls/server.crt" [pwd]]
            set ca_bundle [format "%s/tests/tls/ca-multi.crt" [pwd]]
            if {![file exists $ca_bundle]} {
                fail "missing $ca_bundle; run utils/gen-test-certs.sh"
            }
            start_server [list overrides [list tls-ca-cert-file $ca_bundle]] {
                set info [r info tls]
                if {![regexp {tls_server_cert_expires_in_seconds:(-?[0-9]+)} $info -> server_exp]} {
                    fail "INFO tls missing tls_server_cert_expires_in_seconds"
                }
                if {![regexp {tls_ca_cert_expires_in_seconds:(-?[0-9]+)} $info -> ca_exp]} {
                    fail "INFO tls missing tls_ca_cert_expires_in_seconds"
                }
                if {![regexp {tls_ca_cert_serial:([^\r\n]+)} $info -> ca_serial]} {
                    fail "INFO tls missing tls_ca_cert_serial"
                }
                assert_morethan $server_exp 0
                assert_morethan $ca_exp 0
                assert {$ca_serial ne "none"}
                assert_morethan_equal $server_exp $ca_exp
            }
        }

        test {TLS: INFO tls reports CA cert info from directory} {
            set ca_dir [format "%s/tests/tls/ca-dir" [pwd]]
            if {![file isdirectory $ca_dir]} {
                fail "missing $ca_dir; run utils/gen-test-certs.sh"
            }
            start_server [list overrides [list tls-ca-cert-dir $ca_dir]] {
                set info [r info tls]
                if {![regexp {tls_ca_cert_serial:([^\r\n]+)} $info -> ca_serial]} {
                    fail "INFO tls missing tls_ca_cert_serial"
                }
                assert {$ca_serial ne "none"}
                if {![regexp {tls_ca_cert_expires_in_seconds:(-?[0-9]+)} $info -> ca_exp]} {
                    fail "INFO tls missing tls_ca_cert_expires_in_seconds"
                }
                assert_morethan $ca_exp 0
            }
        }

        test {TLS: INFO tls reports CA cert info from directory only} {
            set ca_dir [format "%s/tests/tls/ca-dir" [pwd]]
            if {![file isdirectory $ca_dir]} {
                fail "missing $ca_dir; run utils/gen-test-certs.sh"
            }
            start_server [list overrides [list tls-ca-cert-dir $ca_dir] \
                               omit [list tls-ca-cert-file]] {
                set info [r info tls]
                if {![regexp {tls_ca_cert_serial:([^\r\n]+)} $info -> ca_serial]} {
                    fail "INFO tls missing tls_ca_cert_serial"
                }
                assert {$ca_serial ne "none"}
                if {![regexp {tls_ca_cert_expires_in_seconds:(-?[0-9]+)} $info -> ca_exp]} {
                    fail "INFO tls missing tls_ca_cert_expires_in_seconds"
                }
                assert_morethan $ca_exp 0
            }
        }

        test {TLS: INFO tls shows none for missing client cert} {
            start_server [list overrides [list tls-auth-clients no] \
                               omit [list tls-client-cert-file tls-client-key-file]] {
                set info [r info tls]
                set client_serial [getInfoProperty $info tls_client_cert_serial]
                assert_not_equal $client_serial {}
                assert_equal "none" $client_serial
                if {![regexp {tls_client_cert_expires_in_seconds:(-?[0-9]+)} $info -> client_exp]} {
                    fail "INFO tls missing tls_client_cert_expires_in_seconds"
                }
                assert_equal 0 $client_exp
            }
        }

        test {TLS: INFO tls clears expiration countdown when TLS disabled} {
            set host [srv 0 host]
            set tls_port [srv 0 port]
            set plain_port [srv 0 pport]

            if {$plain_port == 0} {
                fail "Plaintext port not available for TLS test harness"
            }

            set plain_client [valkey $host $plain_port 0 0]

            # Ensure the plaintext listener is active in case a prior test disabled it.
            $plain_client CONFIG SET port $plain_port

            $plain_client CONFIG SET tls-port $tls_port
            $plain_client CONFIG SET tls-replication yes
            $plain_client CONFIG SET tls-cluster yes
            if {$::tls_module} {
                # Force TLS module to refresh cert info after re-enable.
                set cert [lindex [$plain_client config get tls-cert-file] 1]
                set key [lindex [$plain_client config get tls-key-file] 1]
                set cafile [lindex [$plain_client config get tls-ca-cert-file] 1]
                $plain_client config set tls-cert-file $cert
                $plain_client config set tls-key-file $key
                $plain_client config set tls-ca-cert-file $cafile
            }

            wait_for_condition 50 100 {
                [catch {set tls_client [valkey $host $tls_port 0 1]} err] == 0
            } else {
                fail "Timed out waiting for TLS listener to restart ($err)"
            }

            set info_enabled [$tls_client info tls]
            if {![regexp {tls_server_cert_expires_in_seconds:(-?[0-9]+)} $info_enabled -> expire_enabled]} {
                fail "INFO tls missing tls_server_cert_expires_in_seconds (enabled)"
            }
            assert_morethan $expire_enabled 0

            $tls_client close

            $plain_client CONFIG SET tls-replication no
            $plain_client CONFIG SET tls-cluster no
            $plain_client CONFIG SET tls-port 0

            wait_for_condition 50 100 {
                [regexp {tls_server_cert_serial:none} [$plain_client info tls]] &&
                [regexp {tls_client_cert_serial:none} [$plain_client info tls]] &&
                [regexp {tls_ca_cert_serial:none} [$plain_client info tls]] &&
                [regexp {tls_server_cert_expires_in_seconds:0} [$plain_client info tls]] &&
                [regexp {tls_client_cert_expires_in_seconds:0} [$plain_client info tls]] &&
                [regexp {tls_ca_cert_expires_in_seconds:0} [$plain_client info tls]]
            } else {
                fail "Timed out waiting for TLS to disable"
            }

            set info_disabled [$plain_client info tls]
            foreach field {tls_server_cert_serial tls_client_cert_serial tls_ca_cert_serial} {
                set pattern [format {%s:([^\r\n]+)} $field]
                if {![regexp $pattern $info_disabled -> serial_value]} {
                    fail "INFO tls missing $field (disabled)"
                }
                assert_equal "none" $serial_value
            }
            if {![regexp {tls_server_cert_expires_in_seconds:(-?[0-9]+)} $info_disabled -> expire_disabled]} {
                fail "INFO tls missing tls_server_cert_expires_in_seconds (disabled)"
            }
            assert_equal 0 $expire_disabled
            foreach field {tls_client_cert_expires_in_seconds tls_ca_cert_expires_in_seconds} {
                set pattern [format {%s:(-?[0-9]+)} $field]
                if {![regexp $pattern $info_disabled -> exp_value]} {
                    fail "INFO tls missing $field (disabled)"
                }
                assert_equal 0 $exp_value
            }

            $plain_client close
        }

        test {TLS: INFO tls shows expiration countdown when TLS re-enabled} {
            set host [srv 0 host]
            set tls_port [srv 0 port]
            set plain_port [srv 0 pport]

            if {$plain_port == 0} {
                fail "Plaintext port not available for TLS test harness"
            }

            set plain_client [valkey $host $plain_port 0 0]

            # Ensure the plaintext listener is active in case a prior test disabled it.
            $plain_client CONFIG SET port $plain_port

            $plain_client CONFIG SET tls-replication no
            $plain_client CONFIG SET tls-cluster no
            $plain_client CONFIG SET tls-port 0

            wait_for_condition 50 100 {
                [regexp {tls_server_cert_serial:none} [$plain_client info tls]] &&
                [regexp {tls_client_cert_serial:none} [$plain_client info tls]] &&
                [regexp {tls_ca_cert_serial:none} [$plain_client info tls]] &&
                [regexp {tls_server_cert_expires_in_seconds:0} [$plain_client info tls]] &&
                [regexp {tls_client_cert_expires_in_seconds:0} [$plain_client info tls]] &&
                [regexp {tls_ca_cert_expires_in_seconds:0} [$plain_client info tls]]
            } else {
                fail "Timed out waiting for TLS to disable"
            }

            $plain_client CONFIG SET tls-port $tls_port
            $plain_client CONFIG SET tls-replication yes
            $plain_client CONFIG SET tls-cluster yes
            if {$::tls_module} {
                # Force TLS module to refresh cert info after re-enable.
                set cert [lindex [$plain_client config get tls-cert-file] 1]
                set key [lindex [$plain_client config get tls-key-file] 1]
                set cafile [lindex [$plain_client config get tls-ca-cert-file] 1]
                $plain_client config set tls-cert-file $cert
                $plain_client config set tls-key-file $key
                $plain_client config set tls-ca-cert-file $cafile
            }

            wait_for_condition 50 100 {
                [catch {set tls_client [valkey $host $tls_port 0 1]} err] == 0
            } else {
                fail "Timed out waiting for TLS listener to restart ($err)"
            }

            set info_reenabled [$tls_client info tls]
            foreach field {tls_server_cert_serial tls_client_cert_serial tls_ca_cert_serial} {
                set pattern [format {%s:([^\r\n]+)} $field]
                if {![regexp $pattern $info_reenabled -> serial_enabled]} {
                    fail "INFO tls missing $field after re-enable"
                }
                assert {$serial_enabled ne "none"}
            }
            if {![regexp {tls_server_cert_expires_in_seconds:(-?[0-9]+)} $info_reenabled -> expire_reenabled]} {
                fail "INFO tls missing tls_server_cert_expires_in_seconds (re-enabled)"
            }
            assert_morethan $expire_reenabled 0
            foreach field {tls_client_cert_expires_in_seconds tls_ca_cert_expires_in_seconds} {
                set pattern [format {%s:(-?[0-9]+)} $field]
                if {![regexp $pattern $info_reenabled -> exp_value]} {
                    fail "INFO tls missing $field (re-enabled)"
                }
                assert_morethan $exp_value 0
            }

            $tls_client close
            $plain_client close
        }
    }
}
