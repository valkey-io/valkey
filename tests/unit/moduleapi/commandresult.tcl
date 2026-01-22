set testmodule [file normalize tests/modules/commandresult.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    # Helper to ensure cleanup between tests
    proc cleanup_callback {} {
        catch {r cmdresult.unregister}
        r cmdresult.reset
    }

    test {Module commandresult - Register callback with 'all' flag} {
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

        r cmdresult.unregister
    }

    test {Module commandresult - Register callback with 'failures' flag} {
        cleanup_callback
        r cmdresult.register failures

        # Execute successful and failing commands
        r cmdresult.success
        r ping
        r cmdresult.success
        catch {r cmdresult.fail} e
        catch {r cmdresult.fail} e

        # With failures-only, should only see 2 callbacks (the failures)
        set stats [r cmdresult.stats]
        assert_equal [dict get $stats failure_count] 2
        # Success count should be 0 since we're only tracking failures
        assert_equal [dict get $stats success_count] 0

        r cmdresult.unregister
    }

    test {Module commandresult - Callback tracks duration} {
        cleanup_callback
        r cmdresult.register all

        r cmdresult.success
        r ping

        set stats [r cmdresult.stats]
        # Duration should be > 0 microseconds
        assert {[dict get $stats total_duration_us] > 0}

        r cmdresult.unregister
    }

    test {Module commandresult - Callback tracks dirty keys} {
        cleanup_callback
        r cmdresult.register all

        # This command modifies a key
        r SET ss 3

        set stats [r cmdresult.stats]
        # Should have at least 1 dirty key
        assert {[dict get $stats total_dirty] >= 1}

        r cmdresult.unregister
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

        r cmdresult.unregister
    }

    test {Module commandresult - Client ID is captured} {
        cleanup_callback
        r cmdresult.register all

        r cmdresult.success

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]
        # Client ID should be a positive integer
        assert {[dict get $entry client_id] > 0}

        r cmdresult.unregister
    }

    test {Module commandresult - NOSELF flag with RM_Call} {
        cleanup_callback
        r cmdresult.register noself

        # This command calls PING via RM_Call
        # With NOSELF, the PING callback should be skipped
        r cmdresult.rmcall ping

        set stats [r cmdresult.stats]
        # Should see callback for cmdresult.rmcall itself, but not for ping
        # Note: After stats is read, we have 2 callbacks: rmcall (from before) + stats (just now)
        # But when we READ stats, it returns the count BEFORE stats callback fires
        assert_equal [dict get $stats total_callbacks] 1

        # Get the last 2 log entries - they will be: [0]=stats, [1]=rmcall (newest first)
        set log [r cmdresult.getlog 2]
        set rmcall_entry [lindex $log 1]
        # Should be cmdresult.rmcall, not ping
        assert {[dict get $rmcall_entry command] eq "cmdresult.rmcall"}

        r cmdresult.unregister
    }

    test {Module commandresult - Without NOSELF flag, RM_Call is tracked} {
        cleanup_callback
        r cmdresult.register all

        # This command calls PING via RM_Call
        # Without NOSELF, both cmdresult.rmcall and ping should be tracked
        r cmdresult.rmcall ping

        set stats [r cmdresult.stats]
        # Should see callbacks for both cmdresult.rmcall and ping
        assert {[dict get $stats total_callbacks] >= 2}

        r cmdresult.unregister
    }

    test {Module commandresult - Unregister callback} {
        cleanup_callback
        r cmdresult.register all

        r cmdresult.success
        r cmdresult.unregister

        # After unregister, new commands shouldn't trigger callbacks
        r cmdresult.success
        r ping

        set stats [r cmdresult.stats]
        # Should only have 1 callback (from before unregister)
        assert_equal [dict get $stats total_callbacks] 1

        # Trying to unregister again should fail
        catch {r cmdresult.unregister} err
        assert_match {*no callback registered*} $err
    }

    test {Module commandresult - Cannot register twice} {
        cleanup_callback
        r cmdresult.register all

        # Trying to register again should fail
        catch {r cmdresult.register all} err
        assert_match {*already registered*} $err

        r cmdresult.unregister
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

        set log [r cmdresult.getlog]
        assert_equal [llength $log] 0
    }

    test {Module commandresult - Invalid flag returns error} {
        cleanup_callback

        catch {r cmdresult.register invalid_flag} err
        assert_match {*invalid flags*} $err
    }

    test {Module commandresult - Failures+noself combination} {
        cleanup_callback
        r cmdresult.register failures+noself

        # Successful commands shouldn't trigger callback
        r cmdresult.success
        r ping

        # Failing command should trigger callback
        catch {r cmdresult.fail} e

        # RM_Call to a failing command - the inner cmdresult.fail is skipped (NOSELF),
        # but cmdresult.rmcall itself forwards the error so it counts as a failure too
        catch {r cmdresult.rmcall cmdresult.fail} e

        set stats [r cmdresult.stats]
        # Should see 2 callbacks: cmdresult.fail (direct) + cmdresult.rmcall (wrapper that forwards error)
        # The inner cmdresult.fail called via RM_Call is skipped due to NOSELF
        assert_equal [dict get $stats failure_count] 2
        assert_equal [dict get $stats success_count] 0

        r cmdresult.unregister
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

        r cmdresult.unregister
    }

    test {Module commandresult - Unload with active callback} {
        cleanup_callback
        r cmdresult.register all

        # Execute some commands to ensure callback is active
        r cmdresult.success
        r ping

        set stats [r cmdresult.stats]
        assert {[dict get $stats total_callbacks] >= 2}

        # Unload module while callback is still registered
        # This tests the moduleUnregisterCommandResultCallbacks cleanup path
        assert_equal {OK} [r module unload commandresult]

        # Reload module for remaining tests
        r module load $testmodule
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

        r cmdresult.unregister
    }

    test {Module commandresult - Empty callback list optimization} {
        cleanup_callback
        # No callback registered - this tests early return in moduleCallCommandResultCallbacks

        # Execute commands without any callbacks registered
        r cmdresult.success
        r ping

        # Verify no callbacks were fired
        set stats [r cmdresult.stats]
        assert_equal [dict get $stats total_callbacks] 0
    }

    test {Module commandresult - Callback registered during command execution} {
        cleanup_callback

        # Register callback
        r cmdresult.register all

        # The registration command itself should NOT trigger a callback
        # due to self-registration prevention (registered_at_cmd_count check)
        set stats [r cmdresult.stats]

        # Stats reads the counter DURING command execution, BEFORE the callback fires
        # So we see count=0 (register was skipped, stats hasn't fired yet)
        assert_equal [dict get $stats total_callbacks] 0

        # Now execute another command
        r ping

        # And read stats again
        set stats [r cmdresult.stats]

        # Should now see: 1 (previous stats fired after returning) + 1 (ping fired) = 2
        # The current stats command's callback hasn't fired yet
        assert_equal [dict get $stats total_callbacks] 2

        r cmdresult.unregister
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

        r cmdresult.unregister
    }

    test {Module commandresult - FAILURES_ONLY with all successes} {
        cleanup_callback
        r cmdresult.register failures

        # Execute only successful commands
        for {set i 0} {$i < 10} {incr i} {
            r ping
        }

        set stats [r cmdresult.stats]
        # With failures-only, no callbacks should fire for successes
        assert_equal [dict get $stats failure_count] 0
        assert_equal [dict get $stats success_count] 0

        r cmdresult.unregister
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

        r cmdresult.unregister
    }

    # Tests for new accessors: GetArgv, GetReplyProto, GetReplySize, CreateReply

    test {Module commandresult - GetArgv captures command arguments} {
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

        r cmdresult.unregister
    }

    test {Module commandresult - GetArgv captures multi-argument commands} {
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

        r cmdresult.unregister
    }

    test {Module commandresult - GetReplySize returns reply size} {
        cleanup_callback
        r cmdresult.register all

        # Execute a command that returns data
        r set testkey "hello world"
        r get testkey

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]

        # Reply size should be > 0 for GET response
        assert {[dict get $entry reply_size] > 0}

        r cmdresult.unregister
    }

    test {Module commandresult - GetReplyProto captures RESP protocol} {
        cleanup_callback
        r cmdresult.register all

        # Execute commands and check reply_proto
        r set testkey "hello"
        r get testkey

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]

        # reply_proto should contain RESP format (bulk string for GET result)
        set reply_proto [dict get $entry reply_proto]
        # RESP bulk string format: $<len>\r\n<data>\r\n
        assert_match {$5*hello*} $reply_proto

        r cmdresult.unregister
    }

    test {Module commandresult - GetReplyProto for integer response} {
        cleanup_callback
        r cmdresult.register all

        # INCR returns an integer
        r set counter 10
        r incr counter

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]

        # RESP integer format: :<value>\r\n
        set reply_proto [dict get $entry reply_proto]
        assert_match {:11*} $reply_proto

        r cmdresult.unregister
    }

    test {Module commandresult - GetReplyProto for error response} {
        cleanup_callback
        r cmdresult.register all

        # Force an error
        catch {r cmdresult.fail} e

        set log [r cmdresult.getlog 1]
        set entry [lindex $log 0]

        # RESP error format: -ERR <message>\r\n
        set reply_proto [dict get $entry reply_proto]
        assert_match {-ERR*} $reply_proto

        r cmdresult.unregister
    }

    test {Module commandresult - CreateReply parses string reply} {
        cleanup_callback
        r cmdresult.register all

        # Enable CreateReply testing
        r cmdresult.testreply on

        r set testkey "hello world"
        r get testkey

        # Get the parsed reply info
        set last_reply [r cmdresult.getlastreply]
        assert_equal [dict get $last_reply type] "string"
        assert_equal [dict get $last_reply string] "hello world"

        r cmdresult.testreply off
        r cmdresult.unregister
    }

    test {Module commandresult - CreateReply parses integer reply} {
        cleanup_callback
        r cmdresult.register all

        # Enable CreateReply testing
        r cmdresult.testreply on

        r set counter 42
        r incr counter

        # Get the parsed reply info
        set last_reply [r cmdresult.getlastreply]
        assert_equal [dict get $last_reply type] "integer"
        assert_equal [dict get $last_reply integer] 43

        r cmdresult.testreply off
        r cmdresult.unregister
    }

    test {Module commandresult - CreateReply parses error reply} {
        cleanup_callback
        r cmdresult.register all

        # Enable CreateReply testing
        r cmdresult.testreply on

        catch {r cmdresult.fail} e

        # Get the parsed reply info
        set last_reply [r cmdresult.getlastreply]
        assert_equal [dict get $last_reply type] "error"
        assert_match {*intentional failure*} [dict get $last_reply string]

        r cmdresult.testreply off
        r cmdresult.unregister
    }

    test {Module commandresult - CreateReply parses null reply} {
        cleanup_callback
        r cmdresult.register all

        # Enable CreateReply testing
        r cmdresult.testreply on

        # GET on non-existent key returns null
        r get nonexistent_key_12345

        # Get the parsed reply info
        set last_reply [r cmdresult.getlastreply]
        assert_equal [dict get $last_reply type] "null"

        r cmdresult.testreply off
        r cmdresult.unregister
    }

    test {Module commandresult - GetArgv with command rewriting} {
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

        r cmdresult.unregister
    }

    test {Module commandresult - Zero overhead when accessors not used} {
        cleanup_callback
        # This test verifies that simply registering a callback without
        # using the new accessors doesn't add significant overhead.
        # We can't directly measure overhead, but we verify the callbacks work.

        r cmdresult.register all

        # Run many commands without using reply accessors
        for {set i 0} {$i < 100} {incr i} {
            r ping
        }

        set stats [r cmdresult.stats]
        assert {[dict get $stats total_callbacks] >= 100}

        r cmdresult.unregister
    }

    test {Unload the module - commandresult} {
        catch {r cmdresult.unregister}
        assert_equal {OK} [r module unload commandresult]
    }
}
