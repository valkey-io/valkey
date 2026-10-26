set testmodule [file normalize tests/modules/commandresult.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    # Helper to ensure cleanup between tests
    proc cleanup_callback {} {
        catch {r cmdresult.unsubscribe}
        r cmdresult.reset
    }

    test {Module commandresult - Subscribe to all command result events} {
        cleanup_callback
        r cmdresult.register all

        # Execute some commands
        r cmdresult.success
        r ping
        catch {r cmdresult.fail} e

        # Check stats
        set stats [r cmdresult.stats]
        assert {[dict get $stats total_callbacks] >= 3}
        assert {[dict get $stats success_count] >= 2}
        assert {[dict get $stats failure_count] >= 1}

        r cmdresult.unsubscribe
    }

    test {Module commandresult - Subscribe to success events only} {
        cleanup_callback
        r cmdresult.register success

        # Execute successful and failing commands
        r cmdresult.success
        r ping
        r cmdresult.success
        catch {r cmdresult.fail} e
        catch {r cmdresult.fail} e

        # With success-only subscription, only success events are received
        set stats [r cmdresult.stats]
        assert {[dict get $stats success_count] >= 3}
        # Failures should NOT be tracked since we only subscribed to success
        assert_equal [dict get $stats failure_count] 0

        r cmdresult.unsubscribe
    }

    test {Module commandresult - Subscribe to failure events only} {
        cleanup_callback
        r cmdresult.register failure

        # Execute successful and failing commands
        r cmdresult.success
        r ping
        r cmdresult.success
        catch {r cmdresult.fail} e
        catch {r cmdresult.fail} e

        # With failure-only subscription, only failure events are received
        set stats [r cmdresult.stats]
        assert_equal [dict get $stats failure_count] 2
        # Successes should NOT be tracked since we only subscribed to failure
        assert_equal [dict get $stats success_count] 0

        r cmdresult.unsubscribe
    }

    test {Module commandresult - Callback tracks duration} {
        cleanup_callback
        r cmdresult.register all

        r eval {local sum = 0; for i = 1, 100000 do sum = sum + i end; return sum} 0

        set stats [r cmdresult.stats]
        assert {[dict get $stats total_duration_us] > 0}

        r cmdresult.unsubscribe
    }

    test {Module commandresult - Callback tracks dirty keys} {
        cleanup_callback
        r cmdresult.register all

        # This command modifies a key
        r SET ss 3

        set stats [r cmdresult.stats]
        # Should have at least 1 dirty key
        assert {[dict get $stats total_dirty] >= 1}

        r cmdresult.unsubscribe
    }

    test {Module commandresult - Get command log} {
        cleanup_callback
        r cmdresult.register all

        r cmdresult.success
        catch {r cmdresult.fail} e
        r ping

        set log [r cmdresult.getlog 3]
        assert_equal [llength $log] 3

        # Check first entry (most recent - ping)
        set entry [lindex $log 0]
        assert {[dict get $entry command] eq "ping"}
        assert {[dict get $entry status] eq "success"}

        # Check second entry (cmdresult.fail)
        set entry [lindex $log 1]
        assert {[dict get $entry command] eq "cmdresult.fail"}
        assert {[dict get $entry status] eq "failure"}

        # Check third entry (cmdresult.success)
        set entry [lindex $log 2]
        assert {[dict get $entry command] eq "cmdresult.success"}
        assert {[dict get $entry status] eq "success"}

        r cmdresult.unsubscribe
    }

    test {Module commandresult - Client ID is captured} {
        cleanup_callback
        r cmdresult.register all

        r cmdresult.success

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]
        # Client ID should be a positive integer
        assert {[dict get $entry client_id] > 0}

        r cmdresult.unsubscribe
    }

    test {Module commandresult - is_module_client detection} {
        cleanup_callback
        r cmdresult.register all

        # Direct command - should NOT be from module client
        r ping

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]
        assert_equal [dict get $entry is_module_client] 0

        r cmdresult.unsubscribe
    }

    test {Module commandresult - RM_Call shows is_module_client=1} {
        cleanup_callback
        r cmdresult.register all

        # This command calls PING via RM_Call
        r cmdresult.rmcall ping

        set log [r cmdresult.getlog 2]
        # The inner ping should have is_module_client=1
        set ping_entry [lindex $log 1]
        assert {[dict get $ping_entry command] eq "ping"}
        assert_equal [dict get $ping_entry is_module_client] 1

        r cmdresult.unsubscribe
    }

    test {Module commandresult - Unsubscribe} {
        cleanup_callback
        r cmdresult.register all

        r cmdresult.success
        # Unsubscribe also triggers a callback before unsubscribing
        r cmdresult.unsubscribe

        # After unsubscribe, new commands shouldn't trigger callbacks
        r cmdresult.success
        r ping

        set stats [r cmdresult.stats]
        # Should have 2 callbacks (cmdresult.success + cmdresult.unsubscribe)
        assert_equal [dict get $stats total_callbacks] 2

        # Trying to unsubscribe again should fail
        catch {r cmdresult.unsubscribe} err
        assert_match {*not subscribed*} $err
    }

    test {Module commandresult - Cannot subscribe twice} {
        cleanup_callback
        r cmdresult.register all

        # Trying to subscribe again should fail
        catch {r cmdresult.register all} err
        assert_match {*already subscribed*} $err

        r cmdresult.unsubscribe
    }

    test {Module commandresult - Reset clears stats and log} {
        cleanup_callback
        r cmdresult.register all

        r cmdresult.success
        r ping
        catch {r cmdresult.fail} e

        # Verify we have stats
        set stats [r cmdresult.stats]
        assert {[dict get $stats total_callbacks] > 0}

        # Reset should clear everything
        cleanup_callback

        set stats [r cmdresult.stats]
        assert_equal [dict get $stats total_callbacks] 0
        assert_equal [dict get $stats success_count] 0
        assert_equal [dict get $stats failure_count] 0
        assert_equal [dict get $stats rejected_count] 0
        assert_equal [dict get $stats acl_denied_count] 0

        set log [r cmdresult.getlog]
        assert_equal [llength $log] 0
    }

    test {Module commandresult - Invalid mode returns error} {
        cleanup_callback

        catch {r cmdresult.register invalid_mode} err
        assert_match {*invalid mode*} $err
    }

    test {Module commandresult - Command name is captured correctly} {
        cleanup_callback
        r cmdresult.register all

        r cmdresult.success
        r set mykey myvalue
        r get mykey

        set log [r cmdresult.getlog 3]

        # Check that command names are correct
        set commands [list]
        foreach entry $log {
            lappend commands [dict get $entry command]
        }

        assert {[lsearch $commands "get"] >= 0}
        assert {[lsearch $commands "set"] >= 0}
        assert {[lsearch $commands "cmdresult.success"] >= 0}

        r cmdresult.unsubscribe
    }

    test {Module commandresult - Unload with active subscription} {
        cleanup_callback
        r cmdresult.register all

        # Execute some commands to ensure callback is active
        r cmdresult.success
        r ping

        set stats [r cmdresult.stats]
        assert {[dict get $stats total_callbacks] >= 2}

        # Unload module while subscription is still active.
        assert_equal {OK} [r module unload commandresult]

        # Unsubscribing after reload has no matching listener. It must not add
        # NULL callbacks that would be invoked by later command result events.
        r module load $testmodule
        catch {r cmdresult.unsubscribe} err
        assert_match {*not subscribed*} $err
        assert_equal {PONG} [r ping]
    }

    test {Module commandresult - Multiple callbacks from different operations} {
        cleanup_callback
        r cmdresult.register all

        # Test callbacks from various sources
        r set testkey testvalue ;# Built-in command
        r get testkey          ;# Built-in command
        r cmdresult.success    ;# Module command
        catch {r cmdresult.fail} e ;# Failing module command

        set stats [r cmdresult.stats]
        # Should have at least 4 callbacks
        assert {[dict get $stats total_callbacks] >= 4}
        assert {[dict get $stats success_count] >= 3}
        assert {[dict get $stats failure_count] >= 1}

        r cmdresult.unsubscribe
    }

    test {Module commandresult - Empty subscription optimization} {
        cleanup_callback
        # No subscription - this tests early return when no modules subscribed

        # Execute commands without any subscriptions
        r cmdresult.success
        r ping

        # Verify no callbacks were fired
        set stats [r cmdresult.stats]
        assert_equal [dict get $stats total_callbacks] 0
    }

    test {Module commandresult - Duration is always positive} {
        cleanup_callback
        r cmdresult.register all

        # Execute a command
        r cmdresult.success

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]

        # Duration should be >= 0 microseconds
        assert {[dict get $entry duration_us] >= 0}

        r cmdresult.unsubscribe
    }

    test {Module commandresult - RM_Call creates nested command execution} {
        cleanup_callback
        r cmdresult.register all

        # cmdresult.rmcall calls PING via RM_Call
        # Both the wrapper and inner command should be tracked
        r cmdresult.rmcall ping

        set stats [r cmdresult.stats]
        # Should see both cmdresult.rmcall and ping callbacks
        assert {[dict get $stats total_callbacks] >= 2}

        r cmdresult.unsubscribe
    }

    test {Module commandresult - argv captures command arguments} {
        cleanup_callback
        r cmdresult.register all

        # Execute a command with arguments
        r set mykey myvalue

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]

        # Check argv is captured
        set argv [dict get $entry argv]
        assert_equal [lindex $argv 0] "set"
        assert_equal [lindex $argv 1] "mykey"
        assert_equal [lindex $argv 2] "myvalue"

        r cmdresult.unsubscribe
    }

    test {Module commandresult - argv captures multi-argument commands} {
        cleanup_callback
        r cmdresult.register all

        # Execute MSET with multiple key-value pairs
        r mset key1 val1 key2 val2 key3 val3

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]

        set argv [dict get $entry argv]
        assert_equal [lindex $argv 0] "mset"
        assert_equal [lindex $argv 1] "key1"
        assert_equal [lindex $argv 2] "val1"
        assert_equal [lindex $argv 3] "key2"
        assert_equal [lindex $argv 4] "val2"

        r cmdresult.unsubscribe
    }

    test {Module commandresult - argv with command rewriting} {
        cleanup_callback
        r cmdresult.register all

        # EXPIRE gets rewritten to PEXPIREAT internally
        # But we should see original argv (EXPIRE)
        r set rewritekey "value"
        r expire rewritekey 100

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]

        set argv [dict get $entry argv]
        # Should see original command, not rewritten one
        assert_equal [string tolower [lindex $argv 0]] "expire"
        assert_equal [lindex $argv 1] "rewritekey"
        assert_equal [lindex $argv 2] "100"

        r cmdresult.unsubscribe
    }

    test {Module commandresult - High volume command tracking} {
        cleanup_callback
        r cmdresult.register all

        # Run many commands
        for {set i 0} {$i < 100} {incr i} {
            r ping
        }

        set stats [r cmdresult.stats]
        assert {[dict get $stats total_callbacks] >= 100}

        r cmdresult.unsubscribe
    }

    test {Module commandresult - acl_rejected: command not permitted (ACL_DENIED_CMD)} {
        cleanup_callback
        r cmdresult.register acl_rejected

        # Create a user with no command permissions and authenticate
        r acl setuser testuser_cmd on >testpass nocommands
        set rd [valkey_deferring_client]
        $rd auth testuser_cmd testpass
        $rd read
        $rd get somekey
        catch {$rd read} e
        $rd close

        set stats [r cmdresult.stats]
        assert {[dict get $stats acl_denied_count] >= 1}
        assert_equal [dict get $stats success_count] 0
        assert_equal [dict get $stats failure_count] 0
        assert_equal [dict get $stats rejected_count] 0

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]
        assert_equal [dict get $entry status] "acl_rejected"
        # VALKEYMODULE_ACL_LOG_CMD = 1
        assert_equal [dict get $entry subevent] 1
        assert_equal [dict get $entry rejection_context] ""

        r acl deluser testuser_cmd
        r cmdresult.unsubscribe
    }

    test {Module commandresult - acl_rejected: key pattern not permitted (ACL_DENIED_KEY)} {
        cleanup_callback
        r cmdresult.register acl_rejected

        # Create a user allowed to run GET but only on keys matching "allowed:*"
        r acl setuser testuser_key on >testpass allcommands ~allowed:* nopass
        set rd [valkey_deferring_client]
        $rd auth testuser_key testpass
        $rd read
        $rd get denied_key
        catch {$rd read} e
        $rd close

        set stats [r cmdresult.stats]
        assert {[dict get $stats acl_denied_count] >= 1}

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]
        assert_equal [dict get $entry status] "acl_rejected"
        # VALKEYMODULE_ACL_LOG_KEY = 2
        assert_equal [dict get $entry subevent] 2
        assert_equal [dict get $entry rejection_context] "denied_key"

        r acl deluser testuser_key
        r cmdresult.unsubscribe
    }

    test {Module commandresult - acl_rejected: channel not permitted (ACL_DENIED_CHANNEL)} {
        cleanup_callback
        r cmdresult.register acl_rejected

        # Create a user with allcommands but no pub/sub channel access
        r acl setuser testuser_chan on >testpass allcommands allkeys resetchannels nopass
        set rd [valkey_deferring_client]
        $rd auth testuser_chan testpass
        $rd read
        $rd subscribe secret_channel
        catch {$rd read} e
        $rd close

        set stats [r cmdresult.stats]
        assert {[dict get $stats acl_denied_count] >= 1}

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]
        assert_equal [dict get $entry status] "acl_rejected"
        # VALKEYMODULE_ACL_LOG_CHANNEL = 3
        assert_equal [dict get $entry subevent] 3
        assert_equal [dict get $entry rejection_context] "secret_channel"

        r acl deluser testuser_chan
        r cmdresult.unsubscribe
    }

    test {Module commandresult - acl_rejected events not fired when not subscribed} {
        cleanup_callback
        r cmdresult.register failure

        r acl setuser testuser_nosub on >testpass nocommands nopass
        set rd [valkey_deferring_client]
        $rd auth testuser_nosub testpass
        $rd read
        $rd get somekey
        catch {$rd read} e
        $rd close

        set stats [r cmdresult.stats]
        assert_equal [dict get $stats acl_denied_count] 0
        assert_equal [dict get $stats rejected_count] 0

        r acl deluser testuser_nosub
        r cmdresult.unsubscribe
    }

    test {Module commandresult - acl_rejected: unauthenticated command (NOAUTH)} {
        cleanup_callback
        r cmdresult.register acl_rejected

        # Enable password so new connections require authentication
        r config set requirepass testpass

        # Open a raw unauthenticated connection and send a command without AUTH
        set rd [valkey_deferring_client_by_addr [srv 0 host] [srv 0 port]]
        $rd get somekey
        catch {$rd read} e
        $rd close

        # Restore: the existing r session stays authenticated; just clear the password
        r config set requirepass ""

        set stats [r cmdresult.stats]
        assert {[dict get $stats acl_denied_count] >= 1}

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]
        assert_equal [dict get $entry status] "acl_rejected"
        # VALKEYMODULE_ACL_LOG_AUTH = 0
        assert_equal [dict get $entry subevent] 0
        assert_equal [dict get $entry rejection_context] ""

        r cmdresult.unsubscribe
    }

    test {Module commandresult - Reset clears acl_denied_count} {
        cleanup_callback
        r cmdresult.register acl_rejected

        r acl setuser testuser_reset on >testpass nocommands nopass
        set rd [valkey_deferring_client]
        $rd auth testuser_reset testpass
        $rd read
        $rd get somekey
        catch {$rd read} e
        $rd close

        set stats [r cmdresult.stats]
        assert {[dict get $stats acl_denied_count] >= 1}

        r cmdresult.reset
        set stats [r cmdresult.stats]
        assert_equal [dict get $stats acl_denied_count] 0
        assert_equal [dict get $stats rejected_count] 0
        assert_equal [dict get $stats total_callbacks] 0

        r acl deluser testuser_reset
        r cmdresult.unsubscribe
    }

    test {Module commandresult - rejected: unknown command (UNKNOWNCMD)} {
        cleanup_callback
        r cmdresult.register rejected

        catch {r thisdoesnotexist} e

        set stats [r cmdresult.stats]
        assert {[dict get $stats rejected_count] >= 1}

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]
        assert_equal [dict get $entry status] "rejected"
        assert_match {*unknown*command*} [string tolower [dict get $entry rejection_context]]

        r cmdresult.unsubscribe
    }

    test {Module commandresult - rejected: wrong number of arguments (WRONGARITY)} {
        cleanup_callback
        r cmdresult.register rejected

        catch {r set} e

        set stats [r cmdresult.stats]
        assert {[dict get $stats rejected_count] >= 1}

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]
        assert_equal [dict get $entry status] "rejected"
        assert_equal [dict get $entry command] "set"
        assert_match {*wrong*number*arguments*} [string tolower [dict get $entry rejection_context]]

        r cmdresult.unsubscribe
    }

    test {Module commandresult - rejected: command not allowed in MULTI (NOMULTI)} {
        cleanup_callback
        r cmdresult.register rejected

        r multi
        catch {r multi} e
        r discard

        set stats [r cmdresult.stats]
        assert {[dict get $stats rejected_count] >= 1}

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]
        assert_equal [dict get $entry status] "rejected"
        assert_equal [dict get $entry command] "multi"
        assert_match {*not allowed inside a transaction*} [string tolower [dict get $entry rejection_context]]

        r cmdresult.unsubscribe
    }

    test {Module commandresult - rejected: command not allowed in Pub/Sub context (PUBSUB)} {
        cleanup_callback
        if {$::force_resp3} {
            skip "RESP3 Pub/Sub clients may issue arbitrary commands"
        }
        r cmdresult.register rejected

        set rd [valkey_deferring_client]
        $rd subscribe testchan
        $rd read
        $rd set foo bar
        catch {$rd read} e
        $rd unsubscribe testchan
        $rd read
        $rd close

        set stats [r cmdresult.stats]
        assert {[dict get $stats rejected_count] >= 1}

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]
        assert_equal [dict get $entry status] "rejected"
        assert_equal [dict get $entry command] "set"
        assert_match {*only*(p|s)subscribe*} [string tolower [dict get $entry rejection_context]]

        r cmdresult.unsubscribe
    }

    test {Module commandresult - rejected: not enough replicas (NOREPLICAS)} {
        cleanup_callback
        r cmdresult.register rejected

        r config set min-replicas-to-write 100

        catch {r set foo bar} e
        assert_match {*NOREPLICAS*} $e

        r config set min-replicas-to-write 0

        set stats [r cmdresult.stats]
        assert {[dict get $stats rejected_count] >= 1}

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]
        assert_equal [dict get $entry status] "rejected"
        assert_equal [dict get $entry command] "set"
        assert_match {*NOREPLICAS*} [dict get $entry rejection_context]

        r cmdresult.unsubscribe
    }

    test {Module commandresult - rejected: out of memory (OOM)} {
        cleanup_callback
        r cmdresult.register rejected

        r config set maxmemory 1
        r config set maxmemory-policy noeviction

        catch {r set oomkey oomval} e
        assert_match {*OOM*} $e

        r config set maxmemory 0

        set stats [r cmdresult.stats]
        assert {[dict get $stats rejected_count] >= 1}

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]
        assert_equal [dict get $entry status] "rejected"
        assert_match {*OOM*} [dict get $entry rejection_context]

        r cmdresult.unsubscribe
    }

    test {Unload the module - commandresult} {
        catch {r cmdresult.unsubscribe}
        assert_equal {OK} [r module unload commandresult]
    }
}
