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

# Redefine 'return' to capture stacktraces
rename return orig_return
proc return {args} {
    set opts {}
    set i 0
    while {$i < [llength $args]} {
        set arg [lindex $args $i]
        switch -glob -- $arg {
            -code - -errorcode - -errorinfo - -errorstack - -level - -options {
                dict set opts $arg [lindex $args $i+1]
                incr i 2
            }
            -- {
                incr i
                break
            }
            default {
                break
            }
        }
    }

    # Intercept errors and capture the stacktrace, unless it's a re-raise
    if {[dict exists $opts -code] && ![dict exists $opts -errorinfo]} {
        set code [dict get $opts -code]
        if {$code eq "error" || $code == 1} {
            set ::stacktrace [stacktrace 2]
            set ::stacktrace_err [lindex $args $i]
        }
    }

    # Bump -level by 1 to account for this wrapper's stack frame.
    if {[dict exists $opts -level]} {
        set level [dict get $opts -level]
        incr level
        set idx [lsearch -exact $args -level]
        set args [lreplace $args $idx [expr {$idx+1}] -level $level]
    } else {
        set args [linsert $args 0 -level 2]
    }
    orig_return {*}$args
}

# Redefine 'error' to capture stacktraces
rename error orig_error
proc error {msg {errorinfo ""} {errorcode ""}} {
    if {$errorinfo eq ""} {
        # Fresh error - use return -code error to trigger stacktrace capture
        return -code error $msg
    } else {
        # Re-raise - pass through with errorinfo
        return -code error -errorinfo $errorinfo -errorcode $errorcode $msg
    }
}
