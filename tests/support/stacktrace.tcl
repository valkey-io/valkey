# Captures the stacktrace at the current stack frame.
proc stacktrace {{skip 1}} {
    set lines {}
    set n [info frame]
    incr n -$skip
    set dir "[pwd]/"
    set dirlen [string length $dir]
    set prev_file ""
    set prev_line 0
    for {set i 1} {$i <= $n} {incr i} {
        set frame [info frame $i]
        set type [dict get $frame type]
        set str ""

        if {$type eq "eval" && [dict exists $frame line] && $prev_file ne ""} {
            # Eval frame from uplevel - compute absolute line from last anchor
            set abs_line [expr {$prev_line + [dict get $frame line] - 1}]
            if {[dict exists $frame cmd]} {
                regexp {^\S+} [dict get $frame cmd] cmd
            } else {
                set cmd "?"
            }
            append str " in $cmd at $prev_file:$abs_line"
            # Update anchor for further nested evals
            set prev_line $abs_line
            lappend lines $str
            continue
        } elseif {[dict exists $frame {proc}]} {
            set ctx [dict get $frame {proc}]
            if {[string match "::*" $ctx]} {
                set ctx [string range $ctx 2 end]
            }
            # Skip internal handler frames
            if {$ctx eq "unknown" || $ctx eq "error"} {
                continue
            }
            append str " in $ctx"
        } elseif {$type ne "source"} {
            continue
        } else {
            # Non-proc source frame: show first word of cmd
            if {[dict exists $frame cmd]} {
                regexp {^\S+} [dict get $frame cmd] cmd
            } else {
                set cmd "?"
            }
            append str " in $cmd"
        }

        if {$type eq {source}} {
            set file [dict get $frame file]
            if {[string length $file] >= $dirlen && [string equal -length $dirlen $dir $file]} {
                set file [string range $file $dirlen end]
            }
            set line [dict get $frame line]
            append str " at $file:$line"
            # Update anchor, but not for uplevel frames
            if {![dict exists $frame cmd] || ![string match "uplevel *" [dict get $frame cmd]]} {
                set prev_file $file
                set prev_line $line
            }
        } else {
            continue
        }

        lappend lines $str
    }
    return [join [lreverse $lines] \n]\n
}

# The last captured stacktrace
set ::stacktrace ""
set ::stacktrace_err ""

# Redefine 'return' to capture stacktraces (requires tailcall, Tcl 8.6+)
if {[info commands tailcall] ne ""} {
    rename return orig_return
    proc return {args} {
        if {[llength $args] % 2 == 1} {
            set opts [lrange $args 0 end-1]
            set result [lindex $args end]
        } else {
            set opts $args
            set result ""
        }

        # Intercept errors and capture the stacktrace, unless it's a re-raise
        if {[dict exists $opts -code] && ![dict exists $opts -errorinfo]} {
            set code [dict get $opts -code]
            if {$code eq "error" || $code == 1} {
                set ::stacktrace [stacktrace 2]
                set ::stacktrace_err $result
            }
        }

        tailcall orig_return {*}$args
    }
}

# Redefine 'error' to capture stacktraces
rename error orig_error
proc error {msg {errorinfo ""} {errorcode ""}} {
    if {$errorinfo eq ""} {
        set ::stacktrace [stacktrace 1]
        set ::stacktrace_err $msg
        orig_error $msg
    } else {
        # Re-raise - pass through with errorinfo
        orig_error $msg $errorinfo $errorcode
    }
}
