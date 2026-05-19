set testmodule [file normalize tests/modules/infotest.so]

test {modules config rewrite} {

    start_server {tags {"modules"}} {
        r module load $testmodule

        set modules [lmap x [r module list] {dict get $x name}]
        assert_not_equal [lsearch $modules infotest] -1

        r config rewrite
        restart_server 0 true false

        set modules [lmap x [r module list] {dict get $x name}]
        assert_not_equal [lsearch $modules infotest] -1

        assert_equal {OK} [r module unload infotest]

        r config rewrite
        restart_server 0 true false

        set modules [lmap x [r module list] {dict get $x name}]
        assert_equal [lsearch $modules infotest] -1
    }
}

proc loadmodule_lines_from_config {} {
    set conf [exec cat [srv 0 config_file]]
    set lines [list]
    foreach line [split $conf "\n"] {
        if {[regexp {^\s*loadmodule\s+} $line]} { lappend lines $line }
    }
    return $lines
}

test {modules config rewrite preserves load order} {
    set m1 [file normalize tests/modules/infotest.so]
    set m2 [file normalize tests/modules/datatype.so]
    start_server {tags {"modules"}} {
        r module load $m1
        r module load $m2
        r config rewrite

        set lines [loadmodule_lines_from_config]
        # Static-module filter check: zero `loadmodule lua` lines.
        foreach line $lines {
            assert {![regexp {^\s*loadmodule\s+\S*lua} $line]}
        }
        set idx_info [lsearch -regexp $lines {infotest}]
        set idx_data [lsearch -regexp $lines {datatype}]
        assert {$idx_info >= 0 && $idx_data >= 0}
        assert {$idx_info < $idx_data}

        # Reload infotest — it should move to the tail of module_load_order.
        assert_equal {OK} [r module unload infotest]
        r module load $m1
        r config rewrite
        set lines [loadmodule_lines_from_config]
        set idx_info [lsearch -regexp $lines {infotest}]
        set idx_data [lsearch -regexp $lines {datatype}]
        assert {$idx_info >= 0 && $idx_data >= 0}
        assert {$idx_data < $idx_info}

        # Secondary smoke check (not an order assertion): the new server starts cleanly
        # with the rewritten conf. Do NOT use MODULE LIST for order assertions —
        # addReplyLoadedModules iterates the modules dict which is hash-seed-random.
        restart_server 0 true false
        set names [lmap x [r module list] {dict get $x name}]
        assert_not_equal [lsearch $names infotest] -1
        assert_not_equal [lsearch $names datatype] -1
    }
}
