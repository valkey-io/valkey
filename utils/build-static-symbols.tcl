# Build a symbol table for static symbols of redis.c
# Useful to get stack traces on segfault without a debugger. See redis.c
# for more information.
#
# Copyright(C) 2009 Redis Ltd.

set fd [open redis.c]
set symlist {}
while {[gets $fd line] != -1} {
    if {[regexp {^static +[A-z0-9]+[ *]+([A-z0-9]*)\(} $line - sym]} {
        lappend symlist $sym
    }
}
set symlist [lsort -unique $symlist]
puts "static struct serverFunctionSym symsTable\[\] = {"
foreach sym $symlist {
    puts "{\"$sym\",(unsigned long)$sym},"
}
puts "{NULL,0}"
puts "};"

close $fd
