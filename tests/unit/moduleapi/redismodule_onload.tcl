set testmodule [file normalize tests/modules/hellovalkey.so]

start_server {tags {"modules external:skip"}} {
    test {module with RedisModule_OnLoad symbol exported via version script can be loaded} {
        if {$::tcl_platform(os) eq "Linux"} {
            set nm_out [exec nm -D $testmodule]
            assert_match {*RedisModule_OnLoad*} $nm_out
        }

        assert_equal {OK} [r module load $testmodule]
        verify_log_message 0 "*Legacy Redis Module*hellovalkey.so found*" 0
        assert_equal {OK} [r module unload hello]
    }
}
