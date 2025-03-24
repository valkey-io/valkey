start_server {tags {"tls-rotation"} overrides {tls-rotation yes}} {
    if {$::tls} {
        package require tls

        test {TLS: Create temporary location for certificate rotation} {
            set rootdir [tmpdir tlscerts]

            file copy "tests/tls" $rootdir
            file copy "tests/tls_1" $rootdir
            file copy "tests/tls_2" $rootdir

            set serverdir [format "%s/$rootdir/tls" [pwd]]
            set clientdir1 [format "%s/$rootdir/tls_1" [pwd]]
            set clientdir2 [format "%s/$rootdir/tls_2" [pwd]]
        }

        test {TLS: Update server config to point to temporary location } {
            r config set tls-key-file "$serverdir/server.key"
            r config set tls-cert-file "$serverdir/server.crt"
            r config set tls-ca-cert-file "$serverdir/ca.crt"
        }

        test {TLS: Connect client to server} {
            set r2 [valkey_client_tls -keyfile "$serverdir/client.key" -certfile "$serverdir/client.crt" -require 1 -cafile "$serverdir/ca.crt"]
            $r2 set x 50
            assert_equal {50} [$r2 get x]
        }

        test {TLS: Rotate all TLS certificates} {
            file delete -force -- $serverdir 
            file copy $clientdir1 $serverdir
            after 1000
        }

        test {TLS: Already connected clients do not lose connection post certificate rotation} {
            $r2 incrby x 50
            assert_equal {100} [$r2 get x]
        }

        test {TLS: Clients with outdated credentials cannot connect} {
            catch {set r2 [valkey_client_tls -keyfile "$clientdir2/client.key" -certfile "$clientdir2/client.crt" -require 1 -cafile "$clientdir2/ca.crt"]} e
            assert_no_match {*::redis::redisHandle*} $e
        }

        test {TLS: Clients with correct certificates can cannect to server post rotation} {
            set r2 [valkey_client_tls -keyfile "$clientdir1/client.key" -certfile "$clientdir1/client.crt" -require 1 -cafile "$clientdir1/ca.crt"]
            $r2 incrby x 50
            assert_equal {150} [$r2 get x]
        }

        test {TLS: Rotate only server key causing key/cert mismatch} {
            file copy -force -- "$clientdir2/server.key" $serverdir
            after 1000
        }

        test {TLS: Already connected clients do not lose connection despite mismatch} {
            $r2 incrby x 50
            assert_equal {200} [$r2 get x]
        }

        test {TLS: Clients with old credentials can still connect} {
            set r2 [valkey_client_tls -keyfile "$clientdir1/client.key" -certfile "$clientdir1/client.crt" -require 1 -cafile "$clientdir1/ca.crt"]
            $r2 incrby x 50
            assert_equal {250} [$r2 get x]
        }

        test {TLS: Rotate corresponding cert fixing key/cert mismatch} {
            file copy -force -- "$clientdir2/server.crt" $serverdir
            after 1000
        }

        test {TLS: Check that old client is still connected post rotation} {
            $r2 incrby x 50
            assert_equal {300} [$r2 get x]
        }

        test {TLS: Clients with outdated credentials cannot connect} {
            catch {set r2 [valkey_client_tls -keyfile "$clientdir1/client.key" -certfile "$clientdir1/client.crt" -require 1 -cafile "$clientdir1/ca.crt"]} e
            assert_no_match {*::redis::redisHandle*} $e
        }

        test {TLS: Clients with correct certificates can cannect to server post rotation} {
            set r2 [valkey_client_tls -keyfile "$clientdir1/client.key" -certfile "$clientdir1/client.crt" -require 1 -cafile "$clientdir2/ca.crt"]
            $r2 incrby x 50
            assert_equal {350} [$r2 get x]
        }

        test {TLS: Rotate only CA cert} {
            file copy -force -- "$clientdir2/ca.crt" $serverdir
            after 1000
        }

        test {TLS: Check that old client is still connected} {
            $r2 incrby x 50
            assert_equal {400} [$r2 get x]
        }

        test {TLS: Check that client with old credentials won't connect} {
            catch {set r2 [valkey_client_tls -keyfile "$clientdir1/client.key" -certfile "$clientdir1/client.crt" -require 1 -cafile "$clientdir2/ca.crt"]} e
            assert_no_match {*::redis::redisHandle*} $e
        }

        test {TLS: Check that client with updated credentials will connect} {
            catch {set r2 [valkey_client_tls -keyfile "$clientdir2/client.key" -certfile "$clientdir2/client.crt" -require 1 -cafile "$clientdir2/ca.crt"]} e
            $r2 incrby x 50
            assert_equal {450} [$r2 get x]
        }
    }
}
