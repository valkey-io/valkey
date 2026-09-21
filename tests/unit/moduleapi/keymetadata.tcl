set testmodule [file normalize tests/modules/keymetadata.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {DUMP without keyword returns the serialized value} {
        r set k hello
        set payload [r dump k]
        assert {$payload ne {}}
    }

    test {DUMP RESTORE emits a replayable command without metadata} {
        r flushall
        r keymetadata.disarm
        r set k hello
        set cmd [r dump k RESTORE]
        assert_equal [lindex $cmd 0] {RESTORE}
        assert_equal [lindex $cmd 1] {k}
        assert_equal [lindex $cmd 4] {REPLACE}
        assert {[lsearch $cmd METADATA] == -1}
    }

    test {DUMP RESTORE round-trips module metadata to the restore callback} {
        r flushall
        r keymetadata.arm payload-abc
        r set k hello
        set cmd [r dump k RESTORE]
        set mi [lsearch $cmd METADATA]
        assert {$mi != -1}
        assert_equal [lindex $cmd [expr {$mi + 1}]] {keymetadata}
        assert_equal [lindex $cmd [expr {$mi + 2}]] {payload-abc}

        r del k
        assert_equal [r {*}$cmd] {OK}
        assert_equal [r get k] {hello}
        assert_equal [r keymetadata.lastkey] {k}
        assert_equal [r keymetadata.lastmeta] {payload-abc}
    }

    test {RESTORE ABSTTL is emitted when the key has an expire} {
        r flushall
        r keymetadata.disarm
        r set k hello
        r pexpireat k [expr {[clock milliseconds] + 100000}]
        set cmd [r dump k RESTORE]
        assert {[lsearch $cmd ABSTTL] != -1}
    }

    test {RESTORE drops metadata for an unknown module and still succeeds} {
        r flushall
        r set src hello
        set payload [r dump src]
        r del src
        assert_equal [r restore src 0 $payload METADATA nosuchmodule ignored] {OK}
        assert_equal [r get src] {hello}
    }

    test {RESTORE fails and rolls back when a module rejects metadata} {
        r flushall
        r set src hello
        set payload [r dump src]
        r del src
        catch {r restore src 0 $payload METADATA keymetadata REJECT} e
        assert_match {*rejected*} $e
        assert_equal [r exists src] {0}
    }

    test {RESTORE METADATA with an odd number of arguments is a syntax error} {
        r flushall
        r set src hello
        set payload [r dump src]
        r del src
        catch {r restore src 0 $payload METADATA keymetadata} e
        assert_match {*syntax*} $e
    }
}

start_server {tags {"modules"} overrides {appendonly yes}} {
    r module load $testmodule

    test {RESTORE with a relative TTL and METADATA propagates a replayable command} {
        r flushall
        r set src hello
        set payload [r dump src]
        r del src
        assert_equal [r restore src 100000 $payload METADATA keymetadata payload-abc] {OK}
        assert {[r pttl src] > 0}

        # ABSTTL must be inserted before METADATA, otherwise the propagated
        # command has an odd trailing pair count and fails to reload.
        r debug loadaof
        assert_equal [r get src] {hello}
        assert {[r pttl src] > 0}
    }

    test {RESTORE REPLACE rollback on rejected metadata is propagated} {
        r flushall
        r set src hello
        set payload [r dump src]
        r set src old-value
        catch {r restore src 0 $payload REPLACE METADATA keymetadata REJECT} e
        assert_match {*rejected*} $e
        assert_equal [r exists src] {0}

        # The rollback deleted a pre-existing value, so it has to propagate or
        # a reload resurrects it.
        r debug loadaof
        assert_equal [r exists src] {0}
    }
}
