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
            r ACL SETUSER {Client-only} on >clientpass allcommands allkeys

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
            set server_path [file normalize ./src/valkey-server]
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
                lappend cmd --loadmodule [file normalize ./src/valkey-tls.so]
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
