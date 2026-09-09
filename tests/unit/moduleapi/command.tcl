set testmodule [file normalize tests/modules/basics.so]

start_server {tags {"modules"}} {
    test {COMMAND caches response} {
        # Call COMMAND twice and verify we get the same commands
        set cmd1 [r command]
        set cmd2 [r command]
        assert_equal [llength $cmd1] [llength $cmd2]
    }

    test {COMMAND cache is invalidated on module load} {
        # Get initial command list
        set commands_before [r command]
        set count_before [llength $commands_before]
        
        # Verify module commands are not present
        set module_commands_found 0
        foreach cmd $commands_before {
            set name [lindex $cmd 0]
            if {[string match "test.*" $name]} {
                incr module_commands_found
            }
        }
        assert_equal $module_commands_found 0
        
        # Load module which should invalidate the cache
        r module load $testmodule
        
        # Get command list again (first call populates cache, second uses cache)
        r command
        set commands_after [r command]
        set count_after [llength $commands_after]
        
        # Verify we have more commands now
        assert {$count_after > $count_before}
        
        # Verify module commands are now present
        set module_commands_found 0
        foreach cmd $commands_after {
            set name [lindex $cmd 0]
            if {[string match "test.*" $name]} {
                incr module_commands_found
            }
        }
        assert {$module_commands_found > 0}
    }

    test {COMMAND INFO works with module commands} {
        # Verify we can get info about a specific module command
        set info [r command info test.basics]
        assert_equal [llength $info] 1
        assert_equal [lindex [lindex $info 0] 0] "test.basics"
    }

    test {COMMAND COUNT includes module commands} {
        set count_with_module [r command count]
        
        # Count should be greater than typical server commands
        assert {$count_with_module > 200}
    }

    test {COMMAND cache is invalidated on module unload} {
        # Get command list with module loaded
        set commands_before [r command]
        set count_before [llength $commands_before]
        
        # Verify module commands are present
        set module_commands_found 0
        foreach cmd $commands_before {
            set name [lindex $cmd 0]
            if {[string match "test.*" $name]} {
                incr module_commands_found
            }
        }
        assert {$module_commands_found > 0}
        
        # Unload module which should invalidate the cache
        r module unload test
        
        # Get command list again (first call populates cache, second uses cache)
        r command
        set commands_after [r command]
        set count_after [llength $commands_after]
        
        # Verify we have fewer commands now
        assert {$count_after < $count_before}
        
        # Verify module commands are no longer present
        set module_commands_found 0
        foreach cmd $commands_after {
            set name [lindex $cmd 0]
            if {[string match "test.*" $name]} {
                incr module_commands_found
            }
        }
        assert_equal $module_commands_found 0
    }

    test {COMMAND INFO returns empty for unloaded module commands} {
        # Verify we get empty/null response for module command after unload
        set info [r command info test.basics]
        # COMMAND INFO returns a list with one empty element for non-existent commands
        assert_equal [llength $info] 1
        assert_equal [lindex $info 0] {}
    }
}

start_server {tags {"modules"}} {
    test {COMMAND cache invalidation with multiple load/unload cycles} {
        # Get baseline
        set baseline_count [r command count]
        
        # Load module
        r module load $testmodule
        set count_loaded [r command count]
        assert {$count_loaded > $baseline_count}
        
        # Unload module
        r module unload test
        set count_unloaded [r command count]
        assert_equal $count_unloaded $baseline_count
        
        # Load again
        r module load $testmodule
        set count_loaded_again [r command count]
        assert_equal $count_loaded_again $count_loaded
        
        # Unload again
        r module unload test
        set count_unloaded_again [r command count]
        assert_equal $count_unloaded_again $baseline_count
    }
}

start_server {tags {"modules"}} {
    test {COMMAND cache works correctly with RESP2 and RESP3} {
        # Test with RESP2
        r hello 2
        set commands_resp2_before [r command]
        
        # Load module
        r module load $testmodule
        set commands_resp2_after [r command]
        assert {[llength $commands_resp2_after] > [llength $commands_resp2_before]}
        
        # Switch to RESP3 and verify cache works
        r hello 3
        set commands_resp3 [r command]
        # Both should have the same number of commands
        assert_equal [llength $commands_resp3] [llength $commands_resp2_after]
        
        # Unload module
        r module unload test
        
        # Verify both RESP versions see the change
        set commands_resp3_after [r command]
        assert {[llength $commands_resp3_after] < [llength $commands_resp3]}
        
        r hello 2
        set commands_resp2_final [r command]
        assert_equal [llength $commands_resp2_final] [llength $commands_resp3_after]
    }
}


