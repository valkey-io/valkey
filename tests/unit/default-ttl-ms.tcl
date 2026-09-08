start_server {tags {expire}} {
    test {default-ttl-ms is disabled by default} {
        assert_equal {default-ttl-ms 0} [r config get default-ttl-ms]
        r set default-ttl-ms:disabled value
        assert_equal -1 [r pttl default-ttl-ms:disabled]
    }

    test {default-ttl-ms applies only to later SET writes} {
        r set default-ttl-ms:old value
        r config set default-ttl-ms 10000
        assert_equal -1 [r pttl default-ttl-ms:old]
        r set default-ttl-ms:new value
        set ttl [r pttl default-ttl-ms:new]
        assert {$ttl > 9000 && $ttl <= 10000}
    }

    test {default-ttl-ms applies to successful SET NX GET} {
        assert_equal {} [r set default-ttl-ms:nx-get value NX GET]
        assert_equal value [r get default-ttl-ms:nx-get]
        set ttl [r pttl default-ttl-ms:nx-get]
        assert {$ttl > 9000 && $ttl <= 10000}
    }

    test {default-ttl-ms rejects negatives without changing its value} {
        assert_error {*argument must be between 0 and*} {
            r config set default-ttl-ms -1
        }
        assert_equal {default-ttl-ms 10000} [r config get default-ttl-ms]
    }

    test {default-ttl-ms accepts LLONG_MAX/2 and rejects the next value} {
        assert_equal OK [r config set default-ttl-ms 4611686018427387903]
        assert_error {*argument must be between 0 and 4611686018427387903 inclusive*} {
            r config set default-ttl-ms 4611686018427387904
        }
        assert_equal {default-ttl-ms 4611686018427387903} [r config get default-ttl-ms]
        r config set default-ttl-ms 10000
    }

    test {explicit TTL and KEEPTTL take precedence over default-ttl-ms} {
        r set default-ttl-ms:explicit value PX 30000
        set ttl [r pttl default-ttl-ms:explicit]
        assert {$ttl > 29000 && $ttl <= 30000}

        set before [r pexpiretime default-ttl-ms:explicit]
        r set default-ttl-ms:explicit replacement KEEPTTL
        assert_equal $before [r pexpiretime default-ttl-ms:explicit]
    }
}
