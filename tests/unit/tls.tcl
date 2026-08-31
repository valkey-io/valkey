start_server {tags {"tls"}} {
    if {$::tls} {
        package require tls

        proc check_client_stuck {control_client client_port {min_omem 1}} {
            set clients [$control_client CLIENT LIST]
            foreach client [split $clients "\n"] {
                if {[regexp "addr=127.0.0.1:$client_port" $client]} {
                    if {[regexp {omem=([0-9]+)} $client -> omem]} {
                        return [expr {$omem >= $min_omem}]
                    }
                }
            }
            return 0
        }

        test {TLS: Not accepting non-TLS connections on a TLS port} {
            set s [valkey [srv 0 host] [srv 0 port]]
            catch {$s PING} e
            set e
        } {*I/O error*}

        test {TLS: Verify tls-auth-clients behaves as expected} {
            set s [valkey [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel] -cafile $::tlsdir/ca.crt
            catch {$s PING} e
            assert_match {*error*} $e

            r CONFIG SET tls-auth-clients no

            set s [valkey [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel] -cafile $::tlsdir/ca.crt
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-auth-clients optional

            set s [valkey [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel] -cafile $::tlsdir/ca.crt
            catch {$s PING} e
            assert_match {PONG} $e

            r CONFIG SET tls-auth-clients yes

            set s [valkey [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel] -cafile $::tlsdir/ca.crt
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

        test {TLS: Certificate CN with an embedded NUL does not authenticate as the truncated user} {
            r ACL SETUSER {Client-only} on allcommands allkeys
            r CONFIG SET tls-auth-clients-user CN
            set denied_before [s acl_access_denied_tls_cert]

            # The CN is "Client-only\0attacker". Read as a C string it is "Client-only".
            set s [valkey [srv 0 host] [srv 0 port]]
            ::tls::import [$s channel] -cafile $::tlsdir/ca.crt \
                -certfile $::tlsdir/client-nul-cn.crt -keyfile $::tlsdir/client-nul-cn.key
            assert_equal "default" [$s ACL WHOAMI]
            $s close

            # The rejected identity reaches the ACL log.
            assert_equal [expr $denied_before + 1] [s acl_access_denied_tls_cert]

            r ACL DELUSER {Client-only}
            r CONFIG SET tls-auth-clients-user off
        }

        test {TLS: connTLSWritev stack overflow crash reproduction} {
            # Regression test for a bug where a previously failed OpenSSL write for a
            # small server response would trigger a stack overflow if immediately
            # followed by any large server response.
            # Prepare data on server
            # We use a control client (TCP) to avoid TLS write errors on control connection
            set plain_port [srv 0 pport]
            # Ensure the plaintext listener is active in case a prior test disabled it.
            r CONFIG SET port $plain_port
            set control_client [valkey [srv 0 host] $plain_port]
            
            $control_client SELECT 0
            $control_client SET large_key [string repeat "A" 10485760] ;# 10MB
            $control_client SET small_key [string repeat "B" 10240]    ;# 10KB
            
            # Connect raw TLS client
            set fd [::tls::socket [srv 0 host] [srv 0 port]]
            fconfigure $fd -translation binary -blocking 1
            
            # 1. Enable forced TLS write errors globally
            assert_equal OK [$control_client DEBUG FORCE-TLS-WRITE-ERROR 1]
            
            # 2. Send Batch 1 on TLS client: 2x GET small_key
            # They will be combined by server and fail to write, setting last_failed to ~20KB
            set payload1 ""
            append payload1 "*2\r\n\$3\r\nGET\r\n\$9\r\nsmall_key\r\n"
            append payload1 "*2\r\n\$3\r\nGET\r\n\$9\r\nsmall_key\r\n"
            puts -nonewline $fd $payload1
            flush $fd
            
            # Get local port of raw client
            set client_port [lindex [fconfigure $fd -sockname] 2]
            
            # Wait until the server has accumulated the replies for Batch 1
            # and is stuck (omem > 0).
            wait_for_condition 50 100 {
                [check_client_stuck $control_client $client_port]
            } else {
                fail "Timeout waiting for client replies to stack up"
            }
            
            # 3. Send Batch 2 on TLS client: GET large_key
            # This is appended to the reply list
            set payload2 ""
            append payload2 "*2\r\n\$3\r\nGET\r\n\$9\r\nlarge_key\r\n"
            puts -nonewline $fd $payload2
            flush $fd
            
            # Wait until the server has processed Batch 2 and queued the large reply.
            # Total expected omem is at least 10MB.
            wait_for_condition 50 100 {
                [check_client_stuck $control_client $client_port 10000000]
            } else {
                fail "Timeout waiting for large key reply to be queued"
            }
            
            # 4. Disable forced TLS write errors globally
            # This will trigger the server to resume writing to the TLS client.
            # It will call connTLSWritev with iov[0].iov_len (10KB) < last_failed (20KB),
            # and iov_bytes_len > 64KB (due to large_key), triggering the fallback path.
            assert_equal OK [$control_client DEBUG FORCE-TLS-WRITE-ERROR 0]
            
            # 5. Read replies from TLS client.
            # If the server crashed, this will fail with I/O error.
            # We expect:
            # - Reply 1: 10KB of 'B's (plus protocol helper)
            # - Reply 2: 10KB of 'B's (plus protocol helper)
            # - Reply 3: 10MB of 'A's (plus protocol helper)
            # Total expected bytes:
            # small_key reply: "$10240\r\n" (8 bytes) + 10240 bytes + "\r\n" (2 bytes) = 10250 bytes
            # large_key reply: "$10485760\r\n" (11 bytes) + 10485760 bytes + "\r\n" (2 bytes) = 10485773 bytes
            # Total = 10250 + 10250 + 10485773 = 10506273 bytes
            # Let's just read the expected number of bytes.
            
            set expected_bytes [expr {10250 + 10250 + 10485773}]
            set got 0
            set data ""
            while {$got < $expected_bytes} {
                set chunk [read $fd [expr {$expected_bytes - $got}]]
                if {[string length $chunk] == 0} {
                    if {[eof $fd]} {
                        error "EOF reached before reading all bytes"
                    }
                    # Keep trying if not EOF
                    after 10
                    continue
                }
                incr got [string length $chunk]
                # We only keep the last 100 bytes to check integrity without using too much memory
                append data $chunk
                if {[string length $data] > 100} {
                    set data [string range $data end-99 end]
                }
            }
            
            # Assert we got everything and the tail is correct (ends with 'A's + \r\n)
            assert_equal $expected_bytes $got
            assert_match "*[string repeat "A" 80]\r\n" $data
            
            close $fd
            $control_client close
        }
    }
}
