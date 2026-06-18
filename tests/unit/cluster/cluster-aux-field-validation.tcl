# Tests for cluster AUX field control-character and delimiter rejection.
# Validates that isValidAuxChar rejects characters that could corrupt
# nodes.conf or enable injection attacks.

start_cluster 1 0 {tags {external:skip cluster}} {
    test "cluster-announce-ip rejects control characters (newline)" {
        assert_error "ERR CONFIG SET failed*cluster-announce-ip*invalid character*" {
            R 0 CONFIG SET cluster-announce-ip "10.0.0.1\n"
        }
    }

    test "cluster-announce-ip rejects control characters (carriage return)" {
        assert_error "ERR CONFIG SET failed*cluster-announce-ip*invalid character*" {
            R 0 CONFIG SET cluster-announce-ip "10.0.0.1\r"
        }
    }

    test "cluster-announce-ip rejects control characters (null byte)" {
        assert_error "ERR CONFIG SET failed*cluster-announce-ip*invalid character*" {
            R 0 CONFIG SET cluster-announce-ip "10.0.0.1\x00"
        }
    }

    test "cluster-announce-ip rejects control characters (tab)" {
        assert_error "ERR CONFIG SET failed*cluster-announce-ip*invalid character*" {
            R 0 CONFIG SET cluster-announce-ip "10.0.0.1\t"
        }
    }

    test "cluster-announce-ip rejects space" {
        assert_error "ERR CONFIG SET failed*cluster-announce-ip*invalid character*" {
            R 0 CONFIG SET cluster-announce-ip "10.0.0.1 injected"
        }
    }

    test "cluster-announce-ip rejects comma" {
        assert_error "ERR CONFIG SET failed*cluster-announce-ip*invalid character*" {
            R 0 CONFIG SET cluster-announce-ip "10.0.0.1,evil"
        }
    }

    test "cluster-announce-ip rejects equals sign" {
        assert_error "ERR CONFIG SET failed*cluster-announce-ip*invalid character*" {
            R 0 CONFIG SET cluster-announce-ip "10.0.0.1=evil"
        }
    }

    test "cluster-announce-ip rejects double quote" {
        assert_error "ERR CONFIG SET failed*cluster-announce-ip*invalid character*" {
            R 0 CONFIG SET cluster-announce-ip "10.0.0.1\"evil"
        }
    }

    test "cluster-announce-ip rejects single quote" {
        assert_error "ERR CONFIG SET failed*cluster-announce-ip*invalid character*" {
            R 0 CONFIG SET cluster-announce-ip "10.0.0.1'evil"
        }
    }

    test "cluster-announce-ip rejects backslash" {
        assert_error "ERR CONFIG SET failed*cluster-announce-ip*invalid character*" {
            R 0 CONFIG SET cluster-announce-ip "10.0.0.1\\evil"
        }
    }

    test "cluster-announce-ip accepts valid IPv4" {
        R 0 CONFIG SET cluster-announce-ip "192.168.1.100"
        assert_equal "192.168.1.100" [lindex [R 0 CONFIG GET cluster-announce-ip] 1]
    }

    test "cluster-announce-ip accepts valid IPv6" {
        R 0 CONFIG SET cluster-announce-ip "::1"
        assert_equal "::1" [lindex [R 0 CONFIG GET cluster-announce-ip] 1]
    }

    test "cluster-announce-ip rejects value exceeding length limit" {
        assert_error "ERR CONFIG SET failed*cluster-announce-ip*too long*" {
            R 0 CONFIG SET cluster-announce-ip [string repeat "a" 100]
        }
    }

    test "cluster-announce-ip accepts hyphen, dot, colon, slash, underscore" {
        R 0 CONFIG SET cluster-announce-ip "my-host_1.example:8080/path"
        assert_equal "my-host_1.example:8080/path" [lindex [R 0 CONFIG GET cluster-announce-ip] 1]
    }

    test "cluster-announce-ip accepts high-byte UTF-8 characters" {
        R 0 CONFIG SET cluster-announce-ip "\xc3\xa9"
        assert_equal "\xc3\xa9" [lindex [R 0 CONFIG GET cluster-announce-ip] 1]
    }

    test "cluster-announce-human-nodename rejects control characters" {
        assert_error "ERR CONFIG SET failed*cluster-announce-human-nodename*invalid character*" {
            R 0 CONFIG SET cluster-announce-human-nodename "node\ninjected"
        }
    }

    test "cluster-announce-human-nodename rejects space" {
        assert_error "ERR CONFIG SET failed*cluster-announce-human-nodename*invalid character*" {
            R 0 CONFIG SET cluster-announce-human-nodename "node injected"
        }
    }

    test "cluster-announce-human-nodename rejects comma" {
        assert_error "ERR CONFIG SET failed*cluster-announce-human-nodename*invalid character*" {
            R 0 CONFIG SET cluster-announce-human-nodename "node,injected"
        }
    }

    test "cluster-announce-human-nodename rejects equals sign" {
        assert_error "ERR CONFIG SET failed*cluster-announce-human-nodename*invalid character*" {
            R 0 CONFIG SET cluster-announce-human-nodename "node=injected"
        }
    }

    test "cluster-announce-human-nodename rejects double quote" {
        assert_error "ERR CONFIG SET failed*cluster-announce-human-nodename*invalid character*" {
            R 0 CONFIG SET cluster-announce-human-nodename "node\"injected"
        }
    }

    test "cluster-announce-human-nodename rejects single quote" {
        assert_error "ERR CONFIG SET failed*cluster-announce-human-nodename*invalid character*" {
            R 0 CONFIG SET cluster-announce-human-nodename "node'injected"
        }
    }

    test "cluster-announce-human-nodename rejects backslash" {
        assert_error "ERR CONFIG SET failed*cluster-announce-human-nodename*invalid character*" {
            R 0 CONFIG SET cluster-announce-human-nodename "node\\injected"
        }
    }
}
