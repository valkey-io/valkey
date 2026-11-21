set testmodule [file normalize tests/modules/datatype.so]
set configmodule [file normalize tests/modules/moduleconfigs.so]

start_server {tags {"modules"}} {
    test {MODULE LOADEX config failure on a module that cannot unload does not crash} {
        # The data type makes the module unloadable, so the LOADEX cleanup path
        # reaches the unload-refused branch after the queued configs were not
        # applied. Before the fix the refusal wrote the error message through a
        # NULL errmsg and killed the server.
        set pid [s process_id]
        catch {r module loadex $testmodule CONFIG moduleconfigs.mutable_bool no} e
        # Match the message this path sets (a parseLoadexArguments error never
        # reaches the cleanup path), so the test proves it took the unload branch.
        assert_match {*loadex configurations were not applied*} $e

        # The server must still answer, and the module must stay loaded because
        # the unload was refused. assert_not_equal on lsearch does not depend on
        # the module's position in MODULE LIST.
        assert_equal $pid [s process_id]
        assert_equal {PONG} [r ping]
        set modules [lmap x [r module list] {dict get $x name}]
        assert_not_equal [lsearch $modules datatype] -1

        # The failed LOADEX queued moduleconfigs.mutable_bool=no. Loading
        # moduleconfigs without CONFIG must not apply it, so the default yes
        # proves the queue did not carry over.
        assert_equal {OK} [r module loadex $configmodule]
        assert_equal [r config get moduleconfigs.mutable_bool] "moduleconfigs.mutable_bool yes"
        r module unload moduleconfigs

        # A valid LOADEX with CONFIG still works after the refused unload.
        assert_equal {OK} [r module loadex $configmodule CONFIG moduleconfigs.mutable_bool no]
        assert_equal [r config get moduleconfigs.mutable_bool] "moduleconfigs.mutable_bool no"
        assert_not_equal [lsearch [lmap x [r module list] {dict get $x name}] moduleconfigs] -1
    }
}
