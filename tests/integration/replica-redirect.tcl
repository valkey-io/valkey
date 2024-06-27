start_server {tags {needs:repl external:skip}} {
    start_server {} {
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]

        r replicaof $primary_host $primary_port
        wait_for_condition 50 100 {
            [s 0 master_link_status] eq {up}
        } else {
            fail "Replicas not replicating from primary"
        }

        test {replica allow read command by default} {
            r get foo
        } {}

        test {replica reply READONLY error for write command by default} {
            assert_error {READONLY*} {r set foo bar}
        }

        test {replica redirect read and write command after CLIENT CAPA REDIRECT} {
            r client capa redirect
            assert_error "REDIRECT $primary_host:$primary_port" {r set foo bar}
            assert_error "REDIRECT $primary_host:$primary_port" {r get foo}
        }

        test {non-data access commands are not redirected} {
            r ping
        } {PONG}

        test {replica allow read command in READONLY mode} {
            r readonly
            r get foo
        } {}
    }
}
