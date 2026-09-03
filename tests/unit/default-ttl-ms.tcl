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

    test {default-ttl-ms rejects negatives without changing its value} {
        assert_error {*argument must be between 0 and*} {
            r config set default-ttl-ms -1
        }
        assert_equal {default-ttl-ms 10000} [r config get default-ttl-ms]
    }

    test {default-ttl-ms rejects a duration that cannot become an absolute expiry} {
        assert_error {*exceeds the maximum duration*} {
            r config set default-ttl-ms 9223372036854775807
        }
        assert_equal {default-ttl-ms 10000} [r config get default-ttl-ms]
    }

    test {explicit TTL, KEEPTTL, and PERSIST take precedence} {
        r set default-ttl-ms:explicit value PX 30000
        set ttl [r pttl default-ttl-ms:explicit]
        assert {$ttl > 29000 && $ttl <= 30000}

        set before [r pexpiretime default-ttl-ms:explicit]
        r set default-ttl-ms:explicit replacement KEEPTTL
        assert_equal $before [r pexpiretime default-ttl-ms:explicit]

        r msetex 1 default-ttl-ms:persistent value PERSIST
        assert_equal -1 [r pttl default-ttl-ms:persistent]
    }
}
