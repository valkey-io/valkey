set testmodule [file normalize tests/modules/helloscripting.so]

set HELLO_PROGRAM "#!hello name=mylib\nFUNCTION foo\nARGS 0\nRETURN\nFUNCTION bar\nCONSTI 432\nRETURN"

start_server {tags {"modules"}} {
    r module load $testmodule

    r function load $HELLO_PROGRAM

    test {Load script with invalid library name} {
        assert_error {ERR Library names can only contain letters, numbers, or underscores(_) and must be at least one character long} {r function load "#!hello name=my-lib\nFUNCTION foo\nARGS 0\nRETURN"}
    }

    test {Load script with existing library} {
        assert_error {ERR Library 'mylib' already exists} {r function load $HELLO_PROGRAM}
    }

    test {Load script with invalid engine} {
        assert_error {ERR Engine 'wasm' not found} {r function load "#!wasm name=mylib2\nFUNCTION foo\nARGS 0\nRETURN"}
    }

    test {Load script with no functions} {
        assert_error {ERR No functions registered} {r function load "#!hello name=mylib2\n"}
    }

    test {Load script with duplicate function} {
        assert_error {ERR Function foo already exists} {r function load "#!hello name=mylib2\nFUNCTION foo\nARGS 0\nRETURN"}
    }

    test {Load script with no metadata header} {
        assert_error {ERR Missing library metadata} {r function load "FUNCTION foo\nARGS 0\nRETURN"}
    }

    test {Load script with header without lib name} {
        assert_error {ERR Library name was not given} {r function load "#!hello \n"}
    }

    test {Load script with header with unknown param} {
        assert_error {ERR Invalid metadata value given: nme=mylib} {r function load "#!hello nme=mylib\n"}
    }

    test {Load script with header with lib name passed twice} {
        assert_error {ERR Invalid metadata value, name argument was given multiple times} {r function load "#!hello name=mylib2 name=mylib3\n"}
    }

    test {Load script with invalid function name} {
        assert_error {ERR Function names can only contain letters, numbers, or underscores(_) and must be at least one character long} {r function load "#!hello name=mylib2\nFUNCTION foo-bar\nARGS 0\nRETURN"}
    }

    test {Load script with duplicate function} {
        assert_error {ERR Function already exists in the library} {r function load "#!hello name=mylib2\nFUNCTION foo\nARGS 0\nRETURN\nFUNCTION foo\nARGS 0\nRETURN"}
    }

    test {Load script with syntax error} {
        assert_error {ERR Failed to parse instruction: 'SEND'} {r function load replace "#!hello name=mylib3\nFUNCTION foo\nARGS 0\nSEND"}
    }

    test {Call scripting engine function: calling foo works} {
        r fcall foo 0 134
    } {134}

    test {Call scripting engine function: calling bar works} {
        r fcall bar 0
    } {432}

    test {Replace function library and call functions} {
        set result [r function load replace "#!hello name=mylib\nFUNCTION foo\nARGS 0\nRETURN\nFUNCTION bar\nCONSTI 500\nRETURN"]
        assert_equal $result "mylib"

        set result [r fcall foo 0 132]
        assert_equal $result 132

        set result [r fcall bar 0]
        assert_equal $result 500
    }

    test {List scripting engine functions} {
        r function load replace "#!hello name=mylib\nFUNCTION foobar\nARGS 0\nRETURN"
        r function list
    } {{library_name mylib engine HELLO functions {{name foobar description {} flags {}}}}}

    test {Load a second library and call a function} {
        r function load "#!hello name=mylib2\nFUNCTION getarg\nARGS 0\nRETURN"
        set result [r fcall getarg 0 456]
        assert_equal $result 456
    }

    test {Delete all libraries and functions} {
        set result [r function flush]
        assert_equal $result {OK}
        r function list
    } {}

    test {Test the deletion of a single library} {
        r function load $HELLO_PROGRAM
        r function load "#!hello name=mylib2\nFUNCTION getarg\nARGS 0\nRETURN"

        set result [r function delete mylib]
        assert_equal $result {OK}

        set result [r fcall getarg 0 446]
        assert_equal $result 446
    }

    test {Test dump and restore function library} {
        r function load $HELLO_PROGRAM

        set result [r fcall bar 0]
        assert_equal $result 432

        set dump [r function dump]

        set result [r function flush]
        assert_equal $result {OK}

        set result [r function restore $dump]
        assert_equal $result {OK}

        set result [r fcall getarg 0 436]
        assert_equal $result 436

        set result [r fcall bar 0]
        assert_equal $result 432
    }

    test {Test function kill} {
        set rd [valkey_deferring_client]
        r config set busy-reply-threshold 10
        r function load REPLACE "#!hello name=mylib\nFUNCTION wait\nARGS 0\nSLEEP\nARGS 0\nRETURN"
        $rd fcall wait 0 100
        after 1000
        catch {r ping} e
        assert_match {BUSY*} $e
        assert_match {running_script {name wait command {fcall wait 0 100} duration_ms *} engines {*}} [r FUNCTION STATS]
        r function kill
        after 1000 ;
        assert_equal [r ping] "PONG"
        assert_error {ERR Script killed by user with FUNCTION KILL*} {$rd read}
        $rd ping
        assert_equal [$rd read] "PONG"
        $rd close
    }

    test {Test eval execution} {
        set result [r eval "#!hello\nFUNCTION foo\nARGS 0\nRETURN" 0 145]
        assert_equal $result 145
    }

    test {Test evalsha execution} {
        set sha [r script load "#!hello\nFUNCTION foo\nARGS 0\nRETURN"]
        set result [r evalsha $sha 0 167]
        assert_equal $result 167
    }

    test {Test script exists} {
        set sha [r script load "#!hello\nFUNCTION foo\nARGS 0\nRETURN"]
        set result [r script exists $sha]
        assert_equal $result 1
    }

    test {Test script flush sync} {
        set sha [r script load "#!hello\nFUNCTION foo\nARGS 0\nRETURN"]
        set result [r script exists $sha]
        assert_equal $result 1
        r script flush SYNC
        set result [r script exists $sha]
        assert_equal $result 0
    }

    test {Test script flush async} {
        set sha [r script load "#!hello\nFUNCTION foo\nARGS 0\nRETURN"]
        set result [r script exists $sha]
        assert_equal $result 1
        r script flush ASYNC
        set result [r script exists $sha]
        assert_equal $result 0
    }

    test {Test HELLO debugger} {
        r script debug sync hello
        set ret [r eval "#!hello\nFUNCTION foo\nARGS 0\nRETURN" 0 167]
        assert_equal {{>>>   0: ARGS 0}} $ret
        set cmd "*1\r\n\$4\r\nstep\r\n"
        r write $cmd
        r flush
        set ret [r read]
        assert_equal {{>>>   1: RETURN}} $ret
        set cmd "*1\r\n\$5\r\nstack\r\n"
        r write $cmd
        r flush
        set ret [r read]
        assert_equal {{Stack contents:} {top -> [0] 167}} $ret
        set cmd "*1\r\n\$1\r\nc\r\n"
        r write $cmd
        r flush
        set ret [r read]
        assert_equal {<endsession>} $ret
        r script debug off
        reconnect
        assert_equal [r ping] {PONG}
    }

    test {Test INFO scriptingengines section} {
        # Get the scripting engines info section
        set info [r info scriptingengines]

        # Verify the section header exists
        assert_match "*# Scripting Engines*" $info

        # Verify we have exactly 2 engines (LUA + HELLO)
        assert_match "*engines_count:*" $info
        regexp {engines_count:([0-9]+)} $info -> engines_count
        assert_equal $engines_count 2

        # Verify memory fields exist and are non-negative numbers
        assert_match "*engines_total_used_memory:*" $info
        assert_match "*engines_total_memory_overhead:*" $info
        regexp {engines_total_used_memory:([0-9]+)} $info -> total_memory
        regexp {engines_total_memory_overhead:([0-9]+)} $info -> total_overhead
        assert {$total_memory >= 0}
        assert {$total_overhead >= 0}

        # Verify individual engine information exists
        assert_match "*engine_0:*" $info
        assert_match "*engine_1:*" $info

        # Check that engines have proper format including abi_version
        assert_match "*engine_*:name=*,module=*,abi_version=*,used_memory=*,memory_overhead=*" $info

        # Verify both LUA and HELLO engines are present
        assert_match "*name=LUA*" $info
        assert_match "*name=HELLO*" $info

        # Verify LUA is built-in and HELLO is from module
        assert_match "*name=LUA,module=built-in*" $info
        assert_match "*name=HELLO,module=helloengine*" $info
    }

    test {Unload scripting engine module} {
        set result [r module unload helloengine]
        assert_equal $result "OK"
    }

    test {Load scripting engine in version before function env reset} {
        r module load $testmodule 2
        r function load $HELLO_PROGRAM
        set result [r fcall foo 0 123]
        assert_equal $result 123
        set result [r function flush async]
        assert_equal $result {OK}
        assert_error {ERR Function not found} {r fcall foo 0 123}
        set result [r module unload helloengine]
        assert_equal $result "OK"
    }

    test {Load scripting engine in version before debugger support} {
        r module load $testmodule 3
        r function load $HELLO_PROGRAM
        set result [r fcall foo 0 123]
        assert_equal $result 123
        assert_error {ERR The scripting engine 'HELLO' does not support interactive script debugging} {r script debug sync hello}
        set result [r module unload helloengine]
        assert_equal $result "OK"
    }
}
