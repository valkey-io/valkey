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

        test {TLS: INFO tls reports decreasing expiration countdown} {
            set info1 [r info tls]
            if {![regexp {tls_server_cert_serial:([^\r\n]+)} $info1 -> server_serial]} {
                fail "INFO tls missing tls_server_cert_serial"
            }
            assert {$server_serial ne "none"}
            if {![regexp {tls_client_cert_serial:([^\r\n]+)} $info1 -> client_serial]} {
                fail "INFO tls missing tls_client_cert_serial"
            }
            assert {$client_serial ne "none"}
            if {![regexp {tls_ca_cert_serial:([^\r\n]+)} $info1 -> ca_serial]} {
                fail "INFO tls missing tls_ca_cert_serial"
            }
            assert {$ca_serial ne "none"}
            if {![regexp {tls_ca_cert_count:([0-9]+)} $info1 -> ca_count]} {
                fail "INFO tls missing tls_ca_cert_count"
            }
            assert_morethan $ca_count 0
            if {![regexp {tls_server_cert_expires_in_seconds:(-?[0-9]+)} $info1 -> expire1]} {
                fail "INFO tls missing tls_server_cert_expires_in_seconds"
            }
            assert_morethan $expire1 0
            foreach field {tls_client_cert_expires_in_seconds tls_ca_cert_expires_in_seconds} {
                if {![regexp "${field}:(-?[0-9]+)" $info1 -> exp_value]} {
                    fail "INFO tls missing $field"
                }
                assert_morethan $exp_value 0
            }

            after 1200

            set info2 [r info tls]
            if {![regexp {tls_server_cert_expires_in_seconds:(-?[0-9]+)} $info2 -> expire2]} {
                fail "INFO tls missing tls_server_cert_expires_in_seconds after delay"
            }

            assert_morethan $expire1 $expire2
            set delta [expr {$expire1 - $expire2}]
            assert_morethan_equal $delta 1
        }

        test {TLS: INFO tls uses earliest CA expiry in bundle} {
            set ca_cert [format "%s/tests/tls/ca.crt" [pwd]]
            set server_cert [format "%s/tests/tls/server.crt" [pwd]]
            if {[catch {set ca_output [exec openssl x509 -noout -enddate -serial -in $ca_cert]} err]} {
                skip "openssl CLI unavailable: $err"
            }
            if {[catch {set server_output [exec openssl x509 -noout -enddate -serial -in $server_cert]} err]} {
                skip "openssl CLI unavailable: $err"
            }
            if {![regexp {notAfter=([^\n]+)} $ca_output -> ca_notafter] ||
                ![regexp {serial=([0-9A-Fa-f]+)} $ca_output -> ca_serial_raw]} {
                fail "Unable to parse CA cert metadata"
            }
            if {![regexp {notAfter=([^\n]+)} $server_output -> server_notafter] ||
                ![regexp {serial=([0-9A-Fa-f]+)} $server_output -> server_serial_raw]} {
                fail "Unable to parse server cert metadata"
            }
            set ca_expiry [clock scan $ca_notafter -format "%b %d %H:%M:%S %Y GMT" -gmt 1]
            set server_expiry [clock scan $server_notafter -format "%b %d %H:%M:%S %Y GMT" -gmt 1]
            set ca_serial_expected [string toupper $ca_serial_raw]
            set server_serial_expected [string toupper $server_serial_raw]
            if {$ca_expiry <= $server_expiry} {
                set expected_serial $ca_serial_expected
            } else {
                set expected_serial $server_serial_expected
            }
            set ca_bundle [format "%s/tests/tls/ca-multi.crt" [pwd]]
            start_server [list overrides [list tls-ca-cert-file $ca_bundle]] {
                set info [r info tls]
                if {![regexp {tls_ca_cert_count:([0-9]+)} $info -> ca_count]} {
                    fail "INFO tls missing tls_ca_cert_count"
                }
                assert_equal 2 $ca_count
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
                assert_equal $expected_serial $ca_serial
            }
        }

        test {TLS: INFO tls resets expiration countdown when TLS disabled/enabled} {
            set host [srv 0 host]
            set tls_port [srv 0 port]
            set plain_port [srv 0 pport]

            if {$plain_port == 0} {
                fail "Plaintext port not available for TLS test harness"
            }

            set tls_client [valkey $host $tls_port 0 1]
            set plain_client [valkey $host $plain_port 0 0]

            set info_enabled [$tls_client info tls]
            if {![regexp {tls_server_cert_expires_in_seconds:(-?[0-9]+)} $info_enabled -> expire_enabled]} {
                fail "INFO tls missing tls_server_cert_expires_in_seconds (enabled)"
            }
            assert_morethan $expire_enabled 0

            $tls_client close

            $plain_client CONFIG SET tls-port 0

            set info_disabled [$plain_client info tls]
            foreach field {tls_server_cert_serial tls_client_cert_serial tls_ca_cert_serial} {
                set pattern [format {%s:([^\r\n]+)} $field]
                if {![regexp $pattern $info_disabled -> serial_value]} {
                    fail "INFO tls missing $field (disabled)"
                }
                assert_equal "none" $serial_value
            }
            if {![regexp {tls_ca_cert_count:([0-9]+)} $info_disabled -> ca_count_disabled]} {
                fail "INFO tls missing tls_ca_cert_count (disabled)"
            }
            assert_equal 0 $ca_count_disabled
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

            $plain_client CONFIG SET tls-port $tls_port

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
            if {![regexp {tls_ca_cert_count:([0-9]+)} $info_reenabled -> ca_count_reenabled]} {
                fail "INFO tls missing tls_ca_cert_count after re-enable"
            }
            assert_morethan $ca_count_reenabled 0
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

start_server {} {
    test {INFO tls reports empty values when TLS disabled} {
        set info [r info tls]
        foreach field {tls_server_cert_serial tls_client_cert_serial tls_ca_cert_serial} {
            set pattern [format {%s:([^\r\n]+)} $field]
            if {![regexp $pattern $info -> value]} {
                fail "INFO tls missing $field"
            }
            assert_equal "none" $value
        }
        if {![regexp {tls_ca_cert_count:([0-9]+)} $info -> ca_count]} {
            fail "INFO tls missing tls_ca_cert_count"
        }
        assert_equal 0 $ca_count
        foreach field {tls_server_cert_expires_in_seconds tls_client_cert_expires_in_seconds tls_ca_cert_expires_in_seconds} {
            set pattern [format {%s:(-?[0-9]+)} $field]
            if {![regexp $pattern $info -> value]} {
                fail "INFO tls missing $field"
            }
            assert_equal 0 $value
        }
    }
}
