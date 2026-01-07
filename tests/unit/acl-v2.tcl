start_server {tags {"acl external:skip"}} {
    set r2 [valkey_client]
    test {Test basic multiple selectors} {
        r ACL SETUSER selector-1 on -@all resetkeys nopass
        $r2 auth selector-1 password
        catch {$r2 ping} err
        assert_match "*NOPERM*command*" $err
        catch {$r2 set write::foo bar} err
        assert_match "*NOPERM*command*" $err
        catch {$r2 get read::foo} err
        assert_match "*NOPERM*command*" $err

        r ACL SETUSER selector-1 (+@write ~write::*) (+@read ~read::*)
        catch {$r2 ping} err
        assert_equal "OK" [$r2 set write::foo bar]
        assert_equal "" [$r2 get read::foo]
        catch {$r2 get write::foo} err
        assert_match "*NOPERM*key*" $err
        catch {$r2 set read::foo bar} err
        assert_match "*NOPERM*key*" $err
    }

    test {Test ACL selectors by default have no permissions} {
        r ACL SETUSER selector-default reset ()
        set user [r ACL GETUSER "selector-default"]
        assert_equal 1 [llength [dict get $user selectors]]
        assert_equal "" [dict get [lindex [dict get $user selectors] 0] keys]
        assert_equal "" [dict get [lindex [dict get $user selectors] 0] channels]
        assert_equal "-@all" [dict get [lindex [dict get $user selectors] 0] commands]
    }

    test {Test deleting selectors} {
        r ACL SETUSER selector-del on "(~added-selector)"
        set user [r ACL GETUSER "selector-del"]
        assert_equal "~added-selector" [dict get [lindex [dict get $user selectors] 0] keys]
        assert_equal [llength [dict get $user selectors]] 1

        r ACL SETUSER selector-del clearselectors
        set user [r ACL GETUSER "selector-del"]
        assert_equal [llength [dict get $user selectors]] 0
    }

    test {Test selector syntax error reports the error in the selector context} {
        catch {r ACL SETUSER selector-syntax on (this-is-invalid)} e
        assert_match "ERR Error in ACL SETUSER modifier '(*)*Syntax*" $e

        catch {r ACL SETUSER selector-syntax on (&* &fail)} e
        assert_match "ERR Error in ACL SETUSER modifier '(*)*Adding a pattern after the*" $e

        catch {r ACL SETUSER selector-syntax on (+PING (+SELECT (+DEL} e
        assert_match "ERR Unmatched parenthesis in acl selector*" $e

        catch {r ACL SETUSER selector-syntax on (+PING (+SELECT (+DEL ) ) ) } e
        assert_match "ERR Error in ACL SETUSER modifier*" $e

        catch {r ACL SETUSER selector-syntax on (+PING (+SELECT (+DEL ) } e
        assert_match "ERR Error in ACL SETUSER modifier*" $e

        assert_equal "" [r ACL GETUSER selector-syntax]
    }

    test {Test flexible selector definition} {
        # Test valid selectors
        r ACL SETUSER selector-2 "(~key1 +get )" "( ~key2 +get )" "( ~key3 +get)" "(~key4 +get)"
        r ACL SETUSER selector-2 (~key5 +get ) ( ~key6 +get ) ( ~key7 +get) (~key8 +get)
        set user [r ACL GETUSER "selector-2"]
        assert_equal "~key1" [dict get [lindex [dict get $user selectors] 0] keys]
        assert_equal "~key2" [dict get [lindex [dict get $user selectors] 1] keys]
        assert_equal "~key3" [dict get [lindex [dict get $user selectors] 2] keys]
        assert_equal "~key4" [dict get [lindex [dict get $user selectors] 3] keys]
        assert_equal "~key5" [dict get [lindex [dict get $user selectors] 4] keys]
        assert_equal "~key6" [dict get [lindex [dict get $user selectors] 5] keys]
        assert_equal "~key7" [dict get [lindex [dict get $user selectors] 6] keys]
        assert_equal "~key8" [dict get [lindex [dict get $user selectors] 7] keys]

        # Test invalid selector syntax
        catch {r ACL SETUSER invalid-selector " () "} err
        assert_match "ERR*Syntax error*" $err
        catch {r ACL SETUSER invalid-selector (} err
        assert_match "ERR*Unmatched parenthesis*" $err
        catch {r ACL SETUSER invalid-selector )} err
        assert_match "ERR*Syntax error*" $err
    }

    test {Test separate read permission} {
        r ACL SETUSER key-permission-R on nopass %R~read* +@all
        $r2 auth key-permission-R password
        assert_equal PONG [$r2 PING]
        r set readstr bar
        assert_equal bar [$r2 get readstr]
        catch {$r2 set readstr bar} err
        assert_match "*NOPERM*key*" $err
        catch {$r2 get notread} err
        assert_match "*NOPERM*key*" $err
    }

    test {Test separate write permission} {
        r ACL SETUSER key-permission-W on nopass %W~write* +@all
        $r2 auth key-permission-W password
        assert_equal PONG [$r2 PING]
        # Note, SET is a RW command, so it's not used for testing
        $r2 LPUSH writelist 10
        catch {$r2 GET writestr} err
        assert_match "*NOPERM*key*" $err
        catch {$r2 LPUSH notwrite 10} err
        assert_match "*NOPERM*key*" $err
    }

    test {Test separate read and write permissions} {
        r ACL SETUSER key-permission-RW on nopass %R~read* %W~write* +@all
        $r2 auth key-permission-RW password
        assert_equal PONG [$r2 PING]
        r set read bar
        $r2 copy read write
        catch {$r2 copy write read} err
        assert_match "*NOPERM*key*" $err
    }

    test {Validate read and write permissions format} {
        # Regression tests for CVE-2024-51741
        assert_error "ERR Error in ACL SETUSER modifier '%~': Syntax error" {r ACL SETUSER invalid %~}
        assert_error "ERR Error in ACL SETUSER modifier '%': Syntax error" {r ACL SETUSER invalid %}
    }

    test {Validate key permissions format - empty and omitted pattern} {
        # Empty pattern results with access to only the empty key
        r ACL SETUSER key-permission-no-key on nopass %RW~ +@all
        assert_equal "User key-permission-no-key has no permissions to access the 'x' key" [r ACL DRYRUN key-permission-no-key GET x]
        assert_equal "OK" [r ACL DRYRUN key-permission-no-key GET ""]

        # This is incorrect syntax, it should have `~`, but we'll allow it for compatibility since it does something
        r ACL SETUSER key-permission-omit on nopass %RW +@all
        assert_equal "User key-permission-omit has no permissions to access the 'x' key" [r ACL DRYRUN key-permission-omit GET x]
        assert_equal "OK" [r ACL DRYRUN key-permission-omit GET ""]

        # Assert these two are equivalent 
        assert_equal [r ACL GETUSER key-permission-omit] [r ACL GETUSER key-permission-no-key]
    }

    test {Test separate read and write permissions on different selectors are not additive} {
        r ACL SETUSER key-permission-RW-selector on nopass "(%R~read* +@all)" "(%W~write* +@all)"
        $r2 auth key-permission-RW-selector password
        assert_equal PONG [$r2 PING]

        # Verify write selector
        $r2 LPUSH writelist 10
        catch {$r2 GET writestr} err
        assert_match "*NOPERM*key*" $err
        catch {$r2 LPUSH notwrite 10} err
        assert_match "*NOPERM*key*" $err

        # Verify read selector
        r set readstr bar
        assert_equal bar [$r2 get readstr]
        catch {$r2 set readstr bar} err
        assert_match "*NOPERM*key*" $err
        catch {$r2 get notread} err
        assert_match "*NOPERM*key*" $err

        # Verify they don't combine
        catch {$r2 copy read write} err
        assert_match "*NOPERM*key*" $err
        catch {$r2 copy write read} err
        assert_match "*NOPERM*key*" $err
    }

    test {Test SET with separate read permission} {
        r del readstr
        r ACL SETUSER set-key-permission-R on nopass %R~read* +@all
        $r2 auth set-key-permission-R password
        assert_equal PONG [$r2 PING]
        assert_equal {} [$r2 get readstr]

        # We don't have the permission to WRITE key.
        assert_error {*NOPERM*key*} {$r2 set readstr bar}
        assert_error {*NOPERM*key*} {$r2 set readstr bar get}
        assert_error {*NOPERM*key*} {$r2 set readstr bar ex 100}
        assert_error {*NOPERM*key*} {$r2 set readstr bar keepttl nx}
    }

    test {Test SET with separate write permission} {
        r del writestr
        r ACL SETUSER set-key-permission-W on nopass %W~write* +@all
        $r2 auth set-key-permission-W password
        assert_equal PONG [$r2 PING]
        assert_equal {OK} [$r2 set writestr bar]
        assert_equal {OK} [$r2 set writestr get]

        # We don't have the permission to READ key.
        assert_error {*NOPERM*key*} {$r2 set get writestr}
        assert_error {*NOPERM*key*} {$r2 set writestr bar get}
        assert_error {*NOPERM*key*} {$r2 set writestr bar get ex 100}
        assert_error {*NOPERM*key*} {$r2 set writestr bar get keepttl nx}

        # this probably should be `ERR value is not an integer or out of range`
        assert_error {*NOPERM*key*} {$r2 set writestr bar ex get}
    }

    test {Test SET with read and write permissions} {
        r del readwrite_str
        r ACL SETUSER set-key-permission-RW-selector on nopass %RW~readwrite* +@all
        $r2 auth set-key-permission-RW-selector password
        assert_equal PONG [$r2 PING]

        assert_equal {} [$r2 get readwrite_str]
        assert_error {ERR * not an integer *} {$r2 set readwrite_str bar ex get}

        assert_equal {OK} [$r2 set readwrite_str bar]
        assert_equal {bar} [$r2 get readwrite_str]

        assert_equal {bar} [$r2 set readwrite_str bar2 get]
        assert_equal {bar2} [$r2 get readwrite_str]

        assert_equal {bar2} [$r2 set readwrite_str bar3 get ex 10]
        assert_equal {bar3} [$r2 get readwrite_str]
        assert_range [$r2 ttl readwrite_str] 5 10
    }

    test {Test BITFIELD with separate read permission} {
        r del readstr
        r ACL SETUSER bitfield-key-permission-R on nopass %R~read* +@all
        $r2 auth bitfield-key-permission-R password
        assert_equal PONG [$r2 PING]
        assert_equal {0} [$r2 bitfield readstr get u4 0]

        # We don't have the permission to WRITE key.
        assert_error {*NOPERM*key*} {$r2 bitfield readstr set u4 0 1}
        assert_error {*NOPERM*key*} {$r2 bitfield readstr get u4 0 set u4 0 1}
        assert_error {*NOPERM*key*} {$r2 bitfield readstr incrby u4 0 1}
    }

    test {Test BITFIELD with separate write permission} {
        r del writestr
        r ACL SETUSER bitfield-key-permission-W on nopass %W~write* +@all
        $r2 auth bitfield-key-permission-W password
        assert_equal PONG [$r2 PING]

        # We don't have the permission to READ key.
        assert_error {*NOPERM*key*} {$r2 bitfield writestr get u4 0}
        assert_error {*NOPERM*key*} {$r2 bitfield writestr set u4 0 1}
        assert_error {*NOPERM*key*} {$r2 bitfield writestr incrby u4 0 1}
    }

    test {Test BITFIELD with read and write permissions} {
        r del readwrite_str
        r ACL SETUSER bitfield-key-permission-RW-selector on nopass %RW~readwrite* +@all
        $r2 auth bitfield-key-permission-RW-selector password
        assert_equal PONG [$r2 PING]

        assert_equal {0} [$r2 bitfield readwrite_str get u4 0]
        assert_equal {0} [$r2 bitfield readwrite_str set u4 0 1]
        assert_equal {2} [$r2 bitfield readwrite_str incrby u4 0 1]
        assert_equal {2} [$r2 bitfield readwrite_str get u4 0]
    }

    test {Test ACL log correctly identifies the relevant item when selectors are used} {
        r ACL SETUSER acl-log-test-selector on nopass
        r ACL SETUSER acl-log-test-selector +mget ~key (+mget ~key ~otherkey)
        $r2 auth acl-log-test-selector password

        # Test that command is shown only if none of the selectors match
        r ACL LOG RESET
        catch {$r2 GET key} err
        assert_match "*NOPERM*command*" $err
        set entry [lindex [r ACL LOG] 0]
        assert_equal [dict get $entry username] "acl-log-test-selector"
        assert_equal [dict get $entry context] "toplevel"
        assert_equal [dict get $entry reason] "command"
        assert_equal [dict get $entry object] "get"

        # Test two cases where the first selector matches less than the
        # second selector. We should still show the logically first unmatched key.
        r ACL LOG RESET
        catch {$r2 MGET otherkey someotherkey} err
        assert_match "*NOPERM*key*" $err
        set entry [lindex [r ACL LOG] 0]
        assert_equal [dict get $entry username] "acl-log-test-selector"
        assert_equal [dict get $entry context] "toplevel"
        assert_equal [dict get $entry reason] "key"
        assert_equal [dict get $entry object] "someotherkey"

        r ACL LOG RESET
        catch {$r2 MGET key otherkey someotherkey} err
        assert_match "*NOPERM*key*" $err
        set entry [lindex [r ACL LOG] 0]
        assert_equal [dict get $entry username] "acl-log-test-selector"
        assert_equal [dict get $entry context] "toplevel"
        assert_equal [dict get $entry reason] "key"
        assert_equal [dict get $entry object] "someotherkey"
    }

    test {Test ACL GETUSER response information} {
        r ACL setuser selector-info -@all +get resetchannels &channel1 %R~foo1 %W~bar1 ~baz1
        r ACL setuser selector-info (-@all +set resetchannels &channel2 %R~foo2 %W~bar2 ~baz2)
        set user [r ACL GETUSER "selector-info"]

        # Root selector
        assert_equal "%R~foo1 %W~bar1 ~baz1" [dict get $user keys]
        assert_equal "&channel1" [dict get $user channels]
        assert_equal "-@all +get" [dict get $user commands]

        # Added selector
        set secondary_selector [lindex [dict get $user selectors] 0]
        assert_equal "%R~foo2 %W~bar2 ~baz2" [dict get $secondary_selector keys]
        assert_equal "&channel2" [dict get $secondary_selector channels]
        assert_equal "-@all +set" [dict get $secondary_selector commands]
    }

    test {Test ACL GETUSER response with database permissions} {
        r ACL setuser db-getuser-test on nopass -@all +get ~* db=0,1,2
        set user [r ACL GETUSER "db-getuser-test"]
        
        assert_equal "db=0,1,2" [dict get $user databases]
        assert_equal "-@all +get" [dict get $user commands]
        
        r ACL setuser db-getuser-alldbs on nopass -@all +get ~* alldbs
        set user [r ACL GETUSER "db-getuser-alldbs"]
        assert_equal "alldbs" [dict get $user databases]
        
        r ACL setuser db-getuser-resetdbs on nopass -@all +get ~* resetdbs
        set user [r ACL GETUSER "db-getuser-resetdbs"]
        assert_equal "" [dict get $user databases]
        
        r ACL setuser db-getuser-selector on nopass db=0,1 -@all +get ~* (db=2,3 -@all +set ~*)
        set user [r ACL GETUSER "db-getuser-selector"]
        
        assert_equal "db=0,1" [dict get $user databases]
        
        set secondary_selector [lindex [dict get $user selectors] 0]
        assert_equal "db=2,3" [dict get $secondary_selector databases]
        assert_equal "-@all +set" [dict get $secondary_selector commands]
        
        r ACL setuser db-getuser-selector-alldbs on nopass db=0 -@all +get ~* (alldbs -@all +set ~*)
        set user [r ACL GETUSER "db-getuser-selector-alldbs"]
        
        assert_equal "db=0" [dict get $user databases]
        
        set secondary_selector [lindex [dict get $user selectors] 0]
        assert_equal "alldbs" [dict get $secondary_selector databases]
        
        r ACL setuser db-getuser-selector-reset on nopass db=0,1 -@all +get ~* (resetdbs -@all +set ~*)
        set user [r ACL GETUSER "db-getuser-selector-reset"]
        
        assert_equal "db=0,1" [dict get $user databases]
        
        set secondary_selector [lindex [dict get $user selectors] 0]
        assert_equal "" [dict get $secondary_selector databases]
    }


    test {Test ACL list idempotency} {
        r ACL SETUSER user-idempotency off -@all +get resetchannels &channel1 %R~foo1 %W~bar1 ~baz1 (-@all +set resetchannels &channel2 %R~foo2 %W~bar2 ~baz2)
        set response [lindex [r ACL LIST] [lsearch [r ACL LIST] "user user-idempotency*"]]

        assert_match "*-@all*+get*(*)*" $response
        assert_match "*resetchannels*&channel1*(*)*" $response
        assert_match "*%R~foo1*%W~bar1*~baz1*(*)*" $response

        assert_match "*(*-@all*+set*)*" $response
        assert_match "*(*resetchannels*&channel2*)*" $response
        assert_match "*(*%R~foo2*%W~bar2*~baz2*)*" $response
    }

    test {Test R+W is the same as all permissions} {
        r ACL setuser selector-rw-info %R~foo %W~foo %RW~bar
        set user [r ACL GETUSER selector-rw-info]
        assert_equal "~foo ~bar" [dict get $user keys]
    }

    test {Test basic dry run functionality} {
        r ACL setuser command-test +@all %R~read* %W~write* %RW~rw*
        assert_equal "OK" [r ACL DRYRUN command-test GET read]

        catch {r ACL DRYRUN not-a-user GET read} e
        assert_equal "ERR User 'not-a-user' not found" $e

        catch {r ACL DRYRUN command-test not-a-command read} e
        assert_equal "ERR Command 'not-a-command' not found" $e
    }

    test {Test various commands for command permissions} {
        r ACL setuser command-test -@all
        assert_match {*has no permissions to run the 'set' command*} [r ACL DRYRUN command-test set somekey somevalue]
        assert_match {*has no permissions to run the 'get' command*} [r ACL DRYRUN command-test get somekey]
    }

    test {Test various odd commands for key permissions} {
        r ACL setuser command-test +@all %R~read* %W~write* %RW~rw*

        # Test migrate, which is marked with incomplete keys
        assert_equal "OK" [r ACL DRYRUN command-test MIGRATE whatever whatever rw 0 500]
        assert_match {*has no permissions to access the 'read' key*} [r ACL DRYRUN command-test MIGRATE whatever whatever read 0 500]
        assert_match {*has no permissions to access the 'write' key*} [r ACL DRYRUN command-test MIGRATE whatever whatever write 0 500]
        assert_equal "OK" [r ACL DRYRUN command-test MIGRATE whatever whatever "" 0 5000 KEYS rw]
        assert_match "*has no permissions to access the 'read' key" [r ACL DRYRUN command-test MIGRATE whatever whatever "" 0 5000 KEYS read]
        assert_match "*has no permissions to access the 'write' key" [r ACL DRYRUN command-test MIGRATE whatever whatever "" 0 5000 KEYS write]
        assert_equal "OK" [r ACL DRYRUN command-test MIGRATE whatever whatever "" 0 5000 AUTH KEYS KEYS rw]
        assert_match "*has no permissions to access the 'read' key" [r ACL DRYRUN command-test MIGRATE whatever whatever "" 0 5000 AUTH KEYS KEYS read]
        assert_match "*has no permissions to access the 'write' key" [r ACL DRYRUN command-test MIGRATE whatever whatever "" 0 5000 AUTH KEYS KEYS write]
        assert_equal "OK" [r ACL DRYRUN command-test MIGRATE whatever whatever "" 0 5000 AUTH2 KEYS 123 KEYS rw]
        assert_match "*has no permissions to access the 'read' key" [r ACL DRYRUN command-test MIGRATE whatever whatever "" 0 5000 AUTH2 KEYS 123 KEYS read]
        assert_match "*has no permissions to access the 'write' key" [r ACL DRYRUN command-test MIGRATE whatever whatever "" 0 5000 AUTH2 KEYS 123 KEYS write]
        assert_equal "OK" [r ACL DRYRUN command-test MIGRATE whatever whatever "" 0 5000 AUTH2 USER KEYS KEYS rw]
        assert_match "*has no permissions to access the 'read' key" [r ACL DRYRUN command-test MIGRATE whatever whatever "" 0 5000 AUTH2 USER KEYS KEYS read]
        assert_match "*has no permissions to access the 'write' key" [r ACL DRYRUN command-test MIGRATE whatever whatever "" 0 5000 AUTH2 USER KEYS KEYS write]

        # Test SORT, which is marked with incomplete keys
        assert_equal "OK" [r ACL DRYRUN command-test SORT read STORE write]
        assert_match {*has no permissions to access the 'read' key*}  [r ACL DRYRUN command-test SORT read STORE read]
        assert_match {*has no permissions to access the 'write' key*}  [r ACL DRYRUN command-test SORT write STORE write]

        # Test EVAL, which uses the numkey keyspec (Also test EVAL_RO)
        assert_equal "OK" [r ACL DRYRUN command-test EVAL "" 1 rw1]
        assert_match {*has no permissions to access the 'read' key*} [r ACL DRYRUN command-test EVAL "" 1 read]
        assert_equal "OK" [r ACL DRYRUN command-test EVAL_RO "" 1 rw1]
        assert_equal "OK" [r ACL DRYRUN command-test EVAL_RO "" 1 read]

        # Read is an optional argument and not a key here, make sure we don't treat it as a key
        assert_equal "OK" [r ACL DRYRUN command-test EVAL "" 0 read]

        # These are syntax errors, but it's 'OK' from an ACL perspective
        assert_equal "OK" [r ACL DRYRUN command-test EVAL "" -1 read]
        assert_equal "OK" [r ACL DRYRUN command-test EVAL "" 3 rw rw]
        assert_equal "OK" [r ACL DRYRUN command-test EVAL "" 3 rw read]

        # Test GEORADIUS which uses the last type of keyspec, keyword
        assert_equal "OK" [r ACL DRYRUN command-test GEORADIUS read longitude latitude radius M STOREDIST write]
        assert_equal "OK" [r ACL DRYRUN command-test GEORADIUS read longitude latitude radius M]
        assert_match {*has no permissions to access the 'read2' key*} [r ACL DRYRUN command-test GEORADIUS read1 longitude latitude radius M STOREDIST read2]
        assert_match {*has no permissions to access the 'write1' key*} [r ACL DRYRUN command-test GEORADIUS write1 longitude latitude radius M STOREDIST write2]
        assert_equal "OK" [r ACL DRYRUN command-test GEORADIUS read longitude latitude radius M STORE write]
        assert_equal "OK" [r ACL DRYRUN command-test GEORADIUS read longitude latitude radius M]
        assert_match {*has no permissions to access the 'read2' key*} [r ACL DRYRUN command-test GEORADIUS read1 longitude latitude radius M STORE read2]
        assert_match {*has no permissions to access the 'write1' key*} [r ACL DRYRUN command-test GEORADIUS write1 longitude latitude radius M STORE write2]
    }

    # Existence test commands are not marked as access since they are the result
    # of a lot of write commands. We therefore make the claim they can be executed
    # when either READ or WRITE flags are provided.
    test {Existence test commands are not marked as access} {
        assert_equal "OK" [r ACL DRYRUN command-test HEXISTS read foo]
        assert_equal "OK" [r ACL DRYRUN command-test HEXISTS write foo]
        assert_match {*has no permissions to access the 'nothing' key*} [r ACL DRYRUN command-test HEXISTS nothing foo]

        assert_equal "OK" [r ACL DRYRUN command-test HSTRLEN read foo]
        assert_equal "OK" [r ACL DRYRUN command-test HSTRLEN write foo]
        assert_match {*has no permissions to access the 'nothing' key*} [r ACL DRYRUN command-test HSTRLEN nothing foo]

        assert_equal "OK" [r ACL DRYRUN command-test SISMEMBER read foo]
        assert_equal "OK" [r ACL DRYRUN command-test SISMEMBER write foo]
        assert_match {*has no permissions to access the 'nothing' key*} [r ACL DRYRUN command-test SISMEMBER nothing foo]
    }

    # Unlike existence test commands, intersection cardinality commands process the data
    # between keys and return an aggregated cardinality. therefore they have the access
    # requirement.
    test {Intersection cardinaltiy commands are access commands} {
        assert_equal "OK" [r ACL DRYRUN command-test SINTERCARD 2 read read]
        assert_match {*has no permissions to access the 'write' key*} [r ACL DRYRUN command-test SINTERCARD 2 write read]
        assert_match {*has no permissions to access the 'nothing' key*} [r ACL DRYRUN command-test SINTERCARD 2 nothing read]

        assert_equal "OK" [r ACL DRYRUN command-test ZCOUNT read 0 1]
        assert_match {*has no permissions to access the 'write' key*} [r ACL DRYRUN command-test ZCOUNT write 0 1]
        assert_match {*has no permissions to access the 'nothing' key*} [r ACL DRYRUN command-test ZCOUNT nothing 0 1]

        assert_equal "OK" [r ACL DRYRUN command-test PFCOUNT read read]
        assert_match {*has no permissions to access the 'write' key*} [r ACL DRYRUN command-test PFCOUNT write read]
        assert_match {*has no permissions to access the 'nothing' key*} [r ACL DRYRUN command-test PFCOUNT nothing read]

        assert_equal "OK" [r ACL DRYRUN command-test ZINTERCARD 2 read read]
        assert_match {*has no permissions to access the 'write' key*} [r ACL DRYRUN command-test ZINTERCARD 2 write read]
        assert_match {*has no permissions to access the 'nothing' key*} [r ACL DRYRUN command-test ZINTERCARD 2 nothing read]
    }

    test {Test general keyspace commands require some type of permission to execute} {
        assert_equal "OK" [r ACL DRYRUN command-test touch read]
        assert_equal "OK" [r ACL DRYRUN command-test touch write]
        assert_equal "OK" [r ACL DRYRUN command-test touch rw]
        assert_match {*has no permissions to access the 'nothing' key*} [r ACL DRYRUN command-test touch nothing]

        assert_equal "OK" [r ACL DRYRUN command-test exists read]
        assert_equal "OK" [r ACL DRYRUN command-test exists write]
        assert_equal "OK" [r ACL DRYRUN command-test exists rw]
        assert_match {*has no permissions to access the 'nothing' key*} [r ACL DRYRUN command-test exists nothing]

        assert_equal "OK" [r ACL DRYRUN command-test MEMORY USAGE read]
        assert_equal "OK" [r ACL DRYRUN command-test MEMORY USAGE write]
        assert_equal "OK" [r ACL DRYRUN command-test MEMORY USAGE rw]
        assert_match {*has no permissions to access the 'nothing' key*} [r ACL DRYRUN command-test MEMORY USAGE nothing]

        assert_equal "OK" [r ACL DRYRUN command-test TYPE read]
        assert_equal "OK" [r ACL DRYRUN command-test TYPE write]
        assert_equal "OK" [r ACL DRYRUN command-test TYPE rw]
        assert_match {*has no permissions to access the 'nothing' key*} [r ACL DRYRUN command-test TYPE nothing]
    }

    test {Cardinality commands require some type of permission to execute} {
        set commands {STRLEN HLEN LLEN SCARD ZCARD XLEN}
        foreach command $commands {
            assert_equal "OK" [r ACL DRYRUN command-test $command read]
            assert_equal "OK" [r ACL DRYRUN command-test $command write]
            assert_equal "OK" [r ACL DRYRUN command-test $command rw]
            assert_match {*has no permissions to access the 'nothing' key*} [r ACL DRYRUN command-test $command nothing]
        }
    }

    test {Test sharded channel permissions} {
        r ACL setuser test-channels +@all resetchannels &channel
        assert_equal "OK" [r ACL DRYRUN test-channels spublish channel foo]
        assert_equal "OK" [r ACL DRYRUN test-channels ssubscribe channel]
        assert_equal "OK" [r ACL DRYRUN test-channels sunsubscribe]
        assert_equal "OK" [r ACL DRYRUN test-channels sunsubscribe channel]
        assert_equal "OK" [r ACL DRYRUN test-channels sunsubscribe otherchannel]

        assert_match {*has no permissions to access the 'otherchannel' channel*} [r ACL DRYRUN test-channels spublish otherchannel foo]
        assert_match {*has no permissions to access the 'otherchannel' channel*} [r ACL DRYRUN test-channels ssubscribe otherchannel foo]
    }

    test {Test sort with ACL permissions} {
        r set v1 1
        r lpush mylist 1
        
        r ACL setuser test-sort-acl on nopass (+sort ~mylist)   
        $r2 auth test-sort-acl nopass
         
        catch {$r2 sort mylist by v*} e
        assert_equal "ERR BY option of SORT denied due to insufficient ACL permissions." $e
        catch {$r2 sort mylist get v*} e
        assert_equal "ERR GET option of SORT denied due to insufficient ACL permissions." $e 
        
        r ACL setuser test-sort-acl (+sort ~mylist ~v*)     
        catch {$r2 sort mylist by v*} e
        assert_equal "ERR BY option of SORT denied due to insufficient ACL permissions." $e  
        catch {$r2 sort mylist get v*} e
        assert_equal "ERR GET option of SORT denied due to insufficient ACL permissions." $e 
        
        r ACL setuser test-sort-acl (+sort ~mylist %W~*)     
        catch {$r2 sort mylist by v*} e
        assert_equal "ERR BY option of SORT denied due to insufficient ACL permissions." $e
        catch {$r2 sort mylist get v*} e
        assert_equal "ERR GET option of SORT denied due to insufficient ACL permissions." $e
       
        r ACL setuser test-sort-acl (+sort ~mylist %R~*)     
        assert_equal "1" [$r2 sort mylist by v*]     
        
        # cleanup
        r ACL deluser test-sort-acl
        r del v1 mylist
    }
    
    test {Test DRYRUN with wrong number of arguments} {
        r ACL setuser test-dry-run +@all ~v*
        
        assert_equal "OK" [r ACL DRYRUN test-dry-run SET v v]
        
        catch {r ACL DRYRUN test-dry-run SET v} e
        assert_equal "ERR wrong number of arguments for 'set' command" $e
        
        catch {r ACL DRYRUN test-dry-run SET} e
        assert_equal "ERR wrong number of arguments for 'set' command" $e
    }

    $r2 close
}

set server_path [tmpdir "selectors.acl"]
exec cp -f tests/assets/userwithselectors.acl $server_path
exec cp -f tests/assets/default.conf $server_path
start_server [list overrides [list "dir" $server_path "aclfile" "userwithselectors.acl"] tags [list "external:skip"]] {

    test {Test behavior of loading ACLs} {
        set selectors [dict get [r ACL getuser alice] selectors]
        assert_equal [llength $selectors] 1
        set test_selector [lindex $selectors 0]
        assert_equal "-@all +get" [dict get $test_selector "commands"]
        assert_equal "~rw*" [dict get $test_selector "keys"]

        set selectors [dict get [r ACL getuser bob] selectors]
        assert_equal [llength $selectors] 2
        set test_selector [lindex $selectors 0]
        assert_equal "-@all +set" [dict get $test_selector "commands"]
        assert_equal "%W~w*" [dict get $test_selector "keys"]

        set test_selector [lindex $selectors 1]
        assert_equal "-@all +get" [dict get $test_selector "commands"]
        assert_equal "%R~r*" [dict get $test_selector "keys"]
    }
}

start_server {tags {"acl external:skip"}} {
    set r2 [valkey_client]
    
    test {Test basic database-level ACL functionality} {
        r ACL SETUSER db-user on +@all nopass ~* db=0,1
        $r2 auth db-user password
        
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 set key1 value1]
        
        assert_equal "OK" [$r2 select 1]
        assert_equal "OK" [$r2 set key2 value2]
        
        catch {$r2 select 2} err
        assert_match "*NOPERM*database*" $err
    }
    
    test {Test database permissions with selectors} {
        r ACL SETUSER db-selector on nopass (db=0,1 +@all -@read ~write*) (db=2,3 +@all -@write ~read*)
        $r2 auth db-selector password
        
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 set writekey value]
        catch {$r2 get writekey} err
        assert_match "*NOPERM*command*" $err
        
        assert_equal "OK" [$r2 select 1]
        assert_equal "OK" [$r2 set writekey value]
        
        assert_equal "OK" [$r2 select 2]
        catch {$r2 set write:key value} err
        assert_match "*NOPERM*command*" $err
        r select 2
        r set readstr bar
        assert_equal [$r2 get readstr] bar
        
        assert_equal "OK" [$r2 select 3]
        r select 3
        r set readstr bar
        assert_equal [$r2 get readstr] bar
        
        catch {$r2 select 4} err
        assert_match "*NOPERM*command*" $err
    }
    
    test {Test alldbs and resetdbs commands} {
        r ACL SETUSER db-reset-user on +@all ~* nopass db=0
        $r2 auth db-reset-user password
        
        assert_equal "OK" [$r2 select 0]
        catch {$r2 select 1} err
        assert_match "*NOPERM*database*" $err
        
        r ACL SETUSER db-reset-user resetdbs

        catch {$r2 select 0} err
        assert_match "*NOPERM*database*" $err
        catch {$r2 select 1} err
        assert_match "*NOPERM*database*" $err
        catch {$r2 select 2} err
        assert_match "*NOPERM*database*" $err
        
        r ACL SETUSER db-reset-user alldbs
        
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 select 1]
    }
    
    test {Test transaction with database switching} {
        r ACL SETUSER db-tx-user on nopass (db=0,1 +@all -@read ~*) (db=2,3 +@all -@write ~*)
        $r2 auth db-tx-user password
        
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 multi]
        assert_equal "QUEUED" [$r2 set key1 value1]
        assert_equal "QUEUED" [$r2 select 2]
        assert_equal "QUEUED" [$r2 get key2]
        assert_equal "QUEUED" [$r2 select 0]
        assert_equal "QUEUED" [$r2 set key3 value3]
        
        set result [$r2 exec]
        assert_equal 5 [llength $result]
        
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 multi]
        assert_equal "QUEUED" [$r2 set key1 value1]
        catch {$r2 select 4} err
        assert_match "*NOPERM*command*" $err
        
        catch {$r2 exec} err
        assert_match "*EXECABORT*" $err
    }
    
    test {Test transaction with command permissions in different DBs} {
        r ACL SETUSER db-cmd-user on nopass (db=0,1 +@all -@read ~*) (db=2,3 +@all -@write ~*)
        $r2 auth db-cmd-user password
        
        assert_equal "OK" [$r2 select 0]
        catch {$r2 get key} err
        assert_match "*NOPERM*command*" $err
        
        assert_equal "OK" [$r2 select 2]
        catch {$r2 set key value} err
        assert_match "*NOPERM*command*" $err
        
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 multi]
        assert_equal "QUEUED" [$r2 set key1 value1]
        assert_equal "QUEUED" [$r2 select 2]
        assert_equal "QUEUED" [$r2 get key2]
        

        set result [$r2 exec]
        assert_equal 3 [llength $result]
    }
    
    test {Test database ACL with string representation} {
        r ACL SETUSER db-string-user on +@all nopass ~* db=0,1
        
        set acl_str [r ACL LIST]
        set user_line [lsearch -inline $acl_str "user db-string-user*"]
        
        assert_match "*db=0,1*" $user_line
    }
    
    test {Test edge cases with database IDs, check new errmsg} {
        catch {r ACL SETUSER db-edge-user db=-1} err
        assert_match "*Error in ACL SETUSER modifier 'db=-1': The provided database ID is out of range*" $err
        
        catch {r ACL SETUSER db-edge-user db=abc} err
        assert_match "*Error in ACL SETUSER modifier 'db=abc': Syntax error*" $err

        catch {r ACL SETUSER db-edge-user db=12345678987654321} err
        assert_match "*Error in ACL SETUSER modifier 'db=12345678987654321': The provided database ID is out of range*" $err
        
        catch {r ACL SETUSER db-edge-user db=non-numeric1,non-numeric2} err
        assert_match "*Error in ACL SETUSER modifier 'db=non-numeric1,non-numeric2': Syntax error*" $err
        
        set acl_list [r ACL LIST]
        set user_line [lsearch -inline $acl_list "user db-edge-user*"]
        assert_equal "" $user_line
    }
    
    test {Test default behavior without db restrictions} {
        r ACL SETUSER db-compat-user on >password +@all 
        $r2 auth db-compat-user password
        
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 select 1]
        assert_equal "OK" [$r2 select 2]
        assert_equal "OK" [$r2 select 3]
        
        r ACL SETUSER db-compat-user2 on >password +@all db=0
        r ACL SETUSER db-compat-user2 reset
        r ACL SETUSER db-compat-user2 on >password +@all
        $r2 auth db-compat-user2 password
        
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 select 1]
        assert_equal "OK" [$r2 select 2]
        assert_equal "OK" [$r2 select 3]
    }

    test {Test FLUSHALL with database permissions} {
        r ACL SETUSER flushall-user on nopass +@all ~* db=0
        $r2 auth flushall-user password
        $r2 select 0
        $r2 set key value
        
        catch {$r2 flushall} e
        assert_match "*NOPERM*database*" $e
        
        assert_equal "OK" [$r2 flushdb]
    }

    test {Test SWAPDB with database permissions} {
        r ACL SETUSER swapdb-user on nopass +@all ~* db=0,1
        $r2 auth swapdb-user password
        
        assert_equal "OK" [$r2 swapdb 0 1]
        
        catch {$r2 swapdb 0 2} e
        assert_match "*NOPERM*database*" $e
    }

    test {Test MOVE with database permissions} {
        r ACL SETUSER move-user on nopass +move +set ~* db=0,1
        $r2 auth move-user password
        $r2 set move-key value
        assert_equal "1" [$r2 move move-key 1]

        catch {$r2 move move-key 2} e
        assert_match "*NOPERM*database*" $e
    }

    test {Test COPY with database permissions} {
        r ACL SETUSER copy-user on nopass +copy +set +get +select ~* db=0,1
        $r2 auth copy-user password
        
        $r2 select 0
        $r2 set copy-key value
        
        assert_equal "1" [$r2 copy copy-key copy-key-dest DB 1]
        
        $r2 select 1
        assert_equal "value" [$r2 get copy-key-dest]
        
        $r2 select 0
        catch {$r2 copy copy-key copy-key-dest2 DB 2} e
        assert_match "*NOPERM*database*" $e
        
        r select 2
        assert_equal {} [r get copy-key-dest2]
        
        # cleanup
        r select 0
    }

    test {Test duplicate database IDs in db= are handled correctly} {
        r ACL SETUSER db-dup-user on nopass +@all ~* db=0,0,1,1,0
        $r2 auth db-dup-user password
        
        # Should work - duplicates should be silently ignored
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 select 1]
        
        catch {$r2 select 2} err
        assert_match "*NOPERM*database*" $err
        
        # Verify ACL LIST shows deduplicated list
        set acl_str [r ACL LIST]
        set user_line [lsearch -inline $acl_str "user db-dup-user*"]
        assert_match "*db=0,1*" $user_line
    }
    
    
    test {Test multiple selectors with overlapping database permissions} {
        r ACL SETUSER db-overlap on nopass (db=0,1 +@write +select ~write*) (db=1,2 +@read +select ~read*)
        $r2 auth db-overlap password
        
        # DB 0: only write selector
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 set writekey value]
        catch {$r2 get readkey} err
        assert_match "*NOPERM*command*" $err
        
        # DB 1: both selectors apply
        assert_equal "OK" [$r2 select 1]
        assert_equal "OK" [$r2 set writekey value]
        catch {$r2 set readkey bar} err
        assert_match "*NOPERM*key*" $err
        assert_equal {} [$r2 get readkey]
        
        # DB 2: only read selector
        assert_equal "OK" [$r2 select 2]
        catch {$r2 set writekey value} err
        assert_match "*NOPERM*command*" $err
        catch {$r2 set readkey bar} err
        assert_match "*NOPERM*command*" $err
        assert_equal {} [$r2 get readkey]
        
        # DB 3: no selector
        catch {$r2 select 3} err
        assert_match "*NOPERM*" $err
    }
    
    test {Test db= with leading/trailing commas} {
        catch {r ACL SETUSER db-comma-start db=,0,1} err
        assert_match "*Error*" $err
        
        catch {r ACL SETUSER db-comma-end db=0,1,} err
        assert_match "*Error*" $err
        
        catch {r ACL SETUSER db-comma-double db=0,,1} err
        assert_match "*Error*" $err
    }
    
    test {Test resetdbs clears explicit db list} {
        r ACL SETUSER db-reset-test on nopass +@all ~* db=0,1
        $r2 auth db-reset-test password
        
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 select 1]
        
        r ACL SETUSER db-reset-test resetdbs
        
        catch {$r2 select 0} err
        assert_match "*NOPERM*database*" $err
        catch {$r2 select 1} err
        assert_match "*NOPERM*database*" $err

        r ACL SETUSER db-reset-test db=0
        $r2 select 0
    }
    
    test {Test clearselectors removes db restrictions from selectors but not root} {
        r ACL SETUSER db-clear on nopass +select db=0,1 (db=2,3 +@all ~*)
        $r2 auth db-clear password
        
        r ACL SETUSER db-clear clearselectors
        
        # Root selector should still have db restrictions
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 select 1]
        
        catch {$r2 select 2} err
        assert_match "*NOPERM*" $err
    }
    
    test {Test ACL DRYRUN with database permissions} {
        r ACL SETUSER db-dryrun on nopass +@all ~* db=0,1
        
        assert_equal "OK" [r ACL DRYRUN db-dryrun SELECT 0]
        assert_equal "OK" [r ACL DRYRUN db-dryrun SELECT 1]
        
        assert_match "*has no permissions to access database*" [r ACL DRYRUN db-dryrun SELECT 2]
        assert_match "*has no permissions to access database*" [r ACL DRYRUN db-dryrun MOVE key 2]
        assert_match "*has no permissions to access database*" [r ACL DRYRUN db-dryrun SWAPDB 0 2]
        assert_equal "OK" [r ACL DRYRUN db-dryrun COPY key1 key2 DB 1]
        assert_match "*has no permissions to access database*" [r ACL DRYRUN db-dryrun COPY key1 key2 DB 2]
    }
    
    test {Test db= with maximum database ID} {
        set max_db [expr {[lindex [r config get databases] 1] - 1}]
        r ACL SETUSER db-max on nopass +@all ~* db=$max_db
        $r2 auth db-max password
        
        assert_equal "OK" [$r2 select $max_db]
        
        # Try one beyond max
        catch {$r2 select [expr {$max_db + 1}]} err
        # Should fail with out of range, not permission error
        assert_match "*out of range*" $err
    }
    
    test {Test consecutive database permissions - only last one should persist} {
        # User db-consec should only have db=2 permissions
        r ACL SETUSER db-consec on nopass +@all ~* db=0 db=1 db=2
        $r2 auth db-consec password
        
        catch {$r2 select 0} err
        assert_match "*NOPERM*database*" $err
        catch {$r2 select 1} err
        assert_match "*NOPERM*database*" $err
        assert_equal "OK" [$r2 select 2]
        catch {$r2 select 3} err
        assert_match "*NOPERM*database*" $err
        
        # User db-consec-2 should only have db=1 permissions
        r ACL SETUSER db-consec-2 on nopass +@all ~* db=0
        r ACL SETUSER db-consec-2 db=1
        $r2 auth db-consec-2 password
        
        catch {$r2 select 0} err
        assert_match "*NOPERM*database*" $err
        assert_equal "OK" [$r2 select 1]
        catch {$r2 select 2} err
        assert_match "*NOPERM*database*" $err

        # Cleanup
        $r2 auth default password
        $r2 select 0
    }

    test {Test commands without key/metadata access work in restricted databases} {
        r ACL SETUSER db-keyless-cmd on nopass db=15 +@all ~*
        $r2 auth db-keyless-cmd password
        
        # Commands that don't touch keys/metadata should work
        set ping_result [$r2 ping]
        assert {[string length $ping_result] > 0}
        
        set ping_msg_result [$r2 ping "hello"]
        assert {[string length $ping_msg_result] > 0}
        
        catch {$r2 acl getuser} err
        assert_match "*wrong number of arguments*" $err
        
        set user_info [$r2 acl getuser db-keyless-cmd]
        assert {[llength $user_info] > 0}
        
        set log_result [$r2 acl log 0]
        assert {[llength $log_result] >= 0}
        
        set info_result [$r2 info server]
        assert {[string length $info_result] > 0}
        
        # Commands that touch keys should fail with NOPERM database error
        catch {$r2 select 2} err
        assert_match "*NOPERM*database*" $err
        
        catch {$r2 get key} err
        assert_match "*NOPERM*database*" $err
        
        catch {$r2 getdel key} err
        assert_match "*NOPERM*database*" $err
        
        catch {$r2 set key value} err
        assert_match "*NOPERM*database*" $err
        
        catch {$r2 del key} err
        assert_match "*NOPERM*database*" $err
        
        catch {$r2 exists key} err
        assert_match "*NOPERM*database*" $err
        
        catch {$r2 flushdb} err
        assert_match "*NOPERM*database*" $err
        
        # But user can select to db=15 where they have permissions
        set select_result [$r2 select 15]
        assert {[string length $select_result] > 0}
        
        set set_result [$r2 set key value]
        assert {[string length $set_result] > 0}
        
        set get_result [$r2 get key]
        assert {[string length $get_result] > 0}
        
        catch {$r2 select 0} err
        assert_match "*NOPERM*database*" $err
    }
    
    test {Test WATCH command with no database access} {
        r ACL SETUSER watch-nodb-user on nopass +@all ~* db=1
        $r2 auth watch-nodb-user password
        
        catch {$r2 watch somekey} err
        assert_match "*NOPERM*database*" $err
        
        assert_equal "OK" [$r2 select 1]
        assert_equal "OK" [$r2 watch somekey]
        
        # cleanup
        $r2 auth default password
        $r2 select 0
    }
    
    test {Test keyspace command with no database access} {
        r ACL SETUSER exists-nodb-user on nopass +@all ~* db=1
        $r2 auth exists-nodb-user password
        
        catch {$r2 exists somekey} err
        assert_match "*NOPERM*database*" $err
        
        assert_equal "OK" [$r2 select 1]
        assert_equal "0" [$r2 exists somekey]

        # cleanup
        $r2 auth default password
        $r2 select 0
    }
    
    test {Test MULTI with SELECT to invalid database} {
        r ACL SETUSER db-invalid-select on nopass +@all ~* db=0,1
        $r2 auth db-invalid-select password
        
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 multi]
        assert_equal "QUEUED" [$r2 set key1 value1]
        
        catch {$r2 select 2} err
        assert_match "*NOPERM*database*" $err
        
        catch {$r2 exec} err
        assert_match "*EXECABORT*" $err
        
        assert_equal {} [$r2 get key1]
    }
    
    test {Test WATCH with MULTI and SELECT - keys modified} {
        r ACL SETUSER db-watch-select on nopass +@all ~* db=0,1
        $r2 auth db-watch-select password
        
        assert_equal "OK" [$r2 select 0]
        $r2 set watchkey original
        assert_equal "OK" [$r2 watch watchkey]
        
        assert_equal "OK" [$r2 multi]
        assert_equal "QUEUED" [$r2 select 1]
        assert_equal "QUEUED" [$r2 set key1 value1]
        
        r select 0
        r set watchkey modified
        
        set result [$r2 exec]
        assert_equal 0 [llength $result]
        
        assert_equal {} [$r2 get key1]
    }
    
    test {Test WATCH with MULTI and SELECT - no modification} {
        r ACL SETUSER db-watch-nomod on nopass +@all ~* db=0,1
        $r2 auth db-watch-nomod password
        
        assert_equal "OK" [$r2 select 0]
        $r2 set watchkey original
        assert_equal "OK" [$r2 watch watchkey]
        
        assert_equal "OK" [$r2 multi]
        assert_equal "QUEUED" [$r2 select 1]
        assert_equal "QUEUED" [$r2 set key1 value1]
        
        # No modification of watched key
        
        set result [$r2 exec]
        assert_equal 2 [llength $result]
        
        assert_equal "value1" [$r2 get key1]
    }
    
    test {Test SORT with BY/GET patterns in MULTI after SELECT} {
        r ACL SETUSER db-sort-multi on nopass +@all ~* db=0,1
        $r2 auth db-sort-multi password
        
        assert_equal "OK" [$r2 select 0]
        $r2 lpush mylist 1 2 3
        $r2 set weight_1 10
        $r2 set weight_2 20
        $r2 set weight_3 30
        
        assert_equal "OK" [$r2 multi]
        assert_equal "QUEUED" [$r2 select 1]
        
        assert_equal "QUEUED" [$r2 sort mylist by weight_*]
        
        set result [$r2 exec]
        assert_equal 2 [llength $result]

        # Cleanup
        $r2 select 0
        $r2 del mylist weight_1 weight_2 weight_3
    }
    
    test {Test DISCARD after SELECT in MULTI} {
        r ACL SETUSER db-discard on nopass +@all ~* db=0,1
        $r2 auth db-discard password
        
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 multi]
        
        assert_equal "QUEUED" [$r2 set key1 value1]
        assert_equal "QUEUED" [$r2 select 1]
        assert_equal "QUEUED" [$r2 set key2 value2]
        
        assert_equal "OK" [$r2 discard]
        
        $r2 set testkey testvalue
        assert_equal "testvalue" [$r2 get testkey]
        
        assert_equal {} [$r2 get key1]
        $r2 select 1
        assert_equal {} [$r2 get key2]
        
        # Cleanup
        $r2 select 0
        $r2 del testkey
    }
    
    test {Test MULTI with multiple SELECT commands} {
        r ACL SETUSER db-multi-select on nopass +@all ~* db=0,1,2
        $r2 auth db-multi-select password
        
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 multi]
        
        assert_equal "QUEUED" [$r2 set key0 value0]
        assert_equal "QUEUED" [$r2 select 1]
        assert_equal "QUEUED" [$r2 set key1 value1]
        assert_equal "QUEUED" [$r2 select 2]
        assert_equal "QUEUED" [$r2 set key2 value2]
        assert_equal "QUEUED" [$r2 select 0]
        assert_equal "QUEUED" [$r2 get key0]
        
        set result [$r2 exec]
        assert_equal 7 [llength $result]
        
        assert_equal "value0" [$r2 get key0]
        $r2 select 1
        assert_equal "value1" [$r2 get key1]
        $r2 select 2
        assert_equal "value2" [$r2 get key2]
        
        # Cleanup
        $r2 select 0
        $r2 del key0 finalkey
        $r2 select 1
        $r2 del key1
        $r2 select 2
        $r2 del key2
        $r2 select 0
    }
    
    test {Test MULTI with SELECT and ACL permission changes between queue and exec} {
        r ACL SETUSER db-acl-change on nopass +@all ~* db=0,1
        $r2 auth db-acl-change password
        
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 multi]
        assert_equal "QUEUED" [$r2 set key1 value1]
        assert_equal "QUEUED" [$r2 select 1]
        assert_equal "QUEUED" [$r2 set key2 value2]
        
        r ACL SETUSER db-acl-change db=0
        
        catch {$r2 exec} err
        assert_match "*NOPERM*" $err
        
        # Cleanup
        r ACL SETUSER db-acl-change db=0,1
    }

    test {Test EVAL with database permissions} {
        r ACL SETUSER eval-db-user on nopass +@all ~* db=0,1
        $r2 auth eval-db-user password
        
        assert_equal "OK" [$r2 select 0]
        assert_equal "OK" [$r2 select 1]
        
        catch {$r2 eval {server.call('SELECT', '2')} 0} err
        assert_match "*ACL failure in script*No permissions to access database*" $err
        
        $r2 select 0
        set result [$r2 eval {server.call('SELECT', '0'); return server.call('SET', 'key', 'value')} 0]
        assert_equal "OK" $result
        
        assert_equal "value" [$r2 get key]
        
        # Cleanup
        $r2 del key
    }
    
    test {Test EVALSHA with database permissions} {
        r ACL SETUSER evalsha-db-user on nopass +@all ~* db=0,1
        $r2 auth evalsha-db-user password
        
        assert_equal "OK" [$r2 select 0]
        
        set sha [r script load {server.call('SELECT', '2'); return 'OK'}]
        
        catch {$r2 evalsha $sha 0} err
        assert_match "*ACL failure in script*No permissions to access database*" $err
        
        set sha_ok [r script load {server.call('SELECT', '1'); return server.call('SET', 'key2', 'value2')}]
        
        set result [$r2 evalsha $sha_ok 0]
        assert_equal "OK" $result
        
        $r2 select 1
        assert_equal "value2" [$r2 get key2]
        
        # Cleanup
        $r2 del key2
        $r2 select 0
    }
    
    test {Test EVAL_RO with database permissions} {
        r ACL SETUSER eval-ro-db-user on nopass +@all ~* db=0,1
        $r2 auth eval-ro-db-user password
        
        assert_equal "OK" [$r2 select 0]
        r set testkey testvalue
        
        catch {$r2 eval_ro {server.call('SELECT', '2')} 0} err
        assert_match "*ACL failure in script*No permissions to access database*" $err
        
        set result [$r2 eval_ro {server.call('SELECT', '0'); return server.call('GET', 'testkey')} 0]
        assert_equal "testvalue" $result
        
        # Cleanup
        r del testkey
    }
    
    test {Test EVALSHA_RO with database permissions} {
        r ACL SETUSER evalsha-ro-db-user on nopass +@all ~* db=0,1
        $r2 auth evalsha-ro-db-user password
        
        assert_equal "OK" [$r2 select 0]
        r set rokey rovalue
        
        set sha [r script load {server.call('SELECT', '3'); return server.call('GET', 'rokey')}]
        
        catch {$r2 evalsha_ro $sha 0} err
        assert_match "*ACL failure in script*No permissions to access database*" $err
        
        set sha_ok [r script load {server.call('SELECT', '1'); return 'allowed'}]
        
        set result [$r2 evalsha_ro $sha_ok 0]
        assert_equal "allowed" $result
        
        # Cleanup
        r del rokey
    }
    
    test {Test FCALL with database permissions} {
        r ACL SETUSER fcall-db-user on nopass +@all ~* db=0,1
        $r2 auth fcall-db-user password
        
        assert_equal "OK" [$r2 select 0]
        
        r function load {#!lua name=dbtest
            server.register_function('select_restricted', function(keys, args)
                server.call('SELECT', '2')
                return 'OK'
            end)
        }
        
        catch {$r2 fcall select_restricted 0} err
        assert_match "*ACL failure in script*No permissions to access database*" $err
        
        r function load replace {#!lua name=dbtest
            server.register_function('select_allowed', function(keys, args)
                server.call('SELECT', '1')
                return server.call('SET', 'fcallkey', 'fcallvalue')
            end)
        }
        
        set result [$r2 fcall select_allowed 0]
        assert_equal "OK" $result
        
        $r2 select 1
        assert_equal "fcallvalue" [$r2 get fcallkey]
        
        # Cleanup
        $r2 del fcallkey
        $r2 select 0
        r function delete dbtest
    }
    
    test {Test FCALL_RO with database permissions} {
        r ACL SETUSER fcall-ro-db-user on nopass +@all ~* db=0,1
        $r2 auth fcall-ro-db-user password
        
        assert_equal "OK" [$r2 select 0]
        r set fcallrokey fcallrovalue
        
        r function load {#!lua name=dbtestro
            server.register_function{
                function_name='read_restricted',
                callback=function(keys, args)
                    server.call('SELECT', '3')
                    return server.call('GET', 'fcallrokey')
                end,
                flags={'no-writes'}
            }
        }
        
        catch {$r2 fcall_ro read_restricted 0} err
        assert_match "*ACL failure in script*No permissions to access database*" $err
        
        r function load replace {#!lua name=dbtestro
            server.register_function{
                function_name='read_allowed',
                callback=function(keys, args)
                    server.call('SELECT', '0')
                    return server.call('GET', 'fcallrokey')
                end,
                flags={'no-writes'}
            }
        }
        
        set result [$r2 fcall_ro read_allowed 0]
        assert_equal "fcallrovalue" $result
        
        # Cleanup
        r del fcallrokey
        r function delete dbtestro
    }
    
    test {Test EVAL with multiple database switches in script} {
        r ACL SETUSER eval-multi-db on nopass +@all ~* db=0,1,2
        $r2 auth eval-multi-db password
        
        assert_equal "OK" [$r2 select 0]
        
        set result [$r2 eval {
            server.call('SELECT', '0')
            server.call('SET', 'key0', 'val0')
            server.call('SELECT', '1')
            server.call('SET', 'key1', 'val1')
            server.call('SELECT', '2')
            server.call('SET', 'key2', 'val2')
            return 'OK'
        } 0]
        assert_equal "OK" $result
        
        $r2 select 0
        assert_equal "val0" [$r2 get key0]
        $r2 select 1
        assert_equal "val1" [$r2 get key1]
        $r2 select 2
        assert_equal "val2" [$r2 get key2]
        
        $r2 select 0
        catch {$r2 eval {
            server.call('SELECT', '0')
            server.call('SET', 'key0', 'newval')
            server.call('SELECT', '3')
            server.call('SET', 'key3', 'val3')
            return 'OK'
        } 0} err
        assert_match "*ACL failure in script*No permissions to access database*" $err
        
        # Cleanup
        $r2 select 0
        $r2 del key0
        $r2 select 1
        $r2 del key1
        $r2 select 2
        $r2 del key2
        $r2 select 0
    }

    test {ACL stats for invalid database accesses} {
        set current_auth_failures [s acl_access_denied_auth]
        set current_invalid_cmd_accesses [s acl_access_denied_cmd]
        set current_invalid_key_accesses [s acl_access_denied_key]
        set current_invalid_channel_accesses [s acl_access_denied_channel]
        set current_invalid_db_accesses [s acl_access_denied_db]
        
        r ACL SETUSER invaliddbuser on nopass +@all ~* db=0,1
        $r2 auth invaliddbuser password
        
        catch {$r2 select 2} err
        assert_match "*NOPERM*database*" $err
        
        catch {$r2 select 3} err
        assert_match "*NOPERM*database*" $err

        catch {$r2 select 4} err
        assert_match "*NOPERM*database*" $err
        
        r AUTH default ""
        
        # Verify the counter increased by 3
        assert {[s acl_access_denied_auth] eq $current_auth_failures}
        assert {[s acl_access_denied_cmd] eq $current_invalid_cmd_accesses}
        assert {[s acl_access_denied_key] eq $current_invalid_key_accesses}
        assert {[s acl_access_denied_channel] eq $current_invalid_channel_accesses}
        assert {[s acl_access_denied_db] eq [expr $current_invalid_db_accesses + 3]}
        
        # Cleanup
        r ACL deluser invaliddbuser
    }

    $r2 close
}
