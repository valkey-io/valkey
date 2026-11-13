set testmodule [file normalize tests/modules/datatype.so]

start_server {tags {"modules"}} {
    test {MODULE LOADEX config failure on a module that cannot unload does not crash} {
        # The data type makes the module unloadable, so the LOADEX cleanup path
        # reaches the unload-refused branch after the queued configs were not
        # applied. Before the fix the refusal wrote the error message through a
        # NULL errmsg and killed the server.
        catch {r module loadex $testmodule CONFIG foo bar} e
        assert_match {*ERR*} $e

        # The server must still answer, and the module must stay loaded because
        # the unload was refused.
        assert_equal {PONG} [r ping]
        assert_equal 0 [lsearch [lmap x [r module list] {dict get $x name}] datatype]
    }
}
