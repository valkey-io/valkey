set testmodule [file normalize tests/modules/unloadrefuse.so]

start_server {tags {"modules external:skip"}} {
    r module load $testmodule

    test {MODULE UNLOAD reports a module whose OnUnload refuses} {
        catch {r module unload unloadrefuse} e
        assert_match {*operation not possible*} $e
        assert_match {*unloadrefuse*} [r module list]
    }

    test {Shutdown logs a module whose OnUnload refuses with a message} {
        set from [count_log_lines 0]
        catch {r shutdown nosave}
        wait_for_log_messages 0 {"*ready to exit, bye bye*"} $from 100 10
        verify_log_message 0 "*Module unloadrefuse OnUnload failed. Unload canceled.*" $from
        verify_log_message 0 "*Failed to unload module unloadrefuse: operation not possible.*" $from
    }
}
